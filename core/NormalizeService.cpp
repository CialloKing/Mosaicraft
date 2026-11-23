#include "NormalizeService.h"
#include "ImageNormalizer.h"
#include "OutputPublication.h"
#include "RequestValidation.h"
#include "SmartCrop.h"
#include "UnicodeIO.h"
#include "WorkerPool.h"
#include <algorithm>
#include <atomic>
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
    std::string error;
    if (!validateBuildRequest(request, error))
    {
        return runtimeFailure(runtime, 1, error, "invalid_argument");
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
    const bool smart = request.cropMode != CropMode::Center;
    auto slots = std::min(files.size(), static_cast<size_t>(normalizationWorkerCount(request.threads)));
    std::vector<std::unique_ptr<SmartCropInference>> sessions;
    if (smart)
    {
        slots = std::min<size_t>(slots, 4);
        runtime.stage("loading_crop_models");
        try
        {
            const auto models = loadSmartCropModelPair(request.cropMode);
            for (size_t slot = 0; slot < slots; ++slot)
            {
                sessions.push_back(std::make_unique<SmartCropInference>(models));
            }
            sessions.front()->probe();
        }
        catch (const SmartCropError &problem)
        {
            return runtimeFailure(runtime, 1, problem.what(), problem.code(), "cropMode");
        }
    }
    fs::create_directories(output);
    std::atomic<size_t> completed{0}, failed{0}, next{0};
    std::vector<SmartCropStats> stats(smart ? slots : 0);
    const auto emitStats = [&]()
    {
        if (!smart)
        {
            return;
        }
        SmartCropStats sum;
        for (const auto &slot : stats)
        {
            sum.facesFound += slot.facesFound;
            sum.moved += slot.moved;
            sum.centered += slot.centered;
            sum.mainFaceClipped += slot.mainFaceClipped;
            sum.inferenceMs += slot.inferenceMs;
        }
        runtime.metric("smartCropFacesFound", static_cast<double>(sum.facesFound));
        runtime.metric("smartCropMoved", static_cast<double>(sum.moved));
        runtime.metric("smartCropCentered", static_cast<double>(sum.centered));
        runtime.metric("smartCropMainFaceClipped", static_cast<double>(sum.mainFaceClipped));
        runtime.metric("smartCropInferenceWorkerMs", sum.inferenceMs);
    };
    const auto process = [&](size_t index, ImageNormalizer &normalizer)
    {
        const auto destination = output / (std::to_string(index) + pathToUtf8(files[index].extension()));
        if (!normalizer.processStandaloneImage(pathToUtf8(files[index]), pathToUtf8(destination)))
        {
            ++failed;
        }
        runtime.advance("images", ++completed, files.size(), "images");
    };
    WorkerPool workers(static_cast<unsigned>(slots));
    runtime.stage("normalizing");
    if (!smart)
    {
        workers.run(files.size(), [&](size_t index)
        {
            ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight);
            process(index, normalizer);
        });
    }
    else
    {
        try
        {
            // A slot never shares the mutable networks with any other task or slot.
            workers.run(slots, [&](size_t slot)
            {
                ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight, sessions[slot].get(), &stats[slot]);
                try
                {
                    for (size_t index = next.fetch_add(1); index < files.size(); index = next.fetch_add(1))
                    {
                        process(index, normalizer);
                    }
                }
                catch (...)
                {
                    next.store(files.size());
                    throw;
                }
            });
        }
        catch (const SmartCropError &problem)
        {
            emitStats();
            const auto saved = completed.load() - failed.load();
            if (saved)
            {
                runtime.artifact("normalized-partial", "directory", pathToUtf8(output));
            }
            return runtimeFailure(runtime, 1, std::string(problem.what()) + "; " + std::to_string(saved) +
                                  " completed outputs kept in " + pathToUtf8(output), problem.code(), "cropMode");
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
