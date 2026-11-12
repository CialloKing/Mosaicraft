#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// Windows spooler headers also define GetJob; API operation names are platform independent.
#ifdef GetJob
#undef GetJob
#endif

namespace mosaicraft
{

enum class ApiOperation
{
    Endpoints,
    Info,
    Ping,

    Mosaic,
    PlanMosaic,
    MosaicCoverage,
    OpenCatalog,
    GetCatalog,
    GetCatalogImage,
    SubmitMosaicSample,
    SubmitPreview,
    GetPreview,
    ClosePreview,
    GetPreviewImage,
    GetPreviewRecommendations,
    BrowsePreviewLibrary,
    SubmitMosaicJob,
    SubmitBuildJob,
    ListJobs,
    ClearFinishedJobs,
    GetJob,
    GetJobEvents,
    GetJobArtifact,
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
    JobQuery,
    JobArtifact,

};

struct ApiErrorResponseMetadata
{
    int status = 500;
    std::string shape;
    std::string responseKey;
};

struct ApiEntryPointMetadata
{
    std::string name;
    std::string executable;
    std::string role;
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
    int successStatus = 200;
    std::string responseKey;
    std::vector<int> errorStatuses;
    std::vector<std::string> errorShapes;
    std::vector<std::string> errorResponseKeys;
    std::vector<ApiErrorResponseMetadata> errorResponses;
    bool sideEffects = false;
    bool longRunning = false;
    bool enabled = true;
};

const char* apiOperationName(ApiOperation operation);
const char* apiRequestShapeName(ApiRequestShape shape);
const char* apiContractVersion();
int apiContractMajorVersion();
const char* apiCompatibilityLevel();
bool apiContractStable();
std::vector<ApiEndpointMetadata> apiEndpointMetadata();
std::vector<std::string> validateApiEndpointMetadata(const std::vector<ApiEndpointMetadata>& endpoints);
std::vector<ApiEntryPointMetadata> apiEntryPointMetadata();
std::vector<std::string> apiFeatureList();

} // namespace mosaicraft
