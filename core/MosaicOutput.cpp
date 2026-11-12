#include "MosaicOutput.h"
#include "RuntimeContext.h"
#include "BrightnessAdjustment.h"
#include "OutputPlan.h"
#include "BigTiffWriter.h"
#include "DeepZoomWriter.h"
#include "ImageCache.h"
#include "JpgStreamWriter.h"
#include "PngStreamWriter.h"
#include "PreviewAccumulator.h"
#include "TileCompositor.h"
#include "WorkerPool.h"
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

namespace mosaicraft
{
namespace
{
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

uint64_t availableMemory()
{
#ifdef _WIN32
    MEMORYSTATUSEX memory{sizeof(memory)};
    return GlobalMemoryStatusEx(&memory) ? memory.ullAvailPhys : 0;
#elif defined(__APPLE__)
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t statistics{};
    const auto host = mach_host_self();
    vm_size_t pageSize = 0;
    const bool valid = host_page_size(host, &pageSize) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    // Inactive pages are reclaimable; do not count compressed or wired memory.
    return valid ? (static_cast<uint64_t>(statistics.free_count) + statistics.inactive_count) * pageSize : 0;
#else
    const long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? static_cast<uint64_t>(pages) * size : 0;
#endif
}
} // namespace

OutputBudget outputBudget(const MosaicEngine::Config &config, const detail::OutputPlan &plan,
                          uint64_t overlayReferenceBytes, uint64_t overlayWorkingBytes)
{
    const uint64_t width = plan.width, height = plan.height;
    const uint64_t rawBytes = width * height * 3;
    const uint64_t rowBytes = width * plan.tileHeight * 3;
    const int rgbRows = static_cast<int>(std::max<uint64_t>(1, std::min<uint64_t>(64, (1024 * 1024) / (width * 3))));
    const uint64_t conversionBytes = width * 3 * (plan.format == "png" ? rgbRows : 1);
    const cv::Size previewSize = config.generatePreview ? PreviewAccumulator::outputSize({plan.width, plan.height}) : cv::Size();
    const uint64_t previewBytes = config.generatePreview
        ? static_cast<uint64_t>(previewSize.area()) * 6 + width * 12 + previewSize.width * 36ULL : 0;
    // 两行预取、编码和预览缓冲可与素材缓存同时存活。
    const uint64_t fixed = ImageCache::kDefaultBudget + conversionBytes + 16 * 1024 * 1024 +
                           overlayReferenceBytes + overlayWorkingBytes + previewBytes;
    const auto maximum = std::numeric_limits<uint64_t>::max();
    const uint64_t streamBytes = rowBytes > (maximum - fixed) / 2 ? maximum : fixed + rowBytes * 2;
    const uint64_t batchBytes = rawBytes > (maximum - streamBytes) / 2 ? maximum : streamBytes + rawBytes * 2;
    const uint64_t memory = availableMemory();
    const bool stream = plan.format != "webp" && (config.writeMode == "stream" ||
        (config.writeMode == "auto" && (memory ? batchBytes > memory : rawBytes > 500ULL * 1024 * 1024)));
    return {rawBytes, rowBytes, batchBytes, streamBytes, rgbRows, stream};
}

static bool writeOutput(const MosaicEngine::Config &config, const std::string &requestedPath, int tilesX, int tilesY,
                        int requestedTileW, int requestedTileH, const std::function<const ImageRecord &(size_t)> &recordAt,
                        const std::vector<int> &selected, const std::function<void(cv::Mat &, double)> &adjustColor,
                        OutputStats &stats, const cv::Mat &target, RuntimeContext *runtime,
                        const detail::OutputPlan *prepared = nullptr)
{
    try
    {
        auto now = [&]() { return config.benchmark ? Clock::now() : Clock::time_point{}; };
        auto elapsed = [&](Clock::time_point start) { return config.benchmark ? milliseconds(start) : 0.0; };
        stats = {};
        if (runtime)
        {
            runtime->stage("writing_output");
        }
        if (config.generatePreview && (config.tiledOutput || config.deepZoom))
        {
            throw std::invalid_argument("--preview requires single-image output; incompatible with --tiled/--deepzoom");
        }
        detail::OutputPlan local;
        if (!prepared)
        {
            std::string error;
            const detail::OutputFormat format{detail::normalizeOutputFormat(config.outputFormat),
                                               config.formatExplicit ? "explicit" : "fallback"};
            if (!detail::makeOutputPlan(format, requestedPath, tilesX, tilesY, requestedTileW, requestedTileH,
                                        !config.tiledOutput && !config.deepZoom, local, error))
            {
                throw std::invalid_argument(error);
            }
        }
        const auto &plan = prepared ? *prepared : local;
        const int tileW = plan.tileWidth, tileH = plan.tileHeight;
        const int width = plan.width, height = plan.height;
        const auto &path = plan.path;
        const size_t count = static_cast<size_t>(tilesX) * tilesY;
        if (selected.size() != count)
        {
            throw std::invalid_argument("selection count does not match output grid");
        }
        if (!std::isfinite(config.overlayStrength) || config.overlayStrength < 0 || config.overlayStrength > 1)
        {
            throw std::invalid_argument("overlayStrength must be finite and between 0 and 1");
        }
        const unsigned workers = static_cast<unsigned>(
            std::min<size_t>(count, config.threads > 0 ? static_cast<unsigned>(config.threads)
                : std::min(8u, std::max(1u, std::thread::hardware_concurrency()))));
        std::unique_ptr<TileCompositor> compositor;
        if (config.overlayStrength > 0)
        {
            compositor = std::make_unique<TileCompositor>(target, tilesX, tilesY, cv::Size(tileW, tileH));
            stats.overlayReferenceBytes = target.total() * target.elemSize();
            stats.overlayWorkingBytes = workers * TileCompositor::workingBytes(cv::Size(tileW, tileH));
        }
        ImageCache cache(ImageCache::kDefaultBudget, config.benchmark);
        std::unordered_map<int, size_t> useCounts;
        for (size_t i = 0; i < count; ++i)
        {
            if (selected[i] >= 0)
            {
                ++useCounts[recordAt(i).id];
            }
        }
        cache.setUseCounts(std::move(useCounts));
        auto finish = [&](size_t i)
        {
            if (selected[i] >= 0)
            {
                cache.finishUse(recordAt(i).id, tileW, tileH);
            }
        };
        WorkerPool pool(workers);
        std::atomic<int> failures{0};
        std::atomic<int64_t> loadingNs{0};
        std::atomic<int64_t> samplingNs{0}, blendingNs{0};
        auto finishOverlayStats = [&]()
        {
            stats.overlaySamplingMs = samplingNs.load() / 1000000.0;
            stats.overlayBlendMs = blendingNs.load() / 1000000.0;
        };
        auto load = [&](size_t i) -> std::shared_ptr<const cv::Mat>
        {
            if (selected[i] < 0)
            {
                ++failures;
                return {};
            }
            const auto loadStart = now();
            auto image = cache.getShared(recordAt(i).id, recordAt(i).filePath, tileW, tileH);
            if (config.benchmark)
            {
                loadingNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - loadStart).count();
            }
            if (!image)
            {
                ++failures;
                return {};
            }
            if (config.colorAdjust || compositor)
            {
                // 同一素材在不同位置有不同参考色，所有调整共享这一份独立副本。
                cv::Mat adjusted = image->clone();
                if (config.colorAdjust)
                {
                    if (config.seed)
                    {
                        detail::adjustBrightnessAt(adjusted, config.colorStrength, *config.seed, i);
                    }
                    else
                    {
                        adjustColor(adjusted, config.colorStrength);
                    }
                }
                if (compositor)
                {
                    cv::Mat reference;
                    const auto sampleStart = now();
                    compositor->sample(i, reference);
                    const auto blendStart = now();
                    cv::addWeighted(adjusted, 1.0 - config.overlayStrength, reference, config.overlayStrength,
                                    0.0, adjusted);
                    if (config.benchmark)
                    {
                        samplingNs += std::chrono::duration_cast<std::chrono::nanoseconds>(blendStart - sampleStart).count();
                        blendingNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - blendStart).count();
                    }
                }
                return std::make_shared<const cv::Mat>(std::move(adjusted));
            }
            return image;
        };
        stats.path = path;
        if (config.tiledOutput)
        {
            const auto start = now();
            std::atomic<size_t> completed{0};
            const auto folder = path + "_files/0";
            std::filesystem::create_directories(u8path(folder));
            pool.run(count,
                     [&](size_t i)
                     {
                         auto image = load(i);
                         if (image)
                         {
                             const auto tilePath =
                                 folder + "/" + std::to_string(i % tilesX) + "_" + std::to_string(i / tilesX) + ".jpg";
                             if (!imwriteUnicode(tilePath, *image, {cv::IMWRITE_JPEG_QUALITY, config.jpegQuality}))
                             {
                                 throw std::runtime_error("tile write failed: " + tilePath);
                             }
                         }
                         finish(i);
                         if (runtime)
                         {
                             runtime->advance("tiles", ++completed, count, "tiles");
                         }
                     });
            if (runtime)
            {
                runtime->advance("tiles", count, count, "tiles", true);
            }
            if (config.deepZoom)
            {
                if (runtime)
                {
                    runtime->stage("building_pyramid");
                }
                if (!DeepZoomWriter::buildPyramid(folder, tileW, tileH, tilesX, tilesY, config.jpegQuality, runtime))
                {
                    throw std::runtime_error("DeepZoom pyramid or manifest could not be written");
                }
            }
            stats.placementMs = elapsed(start);
            stats.cacheHits = cache.stats().hits;
            stats.cacheDecodes = cache.stats().decodes;
            stats.cachePeakBytes = cache.stats().peakBytes;
            stats.cacheSharedLoads = cache.stats().sharedLoads;
            stats.loadingMs = loadingNs.load() / 1000000.0;
            stats.failed = failures;
            stats.matched = static_cast<int>(count) - stats.failed;
            finishOverlayStats();
            if (runtime)
            {
                runtime->output(width, height, config.deepZoom ? "deepzoom" : "tiled", stats.matched, stats.failed);
                runtime->artifact("tiles", "directory", path + "_files");
            }
            return true;
        }

        const auto &format = plan.format;
        const auto budget = outputBudget(config, plan, stats.overlayReferenceBytes, stats.overlayWorkingBytes);
        const int rgbRows = budget.rgbRows;
        const bool stream = budget.stream;
        const bool rgbStream = stream && format == "png";
        std::cout << "  output: " << (stream ? "stream (two tile rows)" : "batch") << std::endl;

        std::unique_ptr<PreviewAccumulator> preview;
        auto previewRows = [&](const cv::Mat &rows, int offset)
        {
            if (runtime)
            {
                runtime->advance("output_rows", offset + rows.rows, height, "rows", offset + rows.rows == height);
            }
            if (config.generatePreview)
            {
                const auto start = now();
                if (!preview)
                {
                    preview = std::make_unique<PreviewAccumulator>(cv::Size(width, height));
                }
                preview->append(rows, offset);
                stats.previewMs += elapsed(start);
            }
        };

        auto fill = [&](cv::Mat &destination, int tileRow)
        {
            destination.setTo(cv::Scalar(64, 64, 64));
            pool.run(tilesX,
                     [&](size_t x)
                     {
                         auto image = load(static_cast<size_t>(tileRow) * tilesX + x);
                         if (image)
                         {
                             auto region = destination(cv::Rect(static_cast<int>(x) * tileW, 0, tileW, tileH));
                             if (rgbStream)
                             {
                                 // 直接填编码器需要的通道顺序，省去读写整条带的二次转换。
                                 cv::cvtColor(*image, region, cv::COLOR_BGR2RGB);
                             }
                             else
                             {
                                 image->copyTo(region);
                             }
                         }
                         finish(static_cast<size_t>(tileRow) * tilesX + x);
                     });
        };

        std::unique_ptr<PngStreamWriter> png;
        std::unique_ptr<JpgStreamWriter> jpg;
        std::unique_ptr<BigTiffWriter> tiff;
        cv::Mat rgb;
        auto openWriter = [&]()
        {
            if (format == "png")
            {
                png = std::make_unique<PngStreamWriter>(stats.path, width, height, config.pngCompressionLevel);
            }
            else if (format == "jpg")
            {
                jpg = std::make_unique<JpgStreamWriter>(stats.path, width, height, config.jpegQuality);
            }
            else
            {
                tiff = std::make_unique<BigTiffWriter>(stats.path, width, height, true);
            }
        };
        auto writeRows = [&](const cv::Mat &rows, int offset)
        {
            if (png)
            {
                const auto encodeRows = [&](const cv::Mat &pixels)
                {
                    const auto codecStart = now();
                    const bool ok = png->writeRows(pixels.data, pixels.step, pixels.rows);
                    stats.codecMs += elapsed(codecStart);
                    if (!ok)
                    {
                        throw std::runtime_error("PNG rows write failed");
                    }
                };
                if (rgbStream)
                {
                    encodeRows(rows);
                }
                else
                {
                    for (int y = 0; y < rows.rows; y += rgbRows)
                    {
                        const int count = std::min(rgbRows, rows.rows - y);
                        const auto colorStart = now();
                        cv::cvtColor(rows.rowRange(y, y + count), rgb, cv::COLOR_BGR2RGB);
                        stats.colorMs += elapsed(colorStart);
                        encodeRows(rgb);
                    }
                }
                return;
            }
            for (int y = 0; y < rows.rows; ++y)
            {
                // TIFF 行接口内部完成转换，避免在公共流程中再转换一次。
                const auto colorStart = now();
                if (!tiff)
                {
                    cv::cvtColor(rows.row(y), rgb, cv::COLOR_BGR2RGB);
                }
                stats.colorMs += elapsed(colorStart);
                const auto codecStart = now();
                const bool ok = jpg ? jpg->writeRow(rgb.data) : tiff->writeRow(offset + y, rows.ptr<uint8_t>(y));
                stats.codecMs += elapsed(codecStart);
                if (!ok)
                {
                    throw std::runtime_error("image row write failed");
                }
            }
        };
        if (stream)
        {
            openWriter();
            std::array<cv::Mat, 2> rows{cv::Mat(tileH, width, CV_8UC3), cv::Mat(tileH, width, CV_8UC3)};
            std::array<bool, 2> ready{false, false};
            std::mutex mutex;
            std::condition_variable changed;
            bool stop = false;
            std::exception_ptr error;
            // 一条生产线程协调固定工作池；两个槽位限制预取内存和背压。
            std::thread producer(
                [&]()
                {
                    try
                    {
                        for (int y = 0; y < tilesY; ++y)
                        {
                            const int slot = y % 2;
                            std::unique_lock<std::mutex> lock(mutex);
                            const auto waitStart = now();
                            changed.wait(lock, [&]() { return stop || !ready[slot]; });
                            stats.producerWaitMs += elapsed(waitStart);
                            if (stop)
                            {
                                return;
                            }
                            lock.unlock();
                            const auto start = now();
                            fill(rows[slot], y);
                            stats.placementMs += elapsed(start);
                            lock.lock();
                            ready[slot] = true;
                            changed.notify_all();
                        }
                    }
                    catch (...)
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        error = std::current_exception();
                        changed.notify_all();
                    }
                });
            try
            {
                for (int y = 0; y < tilesY; ++y)
                {
                    const int slot = y % 2;
                    std::unique_lock<std::mutex> lock(mutex);
                    const auto waitStart = now();
                    changed.wait(lock, [&]() { return ready[slot] || error; });
                    stats.consumerWaitMs += elapsed(waitStart);
                    if (error)
                    {
                        std::rethrow_exception(error);
                    }
                    lock.unlock();
                    const auto start = now();
                    writeRows(rows[slot], y * tileH);
                    previewRows(rows[slot], y * tileH);
                    stats.encodingMs += elapsed(start);
                    lock.lock();
                    ready[slot] = false;
                    changed.notify_all();
                }
            }
            catch (...)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    stop = true;
                }
                changed.notify_all();
                producer.join();
                throw;
            }
            producer.join();
        }
        else
        {
            const auto start = now();
            cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(64, 64, 64));
            pool.run(count,
                     [&](size_t i)
                     {
                         auto image = load(i);
                         if (image)
                         {
                             image->copyTo(canvas(cv::Rect(static_cast<int>(i % tilesX) * tileW,
                                                           static_cast<int>(i / tilesX) * tileH, tileW, tileH)));
                         }
                         finish(i);
                     });
            stats.placementMs = elapsed(start);
            // 画布已持有全部像素，编码前释放缓存，避免和编码缓冲叠加。
            cache.clear();
            const auto encodeStart = now();
            previewRows(canvas, 0);
            if (format == "png")
            {
                openWriter();
                writeRows(canvas, 0);
            }
            else if (format == "tiff")
            {
                BigTiffWriter writer(stats.path, width, height);
                if (!writer.writeMat(canvas.data, static_cast<int>(canvas.step)))
                {
                    throw std::runtime_error("TIFF write failed");
                }
            }
            else if (!imwriteUnicode(
                         stats.path, canvas,
                         {format == "jpg" ? cv::IMWRITE_JPEG_QUALITY : cv::IMWRITE_WEBP_QUALITY, config.jpegQuality}))
            {
                throw std::runtime_error("image write failed");
            }
            stats.encodingMs = elapsed(encodeStart);
        }
        const auto closeStart = now();
        if ((png && !png->close()) || (jpg && !jpg->close()))
        {
            throw std::runtime_error("image close failed");
        }
        tiff.reset();
        stats.encodingMs += elapsed(closeStart);
        if (runtime)
        {
            runtime->output(width, height, format, static_cast<int>(count) - failures.load(), failures.load());
            runtime->artifact("main", "image", stats.path, "image/" + (format == "jpg" ? std::string("jpeg") : format));
        }
        if (preview)
        {
            if (runtime)
            {
                runtime->stage("writing_preview");
            }
            auto previewFile = u8path(stats.path);
            previewFile.replace_extension();
            previewFile += "_preview.jpg";
            const auto previewPath = pathToUtf8(previewFile);
            const auto start = now();
            try
            {
                // 主图已关闭成功，预览失败只报告错误，不删除已经成功的主图。
                const auto &pixels = preview->image();
                cv::Mat bgr;
                if (rgbStream)
                {
                    cv::cvtColor(pixels, bgr, cv::COLOR_RGB2BGR);
                }
                if (!imwriteUnicode(previewPath, rgbStream ? bgr : pixels,
                                    {cv::IMWRITE_JPEG_QUALITY, PreviewAccumulator::kJpegQuality}))
                {
                    throw std::runtime_error("JPEG could not be written");
                }
            }
            catch (const std::exception &error)
            {
                throw std::runtime_error("preview write failed: " + previewPath + "; main image saved: " +
                                         stats.path + "; " + error.what());
            }
            const double previewWriteMs = elapsed(start);
            stats.previewMs += previewWriteMs;
            stats.encodingMs += previewWriteMs;
            stats.previewPath = previewPath;
            if (runtime)
            {
                runtime->artifact("preview", "preview", previewPath, "image/jpeg");
            }
        }
        stats.cacheHits = cache.stats().hits;
        stats.cacheDecodes = cache.stats().decodes;
        stats.cachePeakBytes = cache.stats().peakBytes;
        stats.cacheSharedLoads = cache.stats().sharedLoads;
        stats.loadingMs = loadingNs.load() / 1000000.0;
        stats.failed = failures;
        stats.matched = static_cast<int>(count) - stats.failed;
        finishOverlayStats();
        return true;
    }
    catch (const std::exception &error)
    {
        if (runtime && runtime->snapshot().diagnostic.code.empty())
        {
            runtime->fail(runtime->snapshot().progress.stage == "writing_preview" ? "preview_write_failed"
                                                                                  : "output_write_failed",
                          error.what(), "outputPath", stats.path.empty() ? requestedPath : stats.path);
        }
        else if (!runtime)
        {
            std::cerr << "ERROR: output failed: " << error.what() << std::endl;
        }
        return false;
    }
}

bool writeMosaicOutput(const MosaicEngine::Config &config, const std::string &path, int tilesX, int tilesY, int tileW,
                       int tileH, const std::vector<ImageRecord> &records, const std::vector<int> &selected,
                       const std::function<void(cv::Mat &, double)> &adjustColor, OutputStats &stats,
                       const cv::Mat &target, RuntimeContext *runtime)
{
    return writeOutput(
        config, path, tilesX, tilesY, tileW, tileH,
        [&](size_t i) -> const ImageRecord &
        {
            return records.at(i);
        },
        selected, adjustColor, stats, target, runtime);
}

bool writeIndexedMosaicOutput(const MosaicEngine::Config &config, const std::string &path, int tilesX, int tilesY,
                              int tileW, int tileH, const std::vector<ImageRecord> &library,
                              const std::vector<int> &selected,
                              const std::function<void(cv::Mat &, double)> &adjustColor, OutputStats &stats,
                              const cv::Mat &target, RuntimeContext *runtime)
{
    const ImageRecord missing;
    return writeOutput(
        config, path, tilesX, tilesY, tileW, tileH,
        [&](size_t i) -> const ImageRecord &
        {
            return selected.at(i) < 0 ? missing : library.at(selected[i]);
        },
        selected, adjustColor, stats, target, runtime);
}

bool writeIndexedMosaicOutput(const MosaicEngine::Config &config, const detail::OutputPlan &plan,
                              const std::vector<ImageRecord> &library, const std::vector<int> &selected,
                              const std::function<void(cv::Mat &, double)> &adjustColor, OutputStats &stats,
                              const cv::Mat &target, RuntimeContext *runtime)
{
    const ImageRecord missing;
    return writeOutput(
        config, plan.path, plan.tilesX, plan.tilesY, plan.tileWidth, plan.tileHeight,
        [&](size_t i) -> const ImageRecord &
        {
            return selected.at(i) < 0 ? missing : library.at(selected[i]);
        },
        selected, adjustColor, stats, target, runtime, &plan);
}

} // namespace mosaicraft
