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

enum class ApiRequestShape
{
    None,
    Body,
    Query,
    JobId,
    LegacyCommand
};

struct ApiEndpointMetadata
{
    ApiOperation operation = ApiOperation::Ping;
    ApiRequestShape requestShape = ApiRequestShape::None;
    std::string method;
    std::vector<std::string> methods;
    std::string path;
    std::string httpPattern;
    std::string description;
    std::string category;
    std::vector<std::string> requestFields;
    std::vector<std::string> queryKeys;
    bool legacy = false;
    bool enabled = true;
};

const char* apiOperationName(ApiOperation operation);
const char* apiRequestShapeName(ApiRequestShape shape);
std::vector<ApiEndpointMetadata> apiEndpointMetadata(bool legacyRunEnabled);
std::vector<std::string> apiFeatureList();

} // namespace mosaicraft
