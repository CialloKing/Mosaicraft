#include "ApiMetadata.h"

#include <algorithm>
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
    if (path == "/api/jobs/{id}") {
        return R"(/api/jobs/([A-Za-z0-9_-]+))";
    }
    return path;
}

std::vector<std::string> endpointQueryKeys(ApiOperation operation)
{
    switch (operation)
    {
    case ApiOperation::DatabaseStats:
    case ApiOperation::DatabaseHealth:
        return {"db"};
    case ApiOperation::DatabaseUsage:
        return {"db", "limit", "unused"};
    case ApiOperation::DatabaseUsageExport:
        return {"db", "output", "confirm"};
    case ApiOperation::DatabasePurge:
        return {"db", "dryRun", "confirm"};
    case ApiOperation::Inspect:
        return {"input", "db"};
    case ApiOperation::Endpoints:
    case ApiOperation::Info:
    case ApiOperation::Ping:
    case ApiOperation::LegacyRunDisabled:
    case ApiOperation::Mosaic:
    case ApiOperation::SubmitMosaicJob:
    case ApiOperation::SubmitBuildJob:
    case ApiOperation::ListJobs:
    case ApiOperation::ClearFinishedJobs:
    case ApiOperation::GetJob:
    case ApiOperation::CancelJob:
        return {};
    }
    return {};
}

std::unordered_map<std::string, std::vector<std::string>> endpointFieldAliases(ApiOperation operation)
{
    switch (operation)
    {
    case ApiOperation::Mosaic:
    case ApiOperation::SubmitMosaicJob:
        return {
            {"inputPath", {"input"}},
            {"dbPath", {"db"}},
            {"outputPath", {"output"}},
            {"format", {"outputFormat"}},
            {"quality", {"jpegQuality"}},
            {"pngLevel", {"pngCompressionLevel"}},
            {"topNrandom", {"topNRandom"}},
            {"usePenalty", {"penalty"}},
            {"tiled", {"tiledOutput"}},
            {"deepZoom", {"deepzoom"}}
        };
    case ApiOperation::SubmitBuildJob:
        return {
            {"inputDir", {"input"}},
            {"outputDir", {"output"}},
            {"dbPath", {"db"}},
            {"appendMode", {"append"}},
            {"forceMode", {"force"}}
        };
    case ApiOperation::DatabaseStats:
    case ApiOperation::DatabaseHealth:
        return {{"dbPath", {"db"}}};
    case ApiOperation::DatabaseUsage:
        return {
            {"dbPath", {"db"}},
            {"showUnused", {"unused"}}
        };
    case ApiOperation::DatabaseUsageExport:
        return {
            {"dbPath", {"db"}},
            {"outputDir", {"output"}}
        };
    case ApiOperation::DatabasePurge:
        return {{"dbPath", {"db"}}};
    case ApiOperation::Inspect:
        return {
            {"imagePath", {"input"}},
            {"dbPath", {"db"}}
        };
    case ApiOperation::Endpoints:
    case ApiOperation::Info:
    case ApiOperation::Ping:
    case ApiOperation::LegacyRunDisabled:
    case ApiOperation::ListJobs:
    case ApiOperation::ClearFinishedJobs:
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
                                                   const std::unordered_map<std::string, std::vector<std::string>>& fieldAliases,
                                                   ApiRequestShape requestShape)
{
    std::vector<std::string> keys;
    for (const auto& key : queryKeys) addUnique(keys, key);
    if (requestShape == ApiRequestShape::Query) {
        for (const auto& field : requestFields) addUnique(keys, field);
        for (const auto& item : fieldAliases) {
            for (const auto& alias : item.second) addUnique(keys, alias);
        }
    }
    return keys;
}

int endpointSuccessStatus(ApiOperation operation)
{
    switch (operation)
    {
    case ApiOperation::SubmitMosaicJob:
    case ApiOperation::SubmitBuildJob:
        return 202;
    case ApiOperation::Endpoints:
    case ApiOperation::Info:
    case ApiOperation::Ping:
    case ApiOperation::LegacyRunDisabled:
    case ApiOperation::Mosaic:
    case ApiOperation::ListJobs:
    case ApiOperation::ClearFinishedJobs:
    case ApiOperation::GetJob:
    case ApiOperation::CancelJob:
    case ApiOperation::DatabaseStats:
    case ApiOperation::DatabaseHealth:
    case ApiOperation::DatabaseUsage:
    case ApiOperation::DatabaseUsageExport:
    case ApiOperation::DatabasePurge:
    case ApiOperation::Inspect:
        return 200;
    }
    return 200;
}

std::string endpointResponseKey(ApiOperation operation)
{
    switch (operation)
    {
    case ApiOperation::Endpoints: return "endpoints";
    case ApiOperation::Info: return "info";
    case ApiOperation::Ping: return "message";
    case ApiOperation::SubmitMosaicJob:
    case ApiOperation::SubmitBuildJob:
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
    case ApiOperation::Inspect: return "inspect";
    case ApiOperation::LegacyRunDisabled:
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
    case ApiOperation::LegacyRunDisabled:
        addErrorContract(info, 404, "apiError");
        break;
    case ApiOperation::Mosaic:
    case ApiOperation::SubmitMosaicJob:
    case ApiOperation::SubmitBuildJob:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 500, "serviceResult");
        break;
    case ApiOperation::GetJob:
        addErrorContract(info, 404, "apiError");
        break;
    case ApiOperation::CancelJob:
        addErrorContract(info, 404, "apiError");
        addErrorContract(info, 409, "jobError", "job");
        break;
    case ApiOperation::DatabaseStats:
    case ApiOperation::DatabaseHealth:
    case ApiOperation::DatabaseUsage:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 500, "serviceResult");
        break;
    case ApiOperation::DatabaseUsageExport:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 400, "payloadResult", "export");
        addErrorContract(info, 500, "payloadResult", "export");
        break;
    case ApiOperation::DatabasePurge:
        addErrorContract(info, 400, "serviceResult");
        addErrorContract(info, 400, "payloadResult", "purge");
        addErrorContract(info, 500, "payloadResult", "purge");
        break;
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
    info.successStatus = endpointSuccessStatus(operation);
    info.responseKey = endpointResponseKey(operation);
    info.sideEffects = sideEffects;
    info.longRunning = longRunning;
    info.queryKeys = endpointQueryKeys(operation);
    info.fieldAliases = endpointFieldAliases(operation);
    for (const char* field : requestFields) {
        info.requestFields.emplace_back(field);
    }
    for (const char* field : requiredFields) {
        info.requiredFields.emplace_back(field);
    }
    info.acceptedQueryKeys = endpointAcceptedQueryKeys(
        info.queryKeys, info.requestFields, info.fieldAliases, info.requestShape);
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

const char* apiContractVersion()
{
    return "1.0";
}

int apiContractMajorVersion()
{
    return 1;
}

const char* apiCompatibilityLevel()
{
    return "stable";
}

bool apiContractStable()
{
    return true;
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
            {"inputPath", "dbPath", "outputPath", "tileW", "tileH", "outW", "outH",
             "nativeTileW", "nativeTileH", "candidates", "topNrandom", "neighborWindow",
             "upscale", "quality", "pngLevel", "lRange", "usePenalty", "labWeight",
             "gridWeight", "tinyWeight", "edgeWeight", "lbpWeight", "neighborPenalty",
             "colorStrength", "format", "writeMode", "outputTile", "useGpu", "tiled",
             "deepZoom", "colorAdjust", "adaptiveWeights", "analyze", "benchmark", "cpu"},
            {"inputPath"}, true, true),
        endpoint("POST", "/api/jobs/mosaic", "start mosaic job", "jobs",
            ApiOperation::SubmitMosaicJob, ApiRequestShape::Body,
            {"inputPath", "dbPath", "outputPath", "tileW", "tileH", "outW", "outH",
             "nativeTileW", "nativeTileH", "candidates", "topNrandom", "neighborWindow",
             "upscale", "quality", "pngLevel", "lRange", "usePenalty", "labWeight",
             "gridWeight", "tinyWeight", "edgeWeight", "lbpWeight", "neighborPenalty",
             "colorStrength", "format", "writeMode", "outputTile", "useGpu", "tiled",
             "deepZoom", "colorAdjust", "adaptiveWeights", "analyze", "benchmark", "cpu"},
            {"inputPath"}, true, true),
        endpoint("POST", "/api/jobs/build", "start library build job", "jobs",
            ApiOperation::SubmitBuildJob, ApiRequestShape::Body,
            {"inputDir", "outputDir", "dbPath", "threads", "normalizeSize",
             "normalizeWidth", "normalizeHeight", "appendMode", "recursive",
             "normalizeOnly", "forceMode"},
            {"inputDir"}, true, true),
        endpoint("GET", "/api/jobs", "list jobs", "jobs",
            ApiOperation::ListJobs, ApiRequestShape::None),
        endpoint("DELETE", "/api/jobs", "clear finished jobs", "jobs",
            ApiOperation::ClearFinishedJobs, ApiRequestShape::None,
            {}, {}, true),
        endpoint("GET", "/api/jobs/{id}", "get job status", "jobs",
            ApiOperation::GetJob, ApiRequestShape::JobId, {"id"}, {"id"}),
        endpoint("DELETE", "/api/jobs/{id}", "cancel queued job", "jobs",
            ApiOperation::CancelJob, ApiRequestShape::JobId, {"id"}, {"id"}, true),
        endpoint("GET|POST", "/api/db/stats", "database statistics", "database",
            ApiOperation::DatabaseStats, ApiRequestShape::Query, {"dbPath"}),
        endpoint("GET|POST", "/api/db/health", "database health report", "database",
            ApiOperation::DatabaseHealth, ApiRequestShape::Query, {"dbPath"}),
        endpoint("GET|POST", "/api/db/usage", "database usage report", "database",
            ApiOperation::DatabaseUsage, ApiRequestShape::Query,
            {"dbPath", "limit", "showUnused"}),
        endpoint("POST", "/api/db/usage/export", "export used images", "database",
            ApiOperation::DatabaseUsageExport, ApiRequestShape::Query,
            {"dbPath", "outputDir", "confirm"}, {"outputDir", "confirm"}, true),
        endpoint("GET|POST", "/api/db/purge", "preview or purge orphan records", "database",
            ApiOperation::DatabasePurge, ApiRequestShape::Query,
            {"dbPath", "dryRun", "confirm"}, {}, true),
        endpoint("GET|POST", "/api/inspect", "inspect a source image", "inspect",
            ApiOperation::Inspect, ApiRequestShape::Query,
            {"imagePath", "dbPath"}, {"imagePath"}),
        endpoint("POST", "/api/run", "legacy command compatibility endpoint", "legacy",
            ApiOperation::LegacyRunDisabled, ApiRequestShape::LegacyCommand,
            {"command"}, {"command"}, true, true, true, legacyRunEnabled)
    };
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
            if (shape != "serviceResult" && shape != "apiError" &&
                shape != "jobError" && shape != "payloadResult") {
                errors.push_back("endpoint has unsupported errorShape: " + operationName + " " + shape);
            }
        }
        if (endpoint.requestShape == ApiRequestShape::Query && endpoint.queryKeys.empty()) {
            errors.push_back("query endpoint has no queryKeys: " + operationName);
        }
        if (endpoint.requestShape == ApiRequestShape::Query && endpoint.acceptedQueryKeys.empty()) {
            errors.push_back("query endpoint has no acceptedQueryKeys: " + operationName);
        }
        if (endpoint.requestShape != ApiRequestShape::Query && !endpoint.queryKeys.empty()) {
            errors.push_back("non-query endpoint has queryKeys: " + operationName);
        }
        if (endpoint.requestShape != ApiRequestShape::Query && !endpoint.acceptedQueryKeys.empty()) {
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
        for (const auto& item : endpoint.fieldAliases) {
            if (std::find(endpoint.requestFields.begin(), endpoint.requestFields.end(), item.first) ==
                endpoint.requestFields.end()) {
                errors.push_back("field alias target is not listed in requestFields: " + operationName + " " + item.first);
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
