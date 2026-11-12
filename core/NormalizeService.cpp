#include "NormalizeService.h"
#include "ImageNormalizer.h"
#include "OutputPublication.h"
#include "RequestValidation.h"
#include "UnicodeIO.h"
#include "WorkerPool.h"
#include <algorithm>
#include <atomic>
#include <set>

namespace mosaicraft
{
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
    fs::create_directories(output);
    std::atomic<size_t> completed{0}, failed{0};
    WorkerPool workers(request.threads > 0 ? request.threads : 0);
    runtime.stage("normalizing");
    workers.run(files.size(),
                [&](size_t index)
                {
                    ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight);
                    const auto destination = output / (std::to_string(index) + pathToUtf8(files[index].extension()));
                    auto extension = pathToUtf8(destination.extension());
                    std::transform(extension.begin(), extension.end(), extension.begin(),
                                   [](unsigned char c)
                                   {
                                       return static_cast<char>(std::tolower(c));
                                   });
                    // 与正式建库保持同样的编码质量，不因拆出纯归一化改变素材像素。
                    const auto parameters = extension == ".png" ? std::vector<int>{cv::IMWRITE_PNG_COMPRESSION, 9}
                                            : extension == ".jpg" || extension == ".jpeg"
                                                ? std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 100}
                                                : std::vector<int>{};
                    if (!normalizer.process(pathToUtf8(files[index]), pathToUtf8(destination), parameters))
                    {
                        ++failed;
                    }
                    runtime.advance("images", ++completed, files.size(), "images");
                });
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
