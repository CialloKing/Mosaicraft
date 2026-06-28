#pragma once

#include <string>
#include <vector>

namespace mosaicraft
{

enum class ApiOperation
{
    Endpoints,
    Info,
    Ping,
    LegacyRunDisabled,
    Mosaic,
    SubmitMosaicJob,
    SubmitBuildJob,
    ListJobs,
    ClearFinishedJobs,
    GetJob,
    CancelJob,
    DatabaseStats,
    DatabaseHealth,
    DatabaseUsage,
    DatabaseUsageExport,
    DatabasePurge,
    Inspect
};

struct ApiEndpointMetadata
{
    ApiOperation operation = ApiOperation::Ping;
    std::string method;
    std::string path;
    std::string description;
    std::string category;
    std::vector<std::string> requestFields;
    bool legacy = false;
    bool enabled = true;
};

const char* apiOperationName(ApiOperation operation);
std::vector<ApiEndpointMetadata> apiEndpointMetadata(bool legacyRunEnabled);
std::vector<std::string> apiFeatureList();

} // namespace mosaicraft
