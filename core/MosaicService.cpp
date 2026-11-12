#include "MosaicService.h"

#include "ExecutionScope.h"
#include "OutputPublication.h"
#include "RequestValidation.h"
#include "UnicodeIO.h"

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
    if (!request.directoryOutput && std::filesystem::is_directory(u8path(request.outputPath), ec))
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
        ExecutionScope scope;
        MosaicEngine engine;
        const auto dbPath = resolveDbPathForService(request.dbPath);
        protectOutput(request.outputPath, {request.inputPath, pathToUtf8(u8path(dbPath).parent_path())});
        std::unique_ptr<OutputPublication> publication;
        auto path = request.outputPath;
        if (request.atomicOutput)
        {
            publication = std::make_unique<OutputPublication>(
                path, request.directoryOutput, request.overwrite,
                std::vector<std::string>{request.inputPath, pathToUtf8(u8path(dbPath).parent_path())});
            path = publication->workPath();
            if (request.directoryOutput && validated.config.outputMode != OutputMode::Html)
            {
                path += "/index";
            }
            validated.config.deferUsage = true;
        }
        const bool ok =
            selection ? engine.renderSelection(request.inputPath, dbPath, path, validated.config, *selection, runtime)
                      : engine.generate(request.inputPath, dbPath, path, validated.config, runtime);

        if (publication)
        {
            if (ok)
            {
                publication->commit(runtime,
                                    [&]()
                                    {
                                        runtime.commitUsage();
                                    });
            }
            else
            {
                runtime.discardArtifacts();
            }
        }
        const auto diagnostic = runtime.snapshot().diagnostic;
        const int code = diagnostic.code.rfind("cuda_", 0) == 0 ? 4 : diagnostic.code.rfind("library_", 0) == 0 ? 2 : 1;
        return ok ? ServiceResult::success("mosaic generated")
                  : ServiceResult::failure(code, diagnostic.message.empty() ? "mosaic generation failed"
                                                                            : diagnostic.message);
    }
    catch (const std::bad_alloc&)
    {
        runtime.discardArtifacts();
        return runtimeFailure(runtime, 3, "not enough memory to generate mosaic", "out_of_memory");
    }
    catch (const std::exception& exception)
    {
        if (request.atomicOutput)
        {
            runtime.discardArtifacts();
        }
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
        return ServiceResult::failure(runtime.snapshot().diagnostic.code.rfind("cuda_", 0) == 0 ? 4 : 1,
                                      exception.what());
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
    return preview(request, seed, workspace, canceled, selection, runtime);
}

ServiceResult MosaicService::preview(const MosaicRequest &request, uint32_t seed, PreviewWorkspace &workspace,
                                     const std::atomic_bool &canceled, MosaicSelection &selection,
                                     RuntimeContext &runtime) const
{
    try
    {
        std::lock_guard<std::mutex> lock(pipelineMutex());
        ExecutionScope scope;
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
