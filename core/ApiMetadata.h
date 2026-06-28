#pragma once

#include <string>
#include <unordered_map>
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

struct ApiErrorResponseMetadata
{
    int status = 500;
    std::string shape;
    std::string responseKey;
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
    std::vector<std::string> requiredFields;
    std::vector<std::string> queryKeys;
    std::vector<std::string> acceptedQueryKeys;
    std::unordered_map<std::string, std::vector<std::string>> fieldAliases;
    int successStatus = 200;
    std::string responseKey;
    std::vector<int> errorStatuses;
    std::vector<std::string> errorShapes;
    std::vector<std::string> errorResponseKeys;
    std::vector<ApiErrorResponseMetadata> errorResponses;
    bool sideEffects = false;
    bool longRunning = false;
    bool legacy = false;
    bool enabled = true;
};

const char* apiOperationName(ApiOperation operation);
const char* apiRequestShapeName(ApiRequestShape shape);
const char* apiContractVersion();
int apiContractMajorVersion();
const char* apiCompatibilityLevel();
bool apiContractStable();
std::vector<ApiEndpointMetadata> apiEndpointMetadata(bool legacyRunEnabled);
std::vector<std::string> validateApiEndpointMetadata(const std::vector<ApiEndpointMetadata>& endpoints);
std::vector<std::string> apiFeatureList();

} // namespace mosaicraft
