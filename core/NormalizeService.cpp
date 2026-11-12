#include "NormalizeService.h"
#include "ImageNormalizer.h"
#include "UnicodeIO.h"
#include "RequestValidation.h"
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
    const auto inside = [](const fs::path &a, const fs::path &b)
    {
        const auto relative = a.lexically_relative(b);
        return !relative.empty() && *relative.begin() != "..";
    };
    if (inside(input, output) || inside(output, input))
    {
        return runtimeFailure(runtime, 1, "normalization input and output directories must not overlap", "invalid_argument");
    }
    if (!fs::is_directory(input) || fs::exists(output))
    {
        return runtimeFailure(runtime, 1, "normalization requires an input directory and a new output directory", "invalid_argument");
    }
    std::vector<fs::path> files;
    const std::set<std::string> extensions{".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"};
    const auto collect = [&](const auto &entry)
    {
        auto ext = pathToUtf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
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
    workers.run(files.size(), [&](size_t index)
    {
        ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight);
        const auto destination = output / (std::to_string(index) + pathToUtf8(files[index].extension()));
        if (!normalizer.process(pathToUtf8(files[index]), pathToUtf8(destination)))
        {
            ++failed;
        }
        runtime.advance("images", ++completed, files.size(), "images");
    });
    runtime.artifact("normalized", "directory", pathToUtf8(output));
    return failed ? runtimeFailure(runtime, 1, "some images could not be normalized", "input_unreadable")
                  : ServiceResult::success("images normalized without creating a database");
}
}
