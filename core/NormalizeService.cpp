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
    std::unique_ptr<SmartCropInference> session;
    if (request.cropMode != CropMode::Center)
    {
        runtime.stage("loading_crop_models");
        try
        {
            const auto models = loadSmartCropModelPair(request.cropMode);
            session = std::make_unique<SmartCropInference>(models);
            session->probe();
        }
        catch (const SmartCropError &problem)
        {
            return runtimeFailure(runtime, 1, problem.what(), problem.code(), "cropMode");
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
        runtime.advance("images", ++completed, files.size(), "images");
    };
    runtime.stage("normalizing");
    if (!session)
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
        const auto emitStats = [&]()
        {
            runtime.metric("smartCropFacesFound", static_cast<double>(stats.facesFound));
            runtime.metric("smartCropMoved", static_cast<double>(stats.moved));
            runtime.metric("smartCropCentered", static_cast<double>(stats.centered));
            runtime.metric("smartCropMainFaceClipped", static_cast<double>(stats.mainFaceClipped));
            runtime.metric("smartCropInferenceWorkerMs", stats.inferenceMs);
        };
        try
        {
            // Reuse just one pair of networks: each instance retains its largest full-resolution buffers.
            ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight, session.get(), &stats);
            for (size_t index = 0; index < files.size(); ++index)
            {
                process(index, normalizer);
            }
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
