#include "MosaicOutput.h"
#include "BigTiffWriter.h"
#include "DeepZoomWriter.h"
#include "ImageCache.h"
#include "JpgStreamWriter.h"
#include "PngStreamWriter.h"
#include "WorkerPool.h"
#include <array>
#include <chrono>
#include <iostream>
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
#else
    const long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? static_cast<uint64_t>(pages) * size : 0;
#endif
}
} // namespace

bool writeMosaicOutput(const MosaicEngine::Config &config, const std::string &path, int tilesX, int tilesY, int tileW,
                       int tileH, const std::vector<ImageRecord> &records, const std::vector<int> &selected,
                       const std::function<void(cv::Mat &, double)> &adjustColor, OutputStats &stats)
{
    try
    {
        stats = {};
        const int width = tilesX * tileW, height = tilesY * tileH;
        const size_t count = static_cast<size_t>(tilesX) * tilesY;
        ImageCache cache;
        WorkerPool pool(static_cast<unsigned>(
            std::min<size_t>(count, std::min(8u, std::max(1u, std::thread::hardware_concurrency())))));
        std::atomic<int> failures{0};
        auto load = [&](size_t i) -> std::shared_ptr<const cv::Mat>
        {
            if (selected[i] < 0)
            {
                ++failures;
                return {};
            }
            auto image = cache.getShared(records[i].id, records[i].filePath, tileW, tileH);
            if (!image)
            {
                ++failures;
                return {};
            }
            if (config.colorAdjust)
            {
                cv::Mat adjusted = image->clone();
                adjustColor(adjusted, config.colorStrength);
                return std::make_shared<const cv::Mat>(std::move(adjusted));
            }
            return image;
        };
        stats.path = path;
        if (config.tiledOutput)
        {
            const auto start = Clock::now();
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
                     });
            if (config.deepZoom)
            {
                DeepZoomWriter::buildPyramid(folder, tileW, tileH, tilesX, tilesY, config.jpegQuality);
            }
            stats.placementMs = milliseconds(start);
            stats.failed = failures;
            stats.matched = static_cast<int>(count) - stats.failed;
            return true;
        }

        std::string format = config.outputFormat;
        auto extension = std::filesystem::u8path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c)
                       {
                           return static_cast<char>(std::tolower(c));
                       });
        if (!config.formatExplicit && (format == "jpg" || format.empty()))
        {
            if (extension == ".png")
            {
                format = "png";
            }
            else if (extension == ".webp")
            {
                format = "webp";
            }
            else if (extension == ".tif" || extension == ".tiff")
            {
                format = "tiff";
            }
        }
        if (format != "png" && format != "jpg" && format != "tiff" && format != "webp")
        {
            format = "jpg";
        }
        if (extension == ".jpeg")
        {
            extension = ".jpg";
        }
        if (extension == ".tif")
        {
            extension = ".tiff";
        }
        if ((config.formatExplicit && extension != "." + format) ||
            (format == "tiff" && (extension == ".jpg" || extension == ".png" || extension == ".webp")))
        {
            auto outputPath = u8path(path);
            outputPath.replace_extension("." + format);
            stats.path = pathToUtf8(outputPath);
        }
        const uint64_t rawBytes = static_cast<uint64_t>(width) * height * 3;
        const uint64_t rowBytes = static_cast<uint64_t>(width) * tileH * 3;
        const uint64_t memory = availableMemory();
        // 编码可能暂存整图，且缓存与两行预取会同时存活，不能只估算画布。
        const uint64_t estimated = rawBytes * 2 + ImageCache::kDefaultBudget + rowBytes * 2 + 16 * 1024 * 1024;
        const bool stream =
            format != "webp" &&
            (config.writeMode == "stream" ||
             (config.writeMode == "auto" && (memory ? estimated > memory : rawBytes > 500ULL * 1024 * 1024)));
        std::cout << "  output: " << (stream ? "stream (two tile rows)" : "batch") << std::endl;

        auto fill = [&](cv::Mat &destination, int tileRow)
        {
            destination.setTo(cv::Scalar(64, 64, 64));
            pool.run(tilesX,
                     [&](size_t x)
                     {
                         auto image = load(static_cast<size_t>(tileRow) * tilesX + x);
                         if (image)
                         {
                             image->copyTo(destination(cv::Rect(static_cast<int>(x) * tileW, 0, tileW, tileH)));
                         }
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
            for (int y = 0; y < rows.rows; ++y)
            {
                cv::cvtColor(rows.row(y), rgb, cv::COLOR_BGR2RGB);
                bool ok = png ? png->writeRow(rgb.data)
                              : (jpg ? jpg->writeRow(rgb.data) : tiff->writeRow(offset + y, rows.ptr<uint8_t>(y)));
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
                            changed.wait(lock,
                                         [&]()
                                         {
                                             return stop || !ready[slot];
                                         });
                            if (stop)
                            {
                                return;
                            }
                            lock.unlock();
                            const auto start = Clock::now();
                            fill(rows[slot], y);
                            stats.placementMs += milliseconds(start);
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
                    changed.wait(lock,
                                 [&]()
                                 {
                                     return ready[slot] || error;
                                 });
                    if (error)
                    {
                        std::rethrow_exception(error);
                    }
                    lock.unlock();
                    const auto start = Clock::now();
                    writeRows(rows[slot], y * tileH);
                    stats.encodingMs += milliseconds(start);
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
            const auto start = Clock::now();
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
                     });
            stats.placementMs = milliseconds(start);
            const auto encodeStart = Clock::now();
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
            stats.encodingMs = milliseconds(encodeStart);
        }
        const auto closeStart = Clock::now();
        if ((png && !png->close()) || (jpg && !jpg->close()))
        {
            throw std::runtime_error("image close failed");
        }
        tiff.reset();
        stats.encodingMs += milliseconds(closeStart);
        stats.failed = failures;
        stats.matched = static_cast<int>(count) - stats.failed;
        return true;
    }
    catch (const std::exception &error)
    {
        std::cerr << "ERROR: output failed: " << error.what() << std::endl;
        return false;
    }
}

} // namespace mosaicraft
