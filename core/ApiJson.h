#pragma once

#include "ApiMetadata.h"
#include "DatabaseService.h"
#include "InspectService.h"
#include "JobManager.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace mosaicraft
{

nlohmann::json serviceResultToJson(const ServiceResult& result);
nlohmann::json jobSnapshotToJson(const JobSnapshot& job);
nlohmann::json databaseStatsToJson(const DatabaseStats& stats);
nlohmann::json databaseHealthToJson(const DatabaseHealth& health);
nlohmann::json databaseUsageToJson(const DatabaseUsage& usage);
nlohmann::json databaseUsageExportToJson(const DatabaseUsageExport& info);
nlohmann::json databasePurgeToJson(const DatabasePurge& purge);
nlohmann::json inspectResultToJson(const InspectResult& info);
nlohmann::json apiEndpointToJson(const ApiEndpointMetadata& endpoint);
nlohmann::json apiEndpointsToJson(bool legacyRunEnabled);
nlohmann::json apiInfoToJson(bool legacyRunEnabled, const char* entryName);
nlohmann::json apiOkJson();
nlohmann::json apiOkJson(const std::string& key, nlohmann::json value);
nlohmann::json apiErrorJson(const std::string& message);
nlohmann::json apiPayloadJson(const ServiceResult& status, const std::string& key, nlohmann::json value);
nlohmann::json apiJobJson(const JobSnapshot& job);
nlohmann::json apiJobErrorJson(const std::string& message, const JobSnapshot& job);
nlohmann::json apiJobsJson(const std::vector<JobSnapshot>& jobs);
nlohmann::json apiRemovedJobsJson(int removed);
nlohmann::json apiEndpointsResponseJson(bool legacyRunEnabled);
nlohmann::json apiInfoResponseJson(bool legacyRunEnabled, const char* entryName);

} // namespace mosaicraft
