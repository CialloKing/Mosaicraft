#include "ApiMetadata.h"

#include <algorithm>
#include "Version.h"
#include <initializer_list>
#include <set>
#include <unordered_map>

namespace mosaicraft
{

namespace
{

std::vector<std::string> endpointMethods(const std::string& method)
{
    if (method == "GET|POST") return {"GET", "POST"};
    return {method};
}

std::string endpointHttpPattern(const std::string& path)
{
    if (path == "/api/db/catalog/{id}")
    {
        return R"(/api/db/catalog/([A-Za-z0-9_-]+))";
    }
    if (path == "/api/db/catalog/{id}/image/{asset}")
    {
        return R"(/api/db/catalog/([A-Za-z0-9_-]+)/image/([A-Za-z0-9_-]+))";
    }
    if (path == "/api/jobs/{id}/events")
    {
        return R"(/api/jobs/([A-Za-z0-9_-]+)/events)";
    }
    if (path == "/api/jobs/{id}/artifacts/{artifactId}")
    {
        return R"(/api/jobs/([A-Za-z0-9_-]+)/artifacts/([A-Za-z0-9_-]+))";
    }
    if (path == "/api/jobs/{id}") {
        return R"(/api/jobs/([A-Za-z0-9_-]+))";
    }
    if (path == "/api/mosaic/preview/{id}")
    {
        return R"(/api/mosaic/preview/([A-Za-z0-9_-]+))";
    }
    if (path == "/api/mosaic/preview/{id}/image/{asset}")
    {
        return R"(/api/mosaic/preview/([A-Za-z0-9_-]+)/image/([A-Za-z0-9_-]+))";
    }
    if (path == "/api/mosaic/preview/{id}/recommendations")
    {
        return R"(/api/mosaic/preview/([A-Za-z0-9_-]+)/recommendations)";
    }
    if (path == "/api/mosaic/preview/{id}/library")
    {
        return R"(/api/mosaic/preview/([A-Za-z0-9_-]+)/library)";
    }
    return path;
}

std::vector<std::string> endpointQueryKeys(ApiOperation operation)
{
    switch (operation)
    {
    case ApiOperation::GetCatalog:
        return {"offset"};
    case ApiOperation::GetJobEvents:
        return {"after", "limit"};
    case ApiOperation::GetPreviewRecommendations:
        return {"position"};
    case ApiOperation::BrowsePreviewLibrary:
        return {"q", "offset"};
    case ApiOperation::DatabaseStats:
    case ApiOperation::DatabaseHealth:
        return {"dbPath", "deep"};
    case ApiOperation::DatabaseUsage:
        return {"dbPath", "limit", "showUnused", "sort"};
    case ApiOperation::DatabaseUsageExport:
        return {"dbPath", "outputDir", "confirm", "selection"};
    case ApiOperation::DatabasePurge:
        return {"dbPath", "dryRun", "confirm"};
    case ApiOperation::Inspect:
        return {"imagePath", "dbPath"};
    case ApiOperation::ProjectInfo:
        return {};
    case ApiOperation::Endpoints:
    case ApiOperation::Info:
    case ApiOperation::Ping:
    case ApiOperation::Mosaic:
    case ApiOperation::PinComparison:
    case ApiOperation::SubmitComparison:
    case ApiOperation::ListHistory:
    case ApiOperation::ClearHistory:
    case ApiOperation::PreviewQuality:
    case ApiOperation::MosaicCoverage:
    case ApiOperation::OpenCatalog:
    case ApiOperation::GetCatalogImage:
    case ApiOperation::PlanMosaic:
    case ApiOperation::SubmitMosaicSample:
    case ApiOperation::SubmitPreview:
    case ApiOperation::GetPreview:
    case ApiOperation::ClosePreview:
    case ApiOperation::GetPreviewImage:
    case ApiOperation::SubmitMosaicJob:
    case ApiOperation::SubmitBuildJob:
    case ApiOperation::SubmitRenderJob:
    case ApiOperation::SubmitProjectCheckJob:
    case ApiOperation::SubmitProjectPackJob:
    case ApiOperation::ListJobs:
    case ApiOperation::ClearFinishedJobs:
    case ApiOperation::GetJobArtifact:
    case ApiOperation::GetJob:
    case ApiOperation::CancelJob:
        return {};
    }
    return {};
}

void addUnique(std::vector<std::string>& values, const std::string& value)
{
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

std::vector<std::string> endpointAcceptedQueryKeys(const std::vector<std::string>& queryKeys,
                                                   const std::vector<std::string>& requestFields,
                                                   ApiRequestShape requestShape)
{
    std::vector<std::string> keys;
    for (const auto& key : queryKeys) addUnique(keys, key);
    if ((requestShape == ApiRequestShape::Query || requestShape == ApiRequestShape::JobQuery))
    {
        for (const auto& field : requestFields) addUnique(keys, field);

    }
    return keys;
}

int endpointSuccessStatus(ApiOperation operation)
{
    switch (operation)
    {
    case ApiOperation::SubmitPreview:
    case ApiOperation::SubmitComparison:
    case ApiOperation::SubmitMosaicSample:
    case ApiOperation::SubmitMosaicJob:
    case ApiOperation::SubmitBuildJob:
    case ApiOperation::SubmitRenderJob:
    case ApiOperation::SubmitProjectCheckJob:
    case ApiOperation::SubmitProjectPackJob:
        return 202;
    case ApiOperation::Endpoints:
    case ApiOperation::Info:
    case ApiOperation::Ping:
    case ApiOperation::Mosaic:
    case ApiOperation::PinComparison:
    case ApiOperation::ListHistory:
    case ApiOperation::ClearHistory:
    case ApiOperation::PreviewQuality:
    case ApiOperation::MosaicCoverage:
    case ApiOperation::OpenCatalog:
    case ApiOperation::GetCatalog:
    case ApiOperation::GetCatalogImage:
    case ApiOperation::PlanMosaic:
    case ApiOperation::GetPreview:
    case ApiOperation::ClosePreview:
    case ApiOperation::GetPreviewImage:
    case ApiOperation::GetPreviewRecommendations:
    case ApiOperation::BrowsePreviewLibrary:
    case ApiOperation::ListJobs:
    case ApiOperation::ClearFinishedJobs:
    case ApiOperation::GetJobEvents:
    case ApiOperation::GetJobArtifact:
    case ApiOperation::GetJob:
    case ApiOperation::CancelJob:
    case ApiOperation::DatabaseStats:
    case ApiOperation::DatabaseHealth:
    case ApiOperation::DatabaseUsage:
    case ApiOperation::DatabaseUsageExport:
    case ApiOperation::DatabasePurge:
    case ApiOperation::ProjectInfo:
    case ApiOperation::Inspect:
        return 200;
    }
    return 200;
}

std::string endpointResponseKey(ApiOperation operation)
{
    switch (operation)
    {
    case ApiOperation::SubmitPreview:
    case ApiOperation::SubmitComparison:
    case ApiOperation::SubmitMosaicSample:
    case ApiOperation::GetPreview:
        return "preview";
    case ApiOperation::ClosePreview:
    case ApiOperation::GetPreviewImage:
        return "";
    case ApiOperation::GetPreviewRecommendations:
        return "recommendations";
    case ApiOperation::BrowsePreviewLibrary:
        return "library";
    case ApiOperation::GetJobEvents:
        return "events";
    case ApiOperation::GetJobArtifact:
        return "artifact";
    case ApiOperation::Endpoints: return "endpoints";
    case ApiOperation::Info: return "info";
    case ApiOperation::Ping: return "message";
    case ApiOperation::PlanMosaic: return "plan";
    case ApiOperation::MosaicCoverage: return "coverage";
    case ApiOperation::PreviewQuality: return "quality";
    case ApiOperation::PinComparison: return "";
    case ApiOperation::ListHistory: return "history";
    case ApiOperation::ClearHistory: return "removed";
    case ApiOperation::OpenCatalog:
    case ApiOperation::GetCatalog: return "catalog";
    case ApiOperation::GetCatalogImage: return "";
    case ApiOperation::SubmitMosaicJob:
    case ApiOperation::SubmitBuildJob:
    case ApiOperation::SubmitRenderJob:
    case ApiOperation::SubmitProjectCheckJob:
    case ApiOperation::SubmitProjectPackJob:
    case ApiOperation::GetJob:
    case ApiOperation::CancelJob:
        return "job";
    case ApiOperation::ListJobs: return "jobs";
    case ApiOperation::ClearFinishedJobs: return "removed";
    case ApiOperation::DatabaseStats: return "stats";
    case ApiOperation::DatabaseHealth: return "health";
    case ApiOperation::DatabaseUsage: return "usage";
    case ApiOperation::DatabaseUsageExport: return "export";
    case ApiOperation::DatabasePurge: return "purge";
    case ApiOperation::ProjectInfo:
        return "project";
    case ApiOperation::Inspect: return "inspect";
    case ApiOperation::Mosaic:
        return "";
    }
    return "";
}

void addUniqueStatus(std::vector<int>& values, int value)
{
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void addErrorContract(ApiEndpointMetadata& info, int status, const std::string& shape,
                      const std::string& responseKey = {})
{
    addUniqueStatus(info.errorStatuses, status);
    addUnique(info.errorShapes, shape);
    addUnique(info.errorResponseKeys, responseKey);
    const auto duplicate = std::find_if(info.errorResponses.begin(), info.errorResponses.end(),
        [&](const ApiErrorResponseMetadata& item) {
            return item.status == status && item.shape == shape && item.responseKey == responseKey;
        });
    if (duplicate == info.errorResponses.end()) {
        info.errorResponses.push_back({status, shape, responseKey});
    }
}

void populateEndpointErrors(ApiEndpointMetadata& info)
{
    switch (info.operation)
    {
    case ApiOperation::SubmitPreview:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 500, "serviceResult");
        break;
    case ApiOperation::GetPreview:
    case ApiOperation::ClosePreview:
        addErrorContract(info, 404, "serviceResult");
        break;
    case ApiOperation::GetCatalog:
    case ApiOperation::GetCatalogImage:
    case ApiOperation::GetPreviewImage:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 404, "serviceResult");
        addErrorContract(info, 409, "serviceResult");
        addErrorContract(info, 500, "serviceResult");
        break;
    case ApiOperation::GetPreviewRecommendations:
    case ApiOperation::BrowsePreviewLibrary:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 404, "serviceResult");
        break;
    case ApiOperation::GetJobEvents:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 404, "serviceResult");
        break;
    case ApiOperation::GetJobArtifact:
        addErrorContract(info, 404, "serviceResult");
        addErrorContract(info, 409, "serviceResult");
        addErrorContract(info, 500, "serviceResult");
        break;
    case ApiOperation::SubmitMosaicJob:
    case ApiOperation::SubmitMosaicSample:
    case ApiOperation::PinComparison:
    case ApiOperation::SubmitComparison:
    case ApiOperation::PreviewQuality:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 409, "serviceResult");
        addErrorContract(info, 500, "serviceResult");
        break;
    case ApiOperation::Mosaic:
    case ApiOperation::MosaicCoverage:
    case ApiOperation::OpenCatalog:
    case ApiOperation::ListHistory:
    case ApiOperation::ClearHistory:
    case ApiOperation::PlanMosaic:
    case ApiOperation::SubmitBuildJob:
    case ApiOperation::SubmitRenderJob:
    case ApiOperation::SubmitProjectCheckJob:
    case ApiOperation::SubmitProjectPackJob:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 500, "serviceResult");
        break;
    case ApiOperation::GetJob:
        addErrorContract(info, 404, "serviceResult");
        break;
    case ApiOperation::CancelJob:
        addErrorContract(info, 404, "serviceResult");
        addErrorContract(info, 409, "serviceResult", "job");
        break;
    case ApiOperation::DatabaseStats:
    case ApiOperation::DatabaseHealth:
    case ApiOperation::DatabaseUsage:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 500, "serviceResult");
        break;
    case ApiOperation::DatabaseUsageExport:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 400, "serviceResult", "export");
        addErrorContract(info, 500, "serviceResult", "export");
        break;
    case ApiOperation::DatabasePurge:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 400, "serviceResult", "purge");
        addErrorContract(info, 500, "serviceResult", "purge");
        break;
    case ApiOperation::ProjectInfo:
    case ApiOperation::Inspect:
        addErrorContract(info, 400, "serviceResult");
        break;
    case ApiOperation::Endpoints:
    case ApiOperation::Info:
    case ApiOperation::Ping:
    case ApiOperation::ListJobs:
    case ApiOperation::ClearFinishedJobs:
        break;
    }
}

ApiEndpointMetadata endpoint(const std::string& method,
                             const std::string& path,
                             const std::string& description,
                             const std::string& category,
                             ApiOperation operation,
                             ApiRequestShape requestShape,
                             std::initializer_list<const char*> requestFields = {},
                             std::initializer_list<const char*> requiredFields = {},
                             bool sideEffects = false,
                             bool longRunning = false,
                                                          bool enabled = true)
{
    ApiEndpointMetadata info;
    info.operation = operation;
    info.requestShape = requestShape;
    info.method = method;
    info.methods = endpointMethods(method);
    info.path = path;
    info.httpPattern = endpointHttpPattern(path);
    info.description = description;
    info.category = category;
    info.enabled = enabled;
    info.successStatus = endpointSuccessStatus(operation);
    info.responseKey = endpointResponseKey(operation);
    info.sideEffects = sideEffects;
    info.longRunning = longRunning;
    info.queryKeys = endpointQueryKeys(operation);
    for (const char* field : requestFields) {
        info.requestFields.emplace_back(field);
    }
    for (const char* field : requiredFields) {
        info.requiredFields.emplace_back(field);
    }
    info.acceptedQueryKeys = endpointAcceptedQueryKeys(
        info.queryKeys, info.requestFields, info.requestShape);
    populateEndpointErrors(info);
    return info;
}

} // namespace

const char* apiOperationName(ApiOperation operation)
{
    switch (operation)
    {
    case ApiOperation::Endpoints: return "endpoints";
    case ApiOperation::Info: return "info";
    case ApiOperation::Ping: return "ping";
    case ApiOperation::Mosaic: return "mosaic";
    case ApiOperation::PlanMosaic: return "planMosaic";
    case ApiOperation::MosaicCoverage: return "mosaicCoverage";
    case ApiOperation::PreviewQuality: return "previewQuality";
    case ApiOperation::PinComparison: return "pinComparison";
    case ApiOperation::SubmitComparison: return "submitComparison";
    case ApiOperation::ListHistory: return "listHistory";
    case ApiOperation::ClearHistory: return "clearHistory";
    case ApiOperation::OpenCatalog: return "openCatalog";
    case ApiOperation::GetCatalog: return "getCatalog";
    case ApiOperation::GetCatalogImage: return "getCatalogImage";
    case ApiOperation::SubmitMosaicSample: return "submitMosaicSample";
    case ApiOperation::SubmitPreview: return "submitPreview";
    case ApiOperation::GetPreview: return "getPreview";
    case ApiOperation::ClosePreview: return "closePreview";
    case ApiOperation::GetPreviewImage: return "getPreviewImage";
    case ApiOperation::GetPreviewRecommendations: return "getPreviewRecommendations";
    case ApiOperation::BrowsePreviewLibrary: return "browsePreviewLibrary";
    case ApiOperation::SubmitMosaicJob: return "submitMosaicJob";
    case ApiOperation::SubmitBuildJob: return "submitBuildJob";
    case ApiOperation::ProjectInfo:
        return "projectInfo";
    case ApiOperation::SubmitRenderJob:
        return "submitRenderJob";
    case ApiOperation::SubmitProjectCheckJob:
        return "submitProjectCheckJob";
    case ApiOperation::SubmitProjectPackJob:
        return "submitProjectPackJob";
    case ApiOperation::ListJobs: return "listJobs";
    case ApiOperation::ClearFinishedJobs: return "clearFinishedJobs";
    case ApiOperation::GetJobEvents:
        return "getJobEvents";
    case ApiOperation::GetJobArtifact:
        return "getJobArtifact";
    case ApiOperation::GetJob: return "getJob";
    case ApiOperation::CancelJob: return "cancelJob";
    case ApiOperation::DatabaseStats: return "databaseStats";
    case ApiOperation::DatabaseHealth: return "databaseHealth";
    case ApiOperation::DatabaseUsage: return "databaseUsage";
    case ApiOperation::DatabaseUsageExport: return "databaseUsageExport";
    case ApiOperation::DatabasePurge: return "databasePurge";
    case ApiOperation::Inspect: return "inspect";
    }
    return "unknown";
}

const char* apiRequestShapeName(ApiRequestShape shape)
{
    switch (shape)
    {
    case ApiRequestShape::None: return "none";
    case ApiRequestShape::Body: return "body";
    case ApiRequestShape::Query: return "query";
    case ApiRequestShape::JobId: return "jobId";
    case ApiRequestShape::JobQuery:
        return "jobQuery";
    case ApiRequestShape::JobArtifact:
        return "jobArtifact";
    }
    return "unknown";
}

const char* apiContractVersion()
{
    return kApiVersion;
}

int apiContractMajorVersion()
{
    return kApiMajorVersion;
}

const char* apiCompatibilityLevel()
{
    return "stable";
}

bool apiContractStable()
{
    return true;
}

std::vector<ApiEndpointMetadata> apiEndpointMetadata()
{
    const std::initializer_list<const char *> mosaicFields = {"inputPath",
                                                              "dbPath",
                                                              "outputPath",
                                                              "tileW",
                                                              "tileH",
                                                              "outW",
                                                              "outH",
                                                              "nativeTileW",
                                                              "nativeTileH",
                                                              "candidates",
                                                              "topNRandom",
                                                              "neighborWindow",
                                                              "upscale",
                                                              "quality",
                                                              "pngLevel",
                                                              "lRange",
                                                              "usePenalty",
                                                              "labWeight",
                                                              "gridWeight",
                                                              "tinyWeight",
                                                              "edgeWeight",
                                                              "lbpWeight",
                                                              "neighborPenalty",
                                                              "colorStrength",
                                                              "outputMode",
                                                              "backend",
                                                              "threads",
                                                              "seed",
                                                              "overwrite",
                                                              "overlayStrength",
                                                              "format",
                                                              "writeMode",
                                                              "useGpu",
                                                              "tiled",
                                                              "deepZoom",
                                                              "colorAdjust",
                                                              "adaptiveWeights",
                                                              "analyze",
                                                              "benchmark",
                                                              "generatePreview",
                                                              "resourceMonitor"};
    auto sample = endpoint("POST", "/api/mosaic/sample-size", "sample output using a completed preview", "preview",
                           ApiOperation::SubmitMosaicSample, ApiRequestShape::Body,
                           mosaicFields, {"inputPath", "previewId"}, true, true);
    sample.requestFields.push_back("previewId");
    sample.requestFields.push_back("previewEdits");
    return {endpoint("POST", "/api/project/info", "read portable project metadata", "project",
                     ApiOperation::ProjectInfo, ApiRequestShape::Body, {"projectPath"}, {"projectPath"}),
            endpoint("POST", "/api/jobs/render", "render a project without matching", "jobs",
                     ApiOperation::SubmitRenderJob, ApiRequestShape::Body,
                     {"projectPath", "outputPath", "outputMode", "format", "nativeTileW", "nativeTileH", "quality",
                      "pngLevel", "writeMode", "generatePreview", "overlayStrength", "brightnessJitter", "seed",
                      "threads", "benchmark", "overwrite", "resourceMonitor"},
                     {"projectPath", "outputPath"}, true, true),
            endpoint("POST", "/api/jobs/project-check", "verify portable project integrity", "jobs",
                     ApiOperation::SubmitProjectCheckJob, ApiRequestShape::Body, {"projectPath", "deep"},
                     {"projectPath"}, false, true),
            endpoint("POST", "/api/jobs/project-pack", "stream a complete ZIP64 project archive", "jobs",
                     ApiOperation::SubmitProjectPackJob, ApiRequestShape::Body,
                     {"projectPath", "outputPath", "overwrite", "deep"}, {"projectPath", "outputPath"}, true, true),
            endpoint("GET", "/api/endpoints", "structured API endpoint discovery", "discovery", ApiOperation::Endpoints,
                     ApiRequestShape::None),
            endpoint("GET", "/api/info", "service version and capability summary", "discovery", ApiOperation::Info,
                     ApiRequestShape::None),
            endpoint("GET", "/api/ping", "health check", "health", ApiOperation::Ping, ApiRequestShape::None),
            endpoint("POST", "/api/mosaic", "run mosaic synchronously", "mosaic", ApiOperation::Mosaic,
                     ApiRequestShape::Body, mosaicFields, {"inputPath"}, true, true),
            endpoint("POST", "/api/mosaic/plan", "resolve the mosaic geometry and estimate resources", "mosaic",
                     ApiOperation::PlanMosaic, ApiRequestShape::Body, mosaicFields, {"inputPath"}),
            std::move(sample),
            endpoint("POST", "/api/mosaic/coverage", "measure target and library color coverage", "mosaic",
                     ApiOperation::MosaicCoverage, ApiRequestShape::Body, mosaicFields, {"inputPath"}),
            endpoint("POST", "/api/db/catalog", "open an ordered library catalog", "database",
                     ApiOperation::OpenCatalog, ApiRequestShape::Body, {"dbPath", "sort", "anchor"},
                     {"dbPath", "sort"}),
            endpoint("GET", "/api/db/catalog/{id}", "read a catalog page", "database", ApiOperation::GetCatalog,
                     ApiRequestShape::JobQuery, {"id", "offset"}, {"id"}),
            endpoint("GET", "/api/db/catalog/{id}/image/{asset}", "read a catalog thumbnail", "database",
                     ApiOperation::GetCatalogImage, ApiRequestShape::JobArtifact, {"id", "asset"}, {"id", "asset"}),
            endpoint("POST", "/api/mosaic/preview/pin", "pin a completed preview in an A/B slot", "preview",
                     ApiOperation::PinComparison, ApiRequestShape::Body, {"slot", "previewId"}, {"slot", "previewId"},
                     true),
            endpoint("POST", "/api/mosaic/preview/compare", "compare two aligned previews", "preview",
                     ApiOperation::SubmitComparison, ApiRequestShape::Body, {"a", "b"}, {"a", "b"}, true, true),
            endpoint("GET", "/api/history", "read local completed-task summaries", "jobs", ApiOperation::ListHistory,
                     ApiRequestShape::None),
            endpoint("DELETE", "/api/history", "clear the local task summaries", "jobs", ApiOperation::ClearHistory,
                     ApiRequestShape::None, {}, {}, true),
            endpoint("POST", "/api/mosaic/preview/quality", "inspect selected tiles and candidate features", "preview",
                     ApiOperation::PreviewQuality, ApiRequestShape::Body, {"previewId", "positions", "explain"},
                     {"previewId", "positions"}),
            endpoint("POST", "/api/mosaic/preview", "queue the latest interactive selection", "preview",
                     ApiOperation::SubmitPreview, ApiRequestShape::Body, {"request", "seed"}, {"request", "seed"}, true,
                     true),
            endpoint("GET", "/api/mosaic/preview/{id}", "read interactive selection status and grid", "preview",
                     ApiOperation::GetPreview, ApiRequestShape::JobId, {"id"}, {"id"}),
            endpoint("DELETE", "/api/mosaic/preview/{id}", "cancel or release interactive selection", "preview",
                     ApiOperation::ClosePreview, ApiRequestShape::JobId, {"id"}, {"id"}, true),
            endpoint("GET", "/api/mosaic/preview/{id}/image/{asset}", "read a library thumbnail or target reference",
                     "preview", ApiOperation::GetPreviewImage, ApiRequestShape::JobArtifact, {"id", "asset"},
                     {"id", "asset"}),
            endpoint("GET", "/api/mosaic/preview/{id}/recommendations", "read the highest-ranked images for one tile",
                     "preview", ApiOperation::GetPreviewRecommendations, ApiRequestShape::JobQuery, {"id", "position"},
                     {"id", "position"}),
            endpoint("GET", "/api/mosaic/preview/{id}/library", "browse the preview library by name or ID", "preview",
                     ApiOperation::BrowsePreviewLibrary, ApiRequestShape::JobQuery, {"id", "q", "offset"}, {"id"}),
            endpoint("POST", "/api/jobs/mosaic", "start mosaic job", "jobs", ApiOperation::SubmitMosaicJob,
                     ApiRequestShape::Body,
                     {"inputPath",
                      "dbPath",
                      "outputPath",
                      "tileW",
                      "tileH",
                      "outW",
                      "outH",
                      "nativeTileW",
                      "nativeTileH",
                      "candidates",
                      "topNRandom",
                      "neighborWindow",
                      "upscale",
                      "quality",
                      "pngLevel",
                      "lRange",
                      "usePenalty",
                      "labWeight",
                      "gridWeight",
                      "tinyWeight",
                      "edgeWeight",
                      "lbpWeight",
                      "neighborPenalty",
                      "colorStrength",
                      "overlayStrength",
                      "format",
                      "writeMode",
                      "useGpu",
                      "tiled",
                      "deepZoom",
                      "colorAdjust",
                      "adaptiveWeights",
                      "analyze",
                      "benchmark",
                      "generatePreview",
                      "previewId",
                      "previewEdits",
                      "resourceMonitor",
                      "outputMode",
                      "backend",
                      "threads",
                      "seed",
                      "overwrite"},
                     {"inputPath"}, true, true),
            endpoint("POST", "/api/jobs/build", "start library build job", "jobs", ApiOperation::SubmitBuildJob,
                     ApiRequestShape::Body,
                     {"inputDir", "outputDir", "dbPath", "threads", "normalizeWidth", "normalizeHeight", "appendMode",
                      "recursive", "normalizeOnly", "forceMode", "resourceMonitor", "backend"},
                     {"inputDir"}, true, true),
            endpoint("GET", "/api/jobs", "list jobs", "jobs", ApiOperation::ListJobs, ApiRequestShape::None),
            endpoint("DELETE", "/api/jobs", "clear finished jobs", "jobs", ApiOperation::ClearFinishedJobs,
                     ApiRequestShape::None, {}, {}, true),
            endpoint("GET", "/api/jobs/{id}/events", "read bounded incremental diagnostics", "jobs",
                     ApiOperation::GetJobEvents, ApiRequestShape::JobQuery, {"id", "after", "limit"}, {"id"}),
            endpoint("GET", "/api/jobs/{id}/artifacts/{artifactId}", "read a registered closed artifact", "jobs",
                     ApiOperation::GetJobArtifact, ApiRequestShape::JobArtifact, {"id", "artifactId"},
                     {"id", "artifactId"}),
            endpoint("GET", "/api/jobs/{id}", "get job status", "jobs", ApiOperation::GetJob, ApiRequestShape::JobId,
                     {"id"}, {"id"}),
            endpoint("DELETE", "/api/jobs/{id}", "cancel queued job", "jobs", ApiOperation::CancelJob,
                     ApiRequestShape::JobId, {"id"}, {"id"}, true),
            endpoint("GET|POST", "/api/db/stats", "database statistics", "database", ApiOperation::DatabaseStats,
                     ApiRequestShape::Query, {"dbPath", "deep"}),
            endpoint("GET|POST", "/api/db/health", "database health report", "database", ApiOperation::DatabaseHealth,
                     ApiRequestShape::Query, {"dbPath", "deep"}),
            endpoint("GET|POST", "/api/db/usage", "database usage report", "database", ApiOperation::DatabaseUsage,
                     ApiRequestShape::Query, {"dbPath", "limit", "showUnused", "sort"}),
            endpoint("POST", "/api/db/usage/export", "export used images", "database",
                     ApiOperation::DatabaseUsageExport, ApiRequestShape::Query,
                     {"dbPath", "outputDir", "confirm", "selection"}, {"outputDir", "confirm"}, true),
            endpoint("POST", "/api/db/purge", "preview or purge orphan records", "database",
                     ApiOperation::DatabasePurge, ApiRequestShape::Query, {"dbPath", "dryRun", "confirm"}, {}, true),
            endpoint("GET|POST", "/api/inspect", "inspect a source image", "inspect", ApiOperation::Inspect,
                     ApiRequestShape::Query, {"imagePath", "dbPath"}, {"imagePath"})};
}

std::vector<std::string> validateApiEndpointMetadata(const std::vector<ApiEndpointMetadata>& endpoints)
{
    std::vector<std::string> errors;
    std::set<std::string> routes;
    std::set<std::string> operations;

    for (const auto& endpoint : endpoints) {
        const std::string operationName = apiOperationName(endpoint.operation);
        const std::string shapeName = apiRequestShapeName(endpoint.requestShape);

        if (operationName == "unknown") {
            errors.push_back("endpoint has unknown operation");
        } else if (!operations.insert(operationName).second) {
            errors.push_back("duplicate API operation: " + operationName);
        }

        if (shapeName == "unknown") {
            errors.push_back("endpoint has unknown request shape: " + operationName);
        }
        if (endpoint.method.empty()) {
            errors.push_back("endpoint has empty method: " + operationName);
        }
        if (endpoint.methods.empty()) {
            errors.push_back("endpoint has no methods: " + operationName);
        }
        if (endpoint.path.empty()) {
            errors.push_back("endpoint has empty path: " + operationName);
        }
        if (endpoint.httpPattern.empty()) {
            errors.push_back("endpoint has empty httpPattern: " + operationName);
        }
        if (endpoint.description.empty()) {
            errors.push_back("endpoint has empty description: " + operationName);
        }
        if (endpoint.category.empty()) {
            errors.push_back("endpoint has empty category: " + operationName);
        }
        if (endpoint.successStatus < 200 || endpoint.successStatus > 299) {
            errors.push_back("endpoint has non-success successStatus: " + operationName);
        }
        for (int status : endpoint.errorStatuses) {
            if (status < 400 || status > 599) {
                errors.push_back("endpoint has non-error errorStatus: " + operationName);
            }
        }
        for (const auto& response : endpoint.errorResponses) {
            if (response.status < 400 || response.status > 599) {
                errors.push_back("endpoint has non-error errorResponse status: " + operationName);
            }
            if (std::find(endpoint.errorStatuses.begin(), endpoint.errorStatuses.end(), response.status) ==
                endpoint.errorStatuses.end()) {
                errors.push_back("endpoint errorResponses status missing from errorStatuses: " + operationName);
            }
            if (std::find(endpoint.errorShapes.begin(), endpoint.errorShapes.end(), response.shape) ==
                endpoint.errorShapes.end()) {
                errors.push_back("endpoint errorResponses shape missing from errorShapes: " + operationName);
            }
            if (!response.responseKey.empty() &&
                std::find(endpoint.errorResponseKeys.begin(), endpoint.errorResponseKeys.end(), response.responseKey) ==
                endpoint.errorResponseKeys.end()) {
                errors.push_back("endpoint errorResponses key missing from errorResponseKeys: " + operationName);
            }
        }
        for (const auto& shape : endpoint.errorShapes) {
            if (shape != "serviceResult" && shape != "serviceResult" &&
                shape != "serviceResult" && shape != "serviceResult") {
                errors.push_back("endpoint has unsupported errorShape: " + operationName + " " + shape);
            }
        }
        if ((endpoint.requestShape == ApiRequestShape::Query || endpoint.requestShape == ApiRequestShape::JobQuery) &&
            endpoint.queryKeys.empty())
        {
            errors.push_back("query endpoint has no queryKeys: " + operationName);
        }
        if ((endpoint.requestShape == ApiRequestShape::Query || endpoint.requestShape == ApiRequestShape::JobQuery) &&
            endpoint.acceptedQueryKeys.empty())
        {
            errors.push_back("query endpoint has no acceptedQueryKeys: " + operationName);
        }
        if ((endpoint.requestShape != ApiRequestShape::Query && endpoint.requestShape != ApiRequestShape::JobQuery) &&
            !endpoint.queryKeys.empty())
        {
            errors.push_back("non-query endpoint has queryKeys: " + operationName);
        }
        if ((endpoint.requestShape != ApiRequestShape::Query && endpoint.requestShape != ApiRequestShape::JobQuery) &&
            !endpoint.acceptedQueryKeys.empty())
        {
            errors.push_back("non-query endpoint has acceptedQueryKeys: " + operationName);
        }
        for (const auto& key : endpoint.queryKeys) {
            if (std::find(endpoint.acceptedQueryKeys.begin(), endpoint.acceptedQueryKeys.end(), key) ==
                endpoint.acceptedQueryKeys.end()) {
                errors.push_back("acceptedQueryKeys missing query key: " + operationName + " " + key);
            }
        }
        for (const auto& field : endpoint.requiredFields) {
            if (std::find(endpoint.requestFields.begin(), endpoint.requestFields.end(), field) ==
                endpoint.requestFields.end()) {
                errors.push_back("required field is not listed in requestFields: " + operationName + " " + field);
            }
        }

        for (const auto& method : endpoint.methods) {
            if (method != "GET" && method != "POST" && method != "DELETE") {
                errors.push_back("endpoint has unsupported method: " + operationName + " " + method);
                continue;
            }
            const std::string routeKey = method + " " + endpoint.httpPattern;
            if (!routes.insert(routeKey).second) {
                errors.push_back("duplicate API route: " + routeKey);
            }
        }
    }

    return errors;
}

std::vector<std::string> apiFeatureList()
{
    return {"preview-comparison",   "task-history",     "task-resources",
            "preview-quality",      "library-visuals",  "mosaic-plan",
            "mosaic-size-sample",   "mosaic-jobs",      "build-jobs",
            "job-management",       "job-diagnostics",  "job-progress",
            "job-artifacts",        "result-preview",   "interactive-preview",
            "preview-editing",      "inspect",          "database-stats",
            "database-health",      "database-usage",   "database-maintenance",
            "output-format-policy", "portable-project", "project-render",
            "project-check",        "project-pack"};
}

std::vector<ApiEntryPointMetadata> apiEntryPointMetadata()
{
    return {
        {"cli", "mosaicraft.exe", "command line entry"},
        {"webui", "MosaicraftWebUI.exe", "local Web UI and structured HTTP API entry"}
    };
}

} // namespace mosaicraft
