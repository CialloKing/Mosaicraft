#include "MosaicService.h"

#include "UnicodeIO.h"
#include "RequestValidation.h"

#include <filesystem>

namespace mosaicraft
{

std::mutex& pipelineMutex()
{
    // FeaturePack 的写入器属于整条管线；同步 HTTP 调用也保持一次只运行一个任务。
    static std::mutex mutex;
    return mutex;
}

std::string resolveDbPathForService(const std::string& rawPath)
{
    std::error_code ec;
    if (std::filesystem::is_directory(u8path(rawPath), ec))
    {
        return rawPath + "/mosaicraft.db";
    }
    return rawPath;
}

ServiceResult MosaicService::run(const MosaicRequest& request) const
{
    if (request.inputPath.empty())
    {
        return ServiceResult::failure(1, "inputPath is required");
    }
    if (request.dbPath.empty())
    {
        return ServiceResult::failure(1, "dbPath is required");
    }
    if (request.outputPath.empty())
    {
        return ServiceResult::failure(1, "outputPath is required");
    }

    std::error_code ec;
    if (std::filesystem::is_directory(u8path(request.outputPath), ec))
    {
        return ServiceResult::failure(1, "outputPath is a directory, expected a file path");
    }

    MosaicRequest validated = request;
    std::string error;
    if (!validateMosaicRequest(validated, error))
    {
        return ServiceResult::failure(1, error);
    }
    try
    {
        std::lock_guard<std::mutex> lock(pipelineMutex());
        MosaicEngine engine;
        bool ok = engine.generate(
            request.inputPath,
            resolveDbPathForService(request.dbPath),
            request.outputPath,
            validated.config);

        return ok
            ? ServiceResult::success("mosaic generated")
            : ServiceResult::failure(1, "mosaic generation failed");
    }
    catch (const std::bad_alloc&)
    {
        return ServiceResult::failure(3, "not enough memory to generate mosaic");
    }
    catch (const std::exception& exception)
    {
        return ServiceResult::failure(1, exception.what());
    }
}

} // namespace mosaicraft
