#include "NormalizeService.h"
#include "ImageNormalizer.h"
#include "OutputPublication.h"
#include "RequestValidation.h"
#include "SmartCrop.h"
#if MOSAICRAFT_SMART_CROP_CUDA
#include "SmartCropCudaRecovery.h"
#endif
#include "UnicodeIO.h"
#include "WorkerPool.h"
#include <algorithm>
#include <atomic>
#include <chrono>
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

ServiceResult normalizeImages(const BuildRequest &request, RuntimeContext &runtime)
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
    const char *diagnosticField = policy == CropDevice::Cuda ? "cropDevice" : "cropMode";
    std::optional<SmartCropModelPair> models;
    std::unique_ptr<SmartCropInference> session;
#if MOSAICRAFT_SMART_CROP_CUDA
    std::unique_ptr<SmartCropCudaRecovery> autoCuda;
#endif
    const auto useSession = [&](CropDevice device)
    {
        auto next = std::make_unique<SmartCropInference>(*models, device);
        next->probe();
        session = std::move(next);
    };
    if (request.cropMode != CropMode::Center)
    {
        runtime.metric("smartCropCudaFallbacks", 0);
        runtime.metric("smartCropCudaRecoveries", 0);
        runtime.stage("loading_crop_models");
        try
        {
            if (policy == CropDevice::Cuda)
            {
                requireSmartCropDevice(CropDevice::Cuda);
            }
            models = loadSmartCropModelPair(request.cropMode);
            CropDevice initial = policy == CropDevice::Cuda ? CropDevice::Cuda : CropDevice::Cpu;
#if MOSAICRAFT_SMART_CROP_CUDA
            if (policy == CropDevice::Auto)
            {
                autoCuda = std::make_unique<SmartCropCudaRecovery>(*models, runtime);
                initial = autoCuda->initialize() ? CropDevice::Cuda : CropDevice::Cpu;
            }
            if (!autoCuda || initial == CropDevice::Cpu)
#endif
            {
                useSession(initial);
            }
            runtime.computeIdentity(cropDeviceName(initial), "fp32", "opencv-dnn");
        }
        catch (const SmartCropError &problem)
        {
            return runtimeFailure(runtime, 1, problem.what(), problem.code(), diagnosticField);
        }
    }
    fs::create_directories(output);
    std::atomic<size_t> completed{0}, failed{0};
    const auto process = [&](size_t index, ImageNormalizer &normalizer)
    {
        const auto destination = output / (std::to_string(index) + pathToUtf8(files[index].extension()));
        if (!normalizer.processStandaloneImage(pathToUtf8(files[index]), pathToUtf8(destination)))
        {
            ++failed;
        }
        const auto processed = ++completed;
        // Fast CUDA batches can finish within the progress throttle interval.
        runtime.advance("images", processed, files.size(), "images", processed == files.size());
    };
    runtime.stage("normalizing");
    if (!models)
    {
        const auto slots = std::min(files.size(), static_cast<size_t>(normalizationWorkerCount(request.threads)));
        WorkerPool workers(static_cast<unsigned>(slots));
        workers.run(files.size(), [&](size_t index)
        {
            ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight);
            process(index, normalizer);
        });
    }
    else
    {
        SmartCropStats stats;
        size_t cpuImages = 0, cudaImages = 0;
        const auto processStart = std::chrono::steady_clock::now();
        auto processingEnd = processStart;
        const auto emitStats = [&]()
        {
            runtime.metric("smartCropCpuImages", static_cast<double>(cpuImages));
            runtime.metric("smartCropCudaImages", static_cast<double>(cudaImages));
            if (cpuImages && cudaImages)
            {
                runtime.computeIdentity("mixed", "fp32", "opencv-dnn");
            }
            runtime.metric("smartCropFacesFound", static_cast<double>(stats.facesFound));
            runtime.metric("smartCropMoved", static_cast<double>(stats.moved));
            runtime.metric("smartCropCentered", static_cast<double>(stats.centered));
            runtime.metric("smartCropMainFaceClipped", static_cast<double>(stats.mainFaceClipped));
            runtime.metric("smartCropInferenceWorkerMs", stats.inferenceMs);
            runtime.metric("smartCropSaliencyMs", stats.saliencyMs);
            runtime.metric("smartCropFaceMs", stats.faceMs);
            const auto end = processingEnd == processStart ? std::chrono::steady_clock::now() : processingEnd;
            const double seconds = std::chrono::duration<double>(end - processStart).count();
            runtime.metric("smartCropImagesPerSecond", seconds > 0 ? (completed.load() - failed.load()) / seconds : 0);
        };
        try
        {
            const auto analyze = [&](const cv::Mat &image)
            {
#if MOSAICRAFT_SMART_CROP_CUDA
                if (autoCuda)
                {
                    if (auto result = autoCuda->tryAnalyze(image))
                    {
                        runtime.metric("smartCropCudaImages", static_cast<double>(++cudaImages));
                        session.reset(); // A dormant CPU session can retain large input buffers.
                        runtime.computeIdentity("cuda", "fp32", "opencv-dnn");
                        return std::move(*result);
                    }
                    runtime.computeIdentity("cpu", "fp32", "opencv-dnn");
                }
#endif
                if (!session)
                {
                    useSession(CropDevice::Cpu);
                }
                auto result = session->analyze(image);
                if (policy == CropDevice::Cuda)
                {
                    runtime.metric("smartCropCudaImages", static_cast<double>(++cudaImages));
                }
                else
                {
                    runtime.metric("smartCropCpuImages", static_cast<double>(++cpuImages));
                }
                return result;
            };
            ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight, analyze, &stats);
            for (size_t index = 0; index < files.size(); ++index)
            {
                process(index, normalizer);
            }
            processingEnd = std::chrono::steady_clock::now();
#if MOSAICRAFT_SMART_CROP_CUDA
            if (autoCuda)
            {
                autoCuda->finish();
            }
#endif
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
} // namespace mosaicraft
