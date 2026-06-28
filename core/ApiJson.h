#pragma once

#include "ApiMetadata.h"
#include "DatabaseService.h"
#include "InspectService.h"
#include "JobManager.h"
#include "json.hpp"

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

} // namespace mosaicraft
