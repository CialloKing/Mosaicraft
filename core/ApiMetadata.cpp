#include "ApiMetadata.h"

#include <initializer_list>

namespace mosaicraft
{

namespace
{

ApiEndpointMetadata endpoint(const std::string& method,
                             const std::string& path,
                             const std::string& description,
                             const std::string& category,
                             std::initializer_list<const char*> requestFields = {},
                             bool legacy = false,
                             bool enabled = true)
{
    ApiEndpointMetadata info;
    info.method = method;
    info.path = path;
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

std::vector<ApiEndpointMetadata> apiEndpointMetadata(bool legacyRunEnabled)
{
    return {
        endpoint("GET", "/api/info", "service version and capability summary", "discovery"),
        endpoint("GET", "/api/ping", "health check", "health"),
        endpoint("POST", "/api/mosaic", "run mosaic synchronously", "mosaic",
            {"inputPath", "dbPath", "outputPath", "format", "quality", "writeMode"}),
        endpoint("POST", "/api/jobs/mosaic", "start mosaic job", "jobs",
            {"inputPath", "dbPath", "outputPath", "format", "quality", "writeMode"}),
        endpoint("POST", "/api/jobs/build", "start library build job", "jobs",
            {"inputDir", "outputDir", "dbPath", "threads", "recursive", "forceMode"}),
        endpoint("GET", "/api/jobs", "list jobs", "jobs"),
        endpoint("DELETE", "/api/jobs", "clear finished jobs", "jobs"),
        endpoint("GET", "/api/jobs/{id}", "get job status", "jobs", {"id"}),
        endpoint("DELETE", "/api/jobs/{id}", "cancel queued job", "jobs", {"id"}),
        endpoint("GET|POST", "/api/db/stats", "database statistics", "database", {"dbPath"}),
        endpoint("GET|POST", "/api/db/health", "database health report", "database", {"dbPath"}),
        endpoint("GET|POST", "/api/db/usage", "database usage report", "database",
            {"dbPath", "limit", "showUnused"}),
        endpoint("POST", "/api/db/usage/export", "export used images", "database",
            {"dbPath", "outputDir", "confirm"}),
        endpoint("GET|POST", "/api/db/purge", "preview or purge orphan records", "database",
            {"dbPath", "dryRun", "confirm"}),
        endpoint("GET|POST", "/api/inspect", "inspect a source image", "inspect",
            {"imagePath", "dbPath"}),
        endpoint("POST", "/api/run", "legacy command compatibility endpoint", "legacy",
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
