#include "ApiMetadata.h"

#include <initializer_list>

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
    if (path == "/api/jobs/{id}") {
        return R"(/api/jobs/([A-Za-z0-9_-]+))";
    }
    return path;
}

ApiEndpointMetadata endpoint(const std::string& method,
                             const std::string& path,
                             const std::string& description,
                             const std::string& category,
                             ApiOperation operation,
                             ApiRequestShape requestShape,
                             std::initializer_list<const char*> requestFields = {},
                             bool legacy = false,
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
    info.legacy = legacy;
    info.enabled = enabled;
    for (const char* field : requestFields) {
        info.requestFields.emplace_back(field);
    }
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
    case ApiOperation::LegacyRunDisabled: return "legacyRunDisabled";
    case ApiOperation::Mosaic: return "mosaic";
    case ApiOperation::SubmitMosaicJob: return "submitMosaicJob";
    case ApiOperation::SubmitBuildJob: return "submitBuildJob";
    case ApiOperation::ListJobs: return "listJobs";
    case ApiOperation::ClearFinishedJobs: return "clearFinishedJobs";
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
    case ApiRequestShape::LegacyCommand: return "legacyCommand";
    }
    return "unknown";
}

std::vector<ApiEndpointMetadata> apiEndpointMetadata(bool legacyRunEnabled)
{
    return {
        endpoint("GET", "/api/endpoints", "structured API endpoint discovery", "discovery",
            ApiOperation::Endpoints, ApiRequestShape::None),
        endpoint("GET", "/api/info", "service version and capability summary", "discovery",
            ApiOperation::Info, ApiRequestShape::None),
        endpoint("GET", "/api/ping", "health check", "health",
            ApiOperation::Ping, ApiRequestShape::None),
        endpoint("POST", "/api/mosaic", "run mosaic synchronously", "mosaic",
            ApiOperation::Mosaic, ApiRequestShape::Body,
            {"inputPath", "dbPath", "outputPath", "format", "quality", "writeMode"}),
        endpoint("POST", "/api/jobs/mosaic", "start mosaic job", "jobs",
            ApiOperation::SubmitMosaicJob, ApiRequestShape::Body,
            {"inputPath", "dbPath", "outputPath", "format", "quality", "writeMode"}),
        endpoint("POST", "/api/jobs/build", "start library build job", "jobs",
            ApiOperation::SubmitBuildJob, ApiRequestShape::Body,
            {"inputDir", "outputDir", "dbPath", "threads", "recursive", "forceMode"}),
        endpoint("GET", "/api/jobs", "list jobs", "jobs",
            ApiOperation::ListJobs, ApiRequestShape::None),
        endpoint("DELETE", "/api/jobs", "clear finished jobs", "jobs",
            ApiOperation::ClearFinishedJobs, ApiRequestShape::None),
        endpoint("GET", "/api/jobs/{id}", "get job status", "jobs",
            ApiOperation::GetJob, ApiRequestShape::JobId, {"id"}),
        endpoint("DELETE", "/api/jobs/{id}", "cancel queued job", "jobs",
            ApiOperation::CancelJob, ApiRequestShape::JobId, {"id"}),
        endpoint("GET|POST", "/api/db/stats", "database statistics", "database",
            ApiOperation::DatabaseStats, ApiRequestShape::Query, {"dbPath"}),
        endpoint("GET|POST", "/api/db/health", "database health report", "database",
            ApiOperation::DatabaseHealth, ApiRequestShape::Query, {"dbPath"}),
        endpoint("GET|POST", "/api/db/usage", "database usage report", "database",
            ApiOperation::DatabaseUsage, ApiRequestShape::Query,
            {"dbPath", "limit", "showUnused"}),
        endpoint("POST", "/api/db/usage/export", "export used images", "database",
            ApiOperation::DatabaseUsageExport, ApiRequestShape::Query,
            {"dbPath", "outputDir", "confirm"}),
        endpoint("GET|POST", "/api/db/purge", "preview or purge orphan records", "database",
            ApiOperation::DatabasePurge, ApiRequestShape::Query,
            {"dbPath", "dryRun", "confirm"}),
        endpoint("GET|POST", "/api/inspect", "inspect a source image", "inspect",
            ApiOperation::Inspect, ApiRequestShape::Query,
            {"imagePath", "dbPath"}),
        endpoint("POST", "/api/run", "legacy command compatibility endpoint", "legacy",
            ApiOperation::LegacyRunDisabled, ApiRequestShape::LegacyCommand,
            {"command"}, true, legacyRunEnabled)
    };
}

std::vector<std::string> apiFeatureList()
{
    return {
        "mosaic-jobs",
        "build-jobs",
        "job-management",
        "inspect",
        "database-stats",
        "database-health",
        "database-usage",
        "database-maintenance"
    };
}

} // namespace mosaicraft
