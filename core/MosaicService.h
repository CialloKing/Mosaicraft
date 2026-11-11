#pragma once

#include "MosaicEngine.h"
#include "RuntimeContext.h"

#include <string>
#include <mutex>

namespace mosaicraft
{

struct MosaicSelection;
struct PreviewWorkspace;

struct MosaicRequest
{
    std::string inputPath;
    std::string dbPath = "library/mosaicraft.db";
    std::string outputPath = "output/output";
    MosaicEngine::Config config;
};

struct ServiceResult
{
    bool ok = false;
    int exitCode = 1;
    std::string message;
    RuntimeSnapshot runtime;

    static ServiceResult success(const std::string& msg = {})
    {
        return {true, 0, msg};
    }

    static ServiceResult failure(int code, const std::string &msg,
                                 const std::string &diagnosticCode = "operation_failed", const std::string &field = {})
    {
        ServiceResult result{false, code, msg};
        result.runtime.diagnostic.code = diagnosticCode;
        result.runtime.diagnostic.severity = "error";
        result.runtime.diagnostic.message = msg;
        result.runtime.diagnostic.field = field;
        return result;
    }
};

class MosaicService
{
public:
    ServiceResult run(const MosaicRequest& request) const;
    ServiceResult run(const MosaicRequest &request, RuntimeContext &runtime,
                      const MosaicSelection *selection = nullptr) const;
    ServiceResult preview(const MosaicRequest &request, uint32_t seed, PreviewWorkspace &workspace,
                          const std::atomic_bool &canceled, MosaicSelection &selection) const;
};

std::string resolveDbPathForService(const std::string& rawPath);
std::mutex& pipelineMutex();

inline ServiceResult runtimeFailure(RuntimeContext &runtime, int exitCode, const std::string &message,
                                    const std::string &code = "operation_failed", const std::string &field = {},
                                    const std::string &path = {})
{
    if (runtime.snapshot().diagnostic.code.empty())
    {
        runtime.fail(code, message, field, path);
    }
    return ServiceResult::failure(exitCode, message);
}

inline ServiceResult runtimeResult(ServiceResult result, RuntimeContext &runtime)
{
    result.runtime = runtime.snapshot();
    if (!result.ok && result.runtime.diagnostic.code.empty())
    {
        runtime.fail("operation_failed", result.message);
        result.runtime = runtime.snapshot();
    }
    return result;
}

} // namespace mosaicraft
