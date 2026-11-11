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

static ServiceResult runMosaic(const MosaicRequest &request, RuntimeContext &runtime,
                               const MosaicSelection *selection)
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
        const auto dbPath = resolveDbPathForService(request.dbPath);
        const bool ok = selection
            ? engine.renderSelection(request.inputPath, dbPath, request.outputPath, validated.config, *selection, runtime)
            : engine.generate(request.inputPath, dbPath, request.outputPath, validated.config, runtime);

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
            const auto snapshot = runtime.snapshot();
            const bool cudaMatching = snapshot.progress.stage == "matching" &&
                (snapshot.featureBackend.find("cuda") != std::string::npos ||
                 snapshot.scoreBackend.find("cuda") != std::string::npos);
            runtime.fail(cudaMatching ? "cuda_execution_failed"
                         : snapshot.progress.stage == "analyzing" ? "analysis_write_failed" : "internal_error",
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

ServiceResult MosaicService::run(const MosaicRequest &request, RuntimeContext &runtime,
                                 const MosaicSelection *selection) const
{
    return runtimeResult(runMosaic(request, runtime, selection), runtime);
}

ServiceResult MosaicService::preview(const MosaicRequest &request, uint32_t seed, PreviewWorkspace &workspace,
                                     const std::atomic_bool &canceled, MosaicSelection &selection) const
{
    RuntimeContext runtime(false);
    try
    {
        std::lock_guard<std::mutex> lock(pipelineMutex());
        const bool ok = MosaicEngine().preview(request.inputPath, resolveDbPathForService(request.dbPath),
                                               request.config, seed, workspace, canceled, selection, runtime);
        return runtimeResult(ok ? ServiceResult::success() :
            ServiceResult::failure(1, runtime.snapshot().diagnostic.message), runtime);
    }
    catch (const std::bad_alloc &)
    {
        return runtimeResult(runtimeFailure(runtime, 3, "not enough memory to preview mosaic", "out_of_memory"),
                             runtime);
    }
    catch (const std::exception &error)
    {
        return runtimeResult(runtimeFailure(runtime, 1, error.what(), "internal_error"), runtime);
    }
}
} // namespace mosaicraft
