#include "NormalizeService.h"
#include "ExecutionScope.h"
#include "MosaicService.h"
#include "OutputPublication.h"
#include "RequestValidation.h"
#include "SmartCropSession.h"
#if MOSAICRAFT_SMART_CROP_VULKAN && MOSAICRAFT_SMART_CROP_CUDA
#include "SmartCropVulkan.h"
#endif
#include "UnicodeIO.h"
#include "WorkerPool.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string_view>
#include <set>
#include <thread>

namespace mosaicraft
{
int normalizationWorkerCount(int requestedThreads)
{
    if (requestedThreads > 0)
    {
        return requestedThreads;
    }
    // 与建库和追加共用自动策略，不受通用线程池的 8 线程默认上限限制。
    return std::max(2, static_cast<int>(std::thread::hardware_concurrency()));
}

void detail::runSmartCropPipeline(ImageNormalizer &normalizer, std::size_t count,
                                  const std::function<NormalizePaths(std::size_t)> &paths,
                                  const std::function<void(bool)> &complete, ImageNormalizer::StandaloneTimings &timings,
                                  std::size_t queueBytes)
{
    if (!count)
    {
        return;
    }
    struct Item
    {
        NormalizePaths paths;
        ImageNormalizer::PreparedImage image;
        bool ok = false;
    };
    Item decoded, cropped;
    WorkerPool reader(1), writer(1);
    const auto readAhead = [&](std::size_t index)
    {
        reader.start(1, [&, index](std::size_t)
        {
            decoded.paths = paths(index);
            decoded.ok = normalizer.prepareStandaloneImage(decoded.paths.first, decoded.paths.second, decoded.image, timings);
        });
    };
    const auto waitForWriter = [&]()
    {
        BenchmarkTimer timer(&timings.waitOutputMs);
        writer.wait();
    };
    const auto save = [&](Item item)
    {
        complete(item.ok && normalizer.saveStandaloneImage(item.paths.first, item.paths.second, item.image, timings));
    };
    // Reserve half for each slot so external work never lets one queue starve the other.
    const auto slotLimit = queueBytes / 2;
    const bool largeOutput = normalizer.standaloneOutputPixelBytes() > slotLimit;
    readAhead(0);
    try
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            {
                BenchmarkTimer timer(&timings.waitInputMs);
                reader.wait();
            }
            auto item = std::exchange(decoded, Item{});
            const bool exclusive = item.ok && (largeOutput || item.image.bufferBytes() > slotLimit);
            const bool more = index + 1 < count;
            if (exclusive)
            {
                // Decoding discovers the real allocation size. Keep it and drain instead of decoding twice.
                waitForWriter();
            }
            else if (more)
            {
                readAhead(index + 1);
            }
            if (item.ok)
            {
                item.ok = normalizer.cropStandaloneImage(item.paths.first, item.paths.second, item.image, timings);
            }
            if (exclusive)
            {
                save(std::move(item));
                if (more)
                {
                    readAhead(index + 1);
                }
            }
            else
            {
                waitForWriter();
                cropped = std::move(item);
                writer.start(1, [&](std::size_t)
                {
                    save(std::exchange(cropped, Item{}));
                });
            }
        }
    }
    catch (...)
    {
        // Do not lose the preceding output if the ordered inference fails. Never publish a prefetched successor.
        waitForWriter();
        throw;
    }
    waitForWriter();
}

void detail::runDualSmartCropPipeline(ImageNormalizer &cuda, ImageNormalizer &vulkan, std::size_t count,
                                      const std::function<NormalizePaths(std::size_t)> &paths,
                                      const std::function<void(bool)> &complete, ImageNormalizer::StandaloneTimings &timings,
                                      std::size_t queueBytes, std::size_t vulkanMaxPixels)
{
    if (!count)
    {
        return;
    }
    using Clock = std::chrono::steady_clock;
    enum class State
    {
        Waiting, Running, Done
    };
    struct Item
    {
        std::size_t index = 0, bytes = 0;
        NormalizePaths paths;
        ImageNormalizer::PreparedImage image;
        std::exception_ptr failure;
        State state = State::Waiting;
        bool ok = false, exclusive = false, vulkanEligible = false;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Item> queue;
    std::array<ImageNormalizer::StandaloneTimings, 2> laneTimes;
    std::array<bool, 2> active{};
    Clock::time_point overlapStart;
    std::size_t firstFailure = count, reserved = 0;
    std::exception_ptr failure;
    bool stopped = false;

    // One prefetched input gets its own reservation. The ordered queue also
    // holds running tasks and the writer's current item, not just finished results.
    constexpr std::size_t maxPending = 8;
    const auto readLimit = queueBytes / 4;
    const auto pendingLimit = queueBytes - readLimit;
    const auto outputBytes = cuda.standaloneOutputPixelBytes();
    const auto read = [&]()
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            std::unique_lock<std::mutex> lock(mutex);
            const auto cancelled = [&]()
            {
                return stopped || index >= firstFailure;
            };
            changed.wait(lock, [&]()
            {
                return cancelled() || queue.empty() || !queue.front().exclusive;
            });
            if (cancelled())
            {
                return;
            }
            lock.unlock();

            Item item;
            item.index = index;
            item.paths = paths(index);
            item.ok = cuda.prepareStandaloneImage(item.paths.first, item.paths.second, item.image, timings);
            if (item.ok)
            {
                item.bytes = item.image.bufferBytes();
                if (!item.image.copyOriginal)
                {
                    item.bytes = std::max(item.bytes, outputBytes);
                    const auto w = (static_cast<std::uint64_t>(item.image.pixels.cols) + 31) / 32 * 32;
                    const auto h = (static_cast<std::uint64_t>(item.image.pixels.rows) + 31) / 32 * 32;
                    item.vulkanEligible = count - index >= 4 && w * h <= vulkanMaxPixels;
                }
                item.exclusive = item.bytes > readLimit;
            }
            if (!item.ok || item.image.copyOriginal)
            {
                item.state = State::Done;
            }

            lock.lock();
            {
                BenchmarkTimer timer(&timings.waitOutputMs);
                changed.wait(lock, [&]()
                {
                    return cancelled() || queue.empty() ||
                           (!item.exclusive && queue.size() < maxPending && item.bytes <= pendingLimit - reserved &&
                            queue.back().state != State::Waiting);
                });
            }
            if (cancelled())
            {
                return;
            }
            // Oversized inputs drain both lanes before running alone. Do not decode them again.
            reserved += item.bytes;
            if (!item.exclusive)
            {
                timings.dualQueuePeakBytes = std::max(timings.dualQueuePeakBytes, reserved);
            }
            queue.push_back(std::move(item));
            timings.dualPendingPeak = std::max(timings.dualPendingPeak, queue.size());
            changed.notify_all();
        }
    };
    const auto infer = [&](std::size_t lane)
    {
        auto &normalizer = lane == 0 ? cuda : vulkan;
        const auto other = 1 - lane;
        std::unique_lock<std::mutex> lock(mutex);
        for (;;)
        {
            changed.wait(lock, [&]()
            {
                return stopped || firstFailure < count ||
                       (!queue.empty() && queue.back().state == State::Waiting &&
                        (lane == 0 || (active[0] && queue.back().vulkanEligible && !queue.back().exclusive)));
            });
            if (stopped || firstFailure < count)
            {
                return; // Items already claimed before the failure still finish.
            }
            auto &item = queue.back();
            item.state = State::Running;
            active[lane] = true;
            if (active[other])
            {
                overlapStart = Clock::now();
            }
            changed.notify_all();
            lock.unlock();

            try
            {
                item.ok = normalizer.cropStandaloneImage(item.paths.first, item.paths.second, item.image, laneTimes[lane]);
            }
            catch (...)
            {
                item.failure = std::current_exception();
                item.ok = false;
            }
            if (!item.ok)
            {
                item.image = {};
            }

            lock.lock();
            if (active[other])
            {
                timings.dualOverlapMs += std::chrono::duration<double, std::milli>(Clock::now() - overlapStart).count();
            }
            active[lane] = false;
            reserved -= item.bytes;
            item.bytes = item.image.bufferBytes();
            reserved += item.bytes;
            if (item.failure)
            {
                firstFailure = std::min(firstFailure, item.index);
            }
            item.state = State::Done;
            // The lane can claim more work immediately, even with an unfinished queue head.
            changed.notify_all();
        }
    };
    const auto write = [&]()
    {
        std::unique_lock<std::mutex> lock(mutex);
        for (std::size_t index = 0; index < count; ++index)
        {
            {
                BenchmarkTimer timer(&timings.waitInputMs);
                changed.wait(lock, [&]()
                {
                    return stopped || (!queue.empty() && queue.front().state == State::Done);
                });
            }
            if (stopped)
            {
                return;
            }
            auto &item = queue.front();
            if (item.failure)
            {
                std::rethrow_exception(item.failure);
            }
            lock.unlock();
            complete(item.ok && cuda.saveStandaloneImage(item.paths.first, item.paths.second, item.image, timings));
            lock.lock();
            reserved -= item.bytes;
            queue.pop_front();
            changed.notify_all();
        }
        stopped = true;
        changed.notify_all();
    };

    WorkerPool workers(4);
    workers.run(4, [&](std::size_t role)
    {
        try
        {
            if (role == 0)
            {
                read();
            }
            else if (role == 3)
            {
                write();
            }
            else
            {
                infer(role - 1);
            }
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!failure)
            {
                failure = std::current_exception();
            }
            stopped = true;
            changed.notify_all();
        }
    });
    timings.cropMs += laneTimes[0].cropMs + laneTimes[1].cropMs;
    if (failure)
    {
        std::rethrow_exception(failure);
    }
}

static ServiceResult normalizeImagesImpl(const BuildRequest &request, RuntimeContext &runtime,
                                         std::optional<int> vulkan, bool dual = false,
                                         std::size_t vulkanMaxPixels = detail::kDualVulkanMaxPixels)
{
    namespace fs = std::filesystem;
    std::string error, field;
    if (!validateBuildRequest(request, error, &field))
    {
        return runtimeFailure(runtime, 1, error, "invalid_argument", field);
    }
    const auto input = fs::weakly_canonical(u8path(request.inputDir));
    const auto output = fs::weakly_canonical(u8path(request.outputDir));
    if (pathWithin(input, output) || pathWithin(output, input))
    {
        return runtimeFailure(runtime, 1, "normalization input and output directories must not overlap",
                              "invalid_argument");
    }
    if (!fs::is_directory(input) || (fs::exists(output) && (!fs::is_directory(output) || !fs::is_empty(output))))
    {
        return runtimeFailure(runtime, 1,
                              "normalization requires an input directory and a new or empty output directory",
                              "invalid_argument");
    }
    std::vector<fs::path> files;
    const std::set<std::string> extensions{".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"};
    const auto collect = [&](const auto &entry)
    {
        auto ext = pathToUtf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c)
                       {
                           return std::tolower(c);
                       });
        if (entry.is_regular_file() && extensions.count(ext))
        {
            files.push_back(entry.path());
        }
    };
    if (request.recursive)
    {
        for (const auto &entry : fs::recursive_directory_iterator(input))
        {
            collect(entry);
        }
    }
    else
    {
        for (const auto &entry : fs::directory_iterator(input))
        {
            collect(entry);
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty())
    {
        return runtimeFailure(runtime, 1, "no supported images found", "input_unreadable");
    }
    const auto policy = request.cropDevice.value_or(CropDevice::Auto);
    const char *diagnosticField = vulkan || policy == CropDevice::Cuda ? "cropDevice" : "cropMode";
    const char *setting = std::getenv("MOSAICRAFT_SMART_CROP_PIPELINE");
    const std::string_view trial = setting ? setting : "";
    std::optional<SmartCropSession> smartCrop, secondary;
    if (request.cropMode != CropMode::Center)
    {
        const auto start = std::chrono::steady_clock::now();
        try
        {
            if (dual && (files.size() < 8 || normalizationWorkerCount(request.threads) < 4))
            {
                throw SmartCropError("smart_crop_dual_budget", "dual GPU trial requires at least eight files and four threads");
            }
            if (dual)
            {
                // The two strict probes target separate adapters. Join before publishing any output.
                WorkerPool probe(1);
                probe.start(1, [&](std::size_t)
                {
                    secondary.emplace(request.cropMode, CropDevice::Cpu, runtime, false, vulkan);
                });
                smartCrop.emplace(request.cropMode, CropDevice::Cuda, runtime, false);
                probe.wait();
                runtime.computeIdentity("dual", "fp32", "opencv-dnn+ncnn-vulkan");
            }
            else
            {
                smartCrop.emplace(request.cropMode, policy, runtime, true, vulkan);
            }
        }
        catch (const SmartCropError &problem)
        {
            return runtimeFailure(runtime, 1, problem.what(), problem.code(), diagnosticField);
        }
        if (vulkan || trial == "0" || trial == "1")
        {
            runtime.metric("smartCropStartupMs", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
    }
    fs::create_directories(output);
    std::atomic<size_t> completed{0}, failed{0};
    const auto paths = [&](std::size_t index)
    {
        const auto destination = output / (std::to_string(index) + pathToUtf8(files[index].extension()));
        return detail::NormalizePaths{pathToUtf8(files[index]), pathToUtf8(destination)};
    };
    const auto complete = [&](bool ok)
    {
        if (!ok)
        {
            ++failed;
        }
        const auto processed = ++completed;
        // Fast CUDA batches can finish within the progress throttle interval.
        runtime.advance("images", processed, files.size(), "images", processed == files.size());
    };
    const auto process = [&](std::size_t index, ImageNormalizer &normalizer, ImageNormalizer::StandaloneTimings *timings)
    {
        const auto [source, destination] = paths(index);
        complete(normalizer.processStandaloneImage(source, destination, nullptr, timings));
    };
    runtime.stage("normalizing");
    if (!smartCrop)
    {
        const auto slots = std::min(files.size(), static_cast<size_t>(normalizationWorkerCount(request.threads)));
        WorkerPool workers(static_cast<unsigned>(slots));
        workers.run(files.size(), [&](size_t index)
        {
            ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight);
            process(index, normalizer, nullptr);
        });
    }
    else
    {
        bool pipeline = dual;
#if MOSAICRAFT_SMART_CROP_CUDA && defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        // 只为分别通过同后端字节一致性与性能门槛的模式默认开启。
        const bool defaultPipeline = request.cropMode == CropMode::SmartReal || request.cropMode == CropMode::SmartAnime;
        pipeline = pipeline || (trial != "0" && (defaultPipeline || trial == "1") && smartCrop->initialDevice() == CropDevice::Cuda &&
                   files.size() >= 8 && normalizationWorkerCount(request.threads) >= 3);
#endif
        if (vulkan && !dual)
        {
            pipeline = trial != "0" && files.size() >= 8 && normalizationWorkerCount(request.threads) >= 3;
        }
        const bool measure = pipeline || trial == "0" || trial == "1";
        ImageNormalizer::StandaloneTimings timings;
        const auto processStart = std::chrono::steady_clock::now();
        auto processingEnd = processStart;
        const auto emitStats = [&]()
        {
            smartCrop->report(secondary ? &*secondary : nullptr);
            if (measure)
            {
                runtime.metric("smartCropPipeline", pipeline ? 1 : 0);
                runtime.metric("smartCropReadDecodeMs", timings.readDecodeMs);
                runtime.metric("smartCropCropMs", timings.cropMs);
                runtime.metric("smartCropEncodeWriteMs", timings.encodeWriteMs);
                runtime.metric("smartCropWaitInputMs", timings.waitInputMs);
                runtime.metric("smartCropWaitOutputMs", timings.waitOutputMs);
                if (dual)
                {
                    runtime.metric("smartCropDualOverlapMs", timings.dualOverlapMs);
                    runtime.metric("smartCropDualVulkanMaxPixels", static_cast<double>(vulkanMaxPixels));
                    runtime.metric("smartCropDualPendingPeak", static_cast<double>(timings.dualPendingPeak));
                    runtime.metric("smartCropDualQueuePeakBytes", static_cast<double>(timings.dualQueuePeakBytes));
                }
            }
            const auto end = processingEnd == processStart ? std::chrono::steady_clock::now() : processingEnd;
            const double seconds = std::chrono::duration<double>(end - processStart).count();
            runtime.metric("smartCropImagesPerSecond", seconds > 0 ? (completed.load() - failed.load()) / seconds : 0);
        };
        try
        {
            auto normalizer = smartCrop->normalizer(request.normalizeWidth, request.normalizeHeight);
            if (secondary)
            {
                auto other = secondary->normalizer(request.normalizeWidth, request.normalizeHeight);
                detail::runDualSmartCropPipeline(normalizer, other, files.size(), paths, complete, timings,
                                                 256ull * 1024 * 1024, vulkanMaxPixels);
                secondary->finish();
            }
            else if (pipeline)
            {
                detail::runSmartCropPipeline(normalizer, files.size(), paths, complete, timings);
            }
            else
            {
                for (size_t index = 0; index < files.size(); ++index)
                {
                    process(index, normalizer, measure ? &timings : nullptr);
                }
            }
            processingEnd = std::chrono::steady_clock::now();
            smartCrop->finish();
        }
        catch (const SmartCropError &problem)
        {
            emitStats();
            const auto processed = completed.load();
            runtime.advance("images", processed, files.size(), "images", true);
            const auto saved = processed - failed.load();
            if (saved)
            {
                runtime.artifact("normalized-partial", "directory", pathToUtf8(output));
            }
            return runtimeFailure(runtime, 1, std::string(problem.what()) + "; " + std::to_string(saved) +
                                  " completed outputs kept in " + pathToUtf8(output), problem.code(), diagnosticField);
        }
        emitStats();
    }
    if (failed)
    {
        runtime.emit("warning", "source_skipped", std::to_string(failed.load()) + " images could not be normalized");
    }
    if (failed == files.size())
    {
        return runtimeFailure(runtime, 1, "no images could be normalized", "input_unreadable");
    }
    runtime.artifact("normalized", "directory", pathToUtf8(output));
    return ServiceResult::success("images normalized without creating a database");
}

ServiceResult normalizeImages(const BuildRequest &request, RuntimeContext &runtime)
{
    return normalizeImagesImpl(request, runtime, std::nullopt);
}

std::optional<ServiceResult> runCliSmartCropExperiment(const BuildRequest &request, RuntimeContext &runtime)
{
    const auto *setting = std::getenv("MOSAICRAFT_SMART_CROP_EXPERIMENT");
    if (!request.normalizeOnly || !setting || !*setting)
    {
        return std::nullopt;
    }
    const std::string_view experiment(setting);
    if ((experiment != "vulkan" && experiment != "dual") || request.cropMode == CropMode::Center ||
        request.cropDevice.value_or(CropDevice::Auto) != CropDevice::Auto)
    {
        return runtimeFailure(runtime, 1, "Vulkan trial requires CLI smart normalization and the default crop-device policy",
                              "invalid_argument", "cropDevice");
    }
#if MOSAICRAFT_SMART_CROP_VULKAN
    // Standalone Vulkan anime would also claim oversized inputs, unlike bounded dual dispatch.
    if (request.cropMode == CropMode::SmartAnime && experiment != "dual")
    {
        return runtimeFailure(runtime, 1, "Vulkan anime detection is currently limited to the bounded dual GPU experiment",
                              "smart_crop_vulkan_model", "cropMode");
    }
    const auto *value = std::getenv("MOSAICRAFT_SMART_CROP_VULKAN_DEVICE");
    const std::string_view index = value ? value : "";
    int device = -1;
    const auto parsed = std::from_chars(index.data(), index.data() + index.size(), device);
    if (parsed.ec != std::errc{} || parsed.ptr != index.data() + index.size() || device < 0)
    {
        return runtimeFailure(runtime, 1, "set MOSAICRAFT_SMART_CROP_VULKAN_DEVICE to an enumerated index",
                              "smart_crop_vulkan_device", "cropDevice");
    }
    std::lock_guard<std::mutex> lock(pipelineMutex());
    ExecutionScope scope;
    const bool dual = experiment == "dual";
    // On the tested 780M, admitting medium anime graphs slowed the CUDA lane.
    // This only changes assignment in the explicit experiment, never input scale.
    auto vulkanMaxPixels = request.cropMode == CropMode::SmartAnime ? 768ull * 1024 : detail::kDualVulkanMaxPixels;
    if (dual)
    {
        // Tune device selection, never the detector resolution or the hard Vulkan memory bound.
        if (const char *value = std::getenv("MOSAICRAFT_SMART_CROP_DUAL_MAX_PIXELS"))
        {
            const std::string_view limit(value);
            const auto parsedLimit = std::from_chars(limit.data(), limit.data() + limit.size(), vulkanMaxPixels);
            if (parsedLimit.ec != std::errc{} || parsedLimit.ptr != limit.data() + limit.size() ||
                vulkanMaxPixels < 1024 || vulkanMaxPixels > detail::kDualVulkanMaxPixels)
            {
                return runtimeFailure(runtime, 1, "dual GPU pixel limit must be in [1024, 4194304]",
                                      "smart_crop_dual_budget", "cropDevice");
            }
        }
#if MOSAICRAFT_SMART_CROP_CUDA
        // CUDA selects NVIDIA only. Restrict the second device to AMD/Intel rather than guessing adapter identity.
        try
        {
            const auto vendor = SmartCropVulkan::deviceVendor(device);
            if (vendor != 0x1002 && vendor != 0x8086)
            {
                return runtimeFailure(runtime, 1, "dual trial requires a separate AMD or Intel Vulkan adapter",
                                      "smart_crop_vulkan_device", "cropDevice");
            }
        }
        catch (const SmartCropError &problem)
        {
            return runtimeFailure(runtime, 1, problem.what(), problem.code(), "cropDevice");
        }
#else
        return runtimeFailure(runtime, 1, "dual GPU trial requires a CUDA smart crop build",
                              "smart_crop_cuda_unavailable", "cropDevice");
#endif
    }
    return runtimeResult(normalizeImagesImpl(request, runtime, device, dual, vulkanMaxPixels), runtime);
#else
    return runtimeFailure(runtime, 1, "this build does not support the Vulkan smart crop experiment",
                          "smart_crop_vulkan_unavailable", "cropDevice");
#endif
}
} // namespace mosaicraft
