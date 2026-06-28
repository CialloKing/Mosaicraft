#pragma once

#include "ApiRequestParser.h"
#include "JobManager.h"
#include "json.hpp"

#include <string>

namespace mosaicraft
{

struct ApiResponse
{
    int status = 200;
    nlohmann::json body;
};

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
