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

static ServiceResult runMosaic(const MosaicRequest &request, RuntimeContext &runtime)
{
    runtime.stage("validating");
    if (request.inputPath.empty())
    {
        return runtimeFailure(runtime, 1, "inputPath is required", "invalid_argument", "inputPath");
    }
    if (request.dbPath.empty())
    {
        return runtimeFailure(runtime, 1, "dbPath is required", "invalid_argument", "dbPath");
    }
    if (request.outputPath.empty())
    {
        return runtimeFailure(runtime, 1, "outputPath is required", "invalid_argument", "outputPath");
    }

    std::error_code ec;
    if (std::filesystem::is_directory(u8path(request.outputPath), ec))
    {
        return runtimeFailure(runtime, 1, "outputPath is a directory, expected a file path", "invalid_argument",
                              "outputPath");
    }

    MosaicRequest validated = request;
    std::string error, field;
    if (!validateMosaicRequest(validated, error, &field))
    {
        return runtimeFailure(runtime, 1, error, "invalid_argument", field);
    }
    try
    {
        std::lock_guard<std::mutex> lock(pipelineMutex());
        MosaicEngine engine;
        bool ok = engine.generate(request.inputPath, resolveDbPathForService(request.dbPath), request.outputPath,
                                  validated.config, runtime);

        return ok ? ServiceResult::success("mosaic generated")
                  : ServiceResult::failure(1, runtime.snapshot().diagnostic.message.empty()
                                                  ? "mosaic generation failed"
                                                  : runtime.snapshot().diagnostic.message);
    }
    catch (const std::bad_alloc&)
    {
        return runtimeFailure(runtime, 3, "not enough memory to generate mosaic", "out_of_memory");
    }
    catch (const std::exception& exception)
    {
        if (runtime.snapshot().diagnostic.code.empty())
        {
            runtime.fail(runtime.snapshot().progress.stage == "matching" ? "cuda_execution_failed" : "internal_error",
                         exception.what());
        }
        return ServiceResult::failure(1, exception.what());
    }
}

ServiceResult MosaicService::run(const MosaicRequest &request) const
{
    RuntimeContext runtime;
    return run(request, runtime);
}

ServiceResult MosaicService::run(const MosaicRequest &request, RuntimeContext &runtime) const
{
    return runtimeResult(runMosaic(request, runtime), runtime);
}
} // namespace mosaicraft
