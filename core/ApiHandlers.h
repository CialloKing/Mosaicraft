#pragma once

#include "ApiMetadata.h"
#include "ApiRequestParser.h"
#include "JobManager.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace mosaicraft
{

struct ApiRequest
{
    ApiOperation operation = ApiOperation::Ping;
    ApiQueryParams query;
    std::string body;
    std::string id;
    bool legacyRunEnabled = false;
    const char* entryName = "Mosaicraft";
};

struct ApiRequestContext
{
    ApiQueryParams query;
    std::string body;
    std::string id;
    bool legacyRunEnabled = false;
    const char* entryName = "Mosaicraft";
};

struct ApiResponse
{
    int status = 200;
    nlohmann::json body;
};

ApiRequest apiOperationRequest(ApiOperation operation, ApiRequestContext context = {});
ApiRequest apiEndpointRequest(const ApiEndpointMetadata& endpoint, ApiRequestContext context = {});
ApiRequest apiRequest(ApiOperation operation);
ApiRequest apiBodyRequest(ApiOperation operation, std::string body);
ApiRequest apiQueryRequest(ApiOperation operation, ApiQueryParams query, std::string body = {});
ApiRequest apiJobRequest(ApiOperation operation, std::string id);
ApiRequest apiInfoRequest(bool legacyRunEnabled, const char* entryName);
ApiRequest apiLegacyRunDisabledRequest();
std::vector<std::string> apiQueryKeyList(ApiOperation operation);
std::vector<const char*> apiQueryKeys(ApiOperation operation);
std::vector<std::string> apiAcceptedQueryKeyList(ApiOperation operation);

ApiResponse handleApiRequest(const ApiRequest& request, JobManager& jobs);
std::vector<std::string> validateApiResponseContract(const ApiEndpointMetadata& endpoint,
                                                      const ApiResponse& response);

ApiResponse apiEndpoints(bool legacyRunEnabled);
ApiResponse apiInfo(bool legacyRunEnabled, const char* entryName);
ApiResponse apiPing();
ApiResponse apiLegacyRunDisabled();

ApiResponse apiMosaic(const std::string& body, JobManager& jobs);
ApiResponse apiSubmitMosaicJob(const std::string& body, JobManager& jobs);
ApiResponse apiSubmitBuildJob(const std::string& body, JobManager& jobs);
ApiResponse apiListJobs(const JobManager& jobs);
ApiResponse apiClearFinishedJobs(JobManager& jobs);
ApiResponse apiGetJob(const std::string& id, const JobManager& jobs);
ApiResponse apiCancelJob(const std::string& id, JobManager& jobs);

ApiResponse apiDatabaseStats(const ApiQueryParams& query, const std::string& body);
ApiResponse apiDatabaseHealth(const ApiQueryParams& query, const std::string& body);
ApiResponse apiDatabaseUsage(const ApiQueryParams& query, const std::string& body);
ApiResponse apiDatabaseUsageExport(const ApiQueryParams& query, const std::string& body);
ApiResponse apiDatabasePurge(const ApiQueryParams& query, const std::string& body);
ApiResponse apiInspect(const ApiQueryParams& query, const std::string& body);

} // namespace mosaicraft
