#include "ApiHandlers.h"

#include "ApiJson.h"

#include <exception>
#include <utility>

namespace mosaicraft
{

namespace
{

ApiResponse resultResponse(int status, const ServiceResult& result)
{
    return {status, serviceResultToJson(result)};
}

ApiResponse badRequest(const std::string& error)
{
    return resultResponse(400, ServiceResult::failure(1, error));
}

ApiResponse internalError(const std::string& error)
{
    return resultResponse(500, ServiceResult::failure(1, error));
}

int databaseMaintenanceErrorStatus(const ServiceResult& status)
{
    return status.exitCode == 2 ? 500 : 400;
}

} // namespace

ApiRequest apiRequest(ApiOperation operation)
{
    ApiRequest request;
    request.operation = operation;
    return request;
}

ApiRequest apiBodyRequest(ApiOperation operation, std::string body)
{
    ApiRequest request = apiRequest(operation);
    request.body = std::move(body);
    return request;
}

ApiRequest apiQueryRequest(ApiOperation operation, ApiQueryParams query, std::string body)
{
    ApiRequest request = apiRequest(operation);
    request.query = std::move(query);
    request.body = std::move(body);
    return request;
}

ApiRequest apiJobRequest(ApiOperation operation, std::string id)
{
    ApiRequest request = apiRequest(operation);
    request.id = std::move(id);
    return request;
}

ApiRequest apiInfoRequest(bool legacyRunEnabled, const char* entryName)
{
    ApiRequest request = apiRequest(ApiOperation::Info);
    request.legacyRunEnabled = legacyRunEnabled;
    request.entryName = entryName;
    return request;
}

ApiRequest apiLegacyRunDisabledRequest()
{
    return apiRequest(ApiOperation::LegacyRunDisabled);
}

std::vector<const char*> apiQueryKeys(ApiOperation operation)
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

ApiResponse handleApiRequest(const ApiRequest& request, JobManager& jobs)
{
    switch (request.operation)
    {
    case ApiOperation::Endpoints:
        return apiEndpoints(request.legacyRunEnabled);
    case ApiOperation::Info:
        return apiInfo(request.legacyRunEnabled, request.entryName);
    case ApiOperation::Ping:
        return apiPing();
    case ApiOperation::LegacyRunDisabled:
        return apiLegacyRunDisabled();
    case ApiOperation::Mosaic:
        return apiMosaic(request.body, jobs);
    case ApiOperation::SubmitMosaicJob:
        return apiSubmitMosaicJob(request.body, jobs);
    case ApiOperation::SubmitBuildJob:
        return apiSubmitBuildJob(request.body, jobs);
    case ApiOperation::ListJobs:
        return apiListJobs(jobs);
    case ApiOperation::ClearFinishedJobs:
        return apiClearFinishedJobs(jobs);
    case ApiOperation::GetJob:
        return apiGetJob(request.id, jobs);
    case ApiOperation::CancelJob:
        return apiCancelJob(request.id, jobs);
    case ApiOperation::DatabaseStats:
        return apiDatabaseStats(request.query, request.body);
    case ApiOperation::DatabaseHealth:
        return apiDatabaseHealth(request.query, request.body);
    case ApiOperation::DatabaseUsage:
        return apiDatabaseUsage(request.query, request.body);
    case ApiOperation::DatabaseUsageExport:
        return apiDatabaseUsageExport(request.query, request.body);
    case ApiOperation::DatabasePurge:
        return apiDatabasePurge(request.query, request.body);
    case ApiOperation::Inspect:
        return apiInspect(request.query, request.body);
    }
    return internalError("unknown API operation");
}

ApiResponse apiEndpoints(bool legacyRunEnabled)
{
    return {200, apiEndpointsResponseJson(legacyRunEnabled)};
}

ApiResponse apiInfo(bool legacyRunEnabled, const char* entryName)
{
    return {200, apiInfoResponseJson(legacyRunEnabled, entryName)};
}

ApiResponse apiPing()
{
    return {200, apiOkJson("message", "pong")};
}

ApiResponse apiLegacyRunDisabled()
{
    return {
        404,
        apiErrorJson("legacy /api/run is disabled; set MOSAICRAFT_ENABLE_LEGACY_RUN=1 to enable compatibility mode")
    };
}

ApiResponse apiMosaic(const std::string& body, JobManager& jobs)
{
    MosaicRequest request;
    std::string error;
    if (!parseMosaicRequestJson(body, request, error)) return badRequest(error);

    try {
        const std::string jobId = jobs.submitMosaic(request);
        JobSnapshot snapshot;
        if (!jobs.waitJob(jobId, snapshot)) {
            return internalError("job not found");
        }
        return resultResponse(snapshot.result.ok ? 200 : 500, snapshot.result);
    } catch (const std::exception& e) {
        return internalError(e.what());
    } catch (...) {
        return internalError("internal error");
    }
}

ApiResponse apiSubmitMosaicJob(const std::string& body, JobManager& jobs)
{
    MosaicRequest request;
    std::string error;
    if (!parseMosaicRequestJson(body, request, error)) return badRequest(error);

    try {
        const std::string jobId = jobs.submitMosaic(request);
        JobSnapshot snapshot;
        jobs.getJob(jobId, snapshot);
        return {202, apiJobJson(snapshot)};
    } catch (const std::exception& e) {
        return internalError(e.what());
    } catch (...) {
        return internalError("internal error");
    }
}

ApiResponse apiSubmitBuildJob(const std::string& body, JobManager& jobs)
{
    BuildRequest request;
    std::string error;
    if (!parseBuildRequestJson(body, request, error)) return badRequest(error);

    try {
        const std::string jobId = jobs.submitBuild(request);
        JobSnapshot snapshot;
        jobs.getJob(jobId, snapshot);
        return {202, apiJobJson(snapshot)};
    } catch (const std::exception& e) {
        return internalError(e.what());
    } catch (...) {
        return internalError("internal error");
    }
}

ApiResponse apiListJobs(const JobManager& jobs)
{
    return {200, apiJobsJson(jobs.listJobs())};
}

ApiResponse apiClearFinishedJobs(JobManager& jobs)
{
    return {200, apiRemovedJobsJson(jobs.clearFinishedJobs())};
}

ApiResponse apiGetJob(const std::string& id, const JobManager& jobs)
{
    JobSnapshot snapshot;
    if (!jobs.getJob(id, snapshot)) {
        return {404, apiErrorJson("job not found")};
    }
    return {200, apiJobJson(snapshot)};
}

ApiResponse apiCancelJob(const std::string& id, JobManager& jobs)
{
    JobSnapshot snapshot;
    if (jobs.cancelQueuedJob(id, snapshot)) {
        return {200, apiJobJson(snapshot)};
    }
    if (snapshot.id.empty()) {
        return {404, apiErrorJson("job not found")};
    }
    return {409, apiJobErrorJson("only queued jobs can be canceled", snapshot)};
}

ApiResponse apiDatabaseStats(const ApiQueryParams& query, const std::string& body)
{
    DatabaseRequest request;
    std::string error;
    if (!parseDatabaseRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.stats(request);
    if (!result.status.ok) return resultResponse(500, result.status);
    return {200, apiPayloadJson(result.status, "stats", databaseStatsToJson(result.stats))};
}

ApiResponse apiDatabaseHealth(const ApiQueryParams& query, const std::string& body)
{
    DatabaseRequest request;
    std::string error;
    if (!parseDatabaseRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.health(request);
    if (!result.status.ok) return resultResponse(500, result.status);
    return {200, apiPayloadJson(result.status, "health", databaseHealthToJson(result.health))};
}

ApiResponse apiDatabaseUsage(const ApiQueryParams& query, const std::string& body)
{
    DatabaseUsageRequest request;
    std::string error;
    if (!parseDatabaseUsageRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.usage(request);
    if (!result.status.ok) return resultResponse(500, result.status);
    return {200, apiPayloadJson(result.status, "usage", databaseUsageToJson(result.usage))};
}

ApiResponse apiDatabaseUsageExport(const ApiQueryParams& query, const std::string& body)
{
    DatabaseUsageExportRequest request;
    std::string error;
    if (!parseDatabaseUsageExportRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.exportUsage(request);
    return {
        result.status.ok ? 200 : databaseMaintenanceErrorStatus(result.status),
        apiPayloadJson(result.status, "export", databaseUsageExportToJson(result.exportInfo))
    };
}

ApiResponse apiDatabasePurge(const ApiQueryParams& query, const std::string& body)
{
    DatabasePurgeRequest request;
    std::string error;
    if (!parseDatabasePurgeRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.purge(request);
    return {
        result.status.ok ? 200 : databaseMaintenanceErrorStatus(result.status),
        apiPayloadJson(result.status, "purge", databasePurgeToJson(result.purge))
    };
}

ApiResponse apiInspect(const ApiQueryParams& query, const std::string& body)
{
    InspectRequest request;
    std::string error;
    if (!parseInspectRequestApi(query, body, request, error)) return badRequest(error);

    InspectService service;
    const auto result = service.inspect(request);
    if (!result.status.ok) return resultResponse(400, result.status);
    return {200, apiPayloadJson(result.status, "inspect", inspectResultToJson(result))};
}

} // namespace mosaicraft
