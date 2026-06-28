#include "ApiJson.h"

#include "Version.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace mosaicraft
{

namespace
{

int64_t toUnixSeconds(const std::chrono::system_clock::time_point& time)
{
    if (time == std::chrono::system_clock::time_point{}) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(
        time.time_since_epoch()).count();
}

} // namespace

nlohmann::json serviceResultToJson(const ServiceResult& result)
{
    return {
        {"ok", result.ok},
        {"exitCode", result.exitCode},
        {"message", result.message}
    };
}

nlohmann::json jobSnapshotToJson(const JobSnapshot& job)
{
    return {
        {"id", job.id},
        {"type", job.type},
        {"state", jobStateName(job.state)},
        {"ok", job.result.ok},
        {"exitCode", job.result.exitCode},
        {"message", job.result.message},
        {"inputPath", job.inputPath},
        {"outputPath", job.outputPath},
        {"createdAt", toUnixSeconds(job.createdAt)},
        {"startedAt", toUnixSeconds(job.startedAt)},
        {"finishedAt", toUnixSeconds(job.finishedAt)}
    };
}

nlohmann::json databaseStatsToJson(const DatabaseStats& stats)
{
    nlohmann::json histogram = nlohmann::json::array();
    for (const auto& bin : stats.lHistogram) {
        histogram.push_back({{"lo", bin.lo}, {"hi", bin.hi}, {"count", bin.count}});
    }
    return {
        {"empty", stats.empty},
        {"total", stats.total},
        {"featureWidth", stats.featureWidth},
        {"featureHeight", stats.featureHeight},
        {"gridDim", stats.gridDim},
        {"lab", {
            {"minL", stats.minL}, {"maxL", stats.maxL}, {"avgL", stats.avgL},
            {"minA", stats.minA}, {"maxA", stats.maxA}, {"avgA", stats.avgA},
            {"minB", stats.minB}, {"maxB", stats.maxB}, {"avgB", stats.avgB}
        }},
        {"brightness", {{"dark", stats.dark}, {"mid", stats.mid}, {"bright", stats.bright}}},
        {"lHistogram", histogram},
        {"coverageGaps", stats.coverageGaps}
    };
}

nlohmann::json databaseHealthToJson(const DatabaseHealth& health)
{
    return {
        {"empty", health.empty},
        {"total", health.total},
        {"brightness", {
            {"dark", health.dark}, {"mid", health.mid}, {"bright", health.bright},
            {"darkPct", health.darkPct}, {"midPct", health.midPct}, {"brightPct", health.brightPct}
        }},
        {"colorGamut", {
            {"minA", health.minA}, {"maxA", health.maxA},
            {"minB", health.minB}, {"maxB", health.maxB}
        }},
        {"usage", {
            {"usedCount", health.usedCount}, {"unusedCount", health.unusedCount},
            {"usedPct", health.usedPct}, {"unusedPct", health.unusedPct}
        }},
        {"hotspot", {
            {"topHotspotCount", health.topHotspotCount},
            {"topHotspotTilePct", health.topHotspotTilePct}
        }},
        {"warnings", health.warnings},
        {"recommendations", health.recommendations}
    };
}

nlohmann::json databaseUsageToJson(const DatabaseUsage& usage)
{
    nlohmann::json top = nlohmann::json::array();
    for (const auto& item : usage.top) {
        top.push_back({{"id", item.id}, {"runs", item.runs}, {"tiles", item.tiles}});
    }
    nlohmann::json unused = nlohmann::json::array();
    for (const auto& item : usage.unusedPreview) {
        unused.push_back({{"id", item.id}, {"filePath", item.filePath}});
    }
    return {
        {"empty", usage.empty},
        {"total", usage.total},
        {"top", top},
        {"unusedCount", usage.unusedCount},
        {"unusedPreview", unused}
    };
}

nlohmann::json databaseUsageExportToJson(const DatabaseUsageExport& info)
{
    nlohmann::json exported = nlohmann::json::array();
    for (const auto& item : info.exportedPreview) {
        exported.push_back({
            {"id", item.id},
            {"runs", item.runs},
            {"tiles", item.tiles},
            {"sourcePath", item.sourcePath},
            {"outputPath", item.outputPath}
        });
    }
    return {
        {"outputDir", info.outputDir},
        {"usedCount", info.usedCount},
        {"exportedCount", info.exportedCount},
        {"skippedCount", info.skippedCount},
        {"failedCount", info.failedCount},
        {"exportedPreview", exported},
        {"errors", info.errors}
    };
}

nlohmann::json databasePurgeToJson(const DatabasePurge& purge)
{
    nlohmann::json orphans = nlohmann::json::array();
    for (const auto& item : purge.orphanPreview) {
        orphans.push_back({
            {"id", item.id},
            {"filePath", item.filePath},
            {"tinyPath", item.tinyPath},
            {"histPath", item.histPath}
        });
    }
    return {
        {"dryRun", purge.dryRun},
        {"total", purge.total},
        {"orphanCount", purge.orphanCount},
        {"removedCount", purge.removedCount},
        {"failedCount", purge.failedCount},
        {"orphanPreview", orphans},
        {"errors", purge.errors},
        {"recommendations", purge.recommendations}
    };
}

nlohmann::json inspectResultToJson(const InspectResult& info)
{
    return {
        {"imagePath", info.imagePath},
        {"width", info.width},
        {"height", info.height},
        {"avgL", info.avgL},
        {"avgA", info.avgA},
        {"avgB", info.avgB},
        {"edgeDensity", info.edgeDensity},
        {"lbpEntropy", info.lbpEntropy},
        {"databaseAvailable", info.databaseAvailable},
        {"databaseTotal", info.databaseTotal},
        {"candidateMinL", info.candidateMinL},
        {"candidateMaxL", info.candidateMaxL},
        {"candidateCount", info.candidateCount},
        {"libraryMinL", info.libraryMinL},
        {"libraryMaxL", info.libraryMaxL},
        {"libraryAvgL", info.libraryAvgL},
        {"libraryDark", info.libraryDark},
        {"libraryMid", info.libraryMid},
        {"libraryBright", info.libraryBright}
    };
}

nlohmann::json apiEndpointToJson(const ApiEndpointMetadata& endpoint)
{
    nlohmann::json fields = nlohmann::json::array();
    for (const auto& field : endpoint.requestFields) fields.push_back(field);
    return {
        {"operation", apiOperationName(endpoint.operation)},
        {"requestShape", apiRequestShapeName(endpoint.requestShape)},
        {"method", endpoint.method},
        {"methods", endpoint.methods},
        {"path", endpoint.path},
        {"description", endpoint.description},
        {"category", endpoint.category},
        {"requestFields", fields},
        {"legacy", endpoint.legacy},
        {"enabled", endpoint.enabled}
    };
}

nlohmann::json apiEndpointsToJson(bool legacyRunEnabled)
{
    nlohmann::json endpoints = nlohmann::json::array();
    for (const auto& endpoint : apiEndpointMetadata(legacyRunEnabled)) {
        endpoints.push_back(apiEndpointToJson(endpoint));
    }
    return endpoints;
}

nlohmann::json apiInfoToJson(bool legacyRunEnabled, const char* entryName)
{
    return {
        {"name", "Mosaicraft"},
        {"version", kVersion},
        {"entry", entryName},
        {"api", {
            {"structured", true},
            {"legacyRunEnabled", legacyRunEnabled}
        }},
        {"features", apiFeatureList()}
    };
}

nlohmann::json apiOkJson()
{
    return {{"ok", true}};
}

nlohmann::json apiOkJson(const std::string& key, nlohmann::json value)
{
    nlohmann::json body = apiOkJson();
    body[key] = std::move(value);
    return body;
}

nlohmann::json apiErrorJson(const std::string& message)
{
    return {{"ok", false}, {"message", message}};
}

nlohmann::json apiPayloadJson(const ServiceResult& status, const std::string& key, nlohmann::json value)
{
    nlohmann::json body = {
        {"ok", status.ok},
        {"message", status.message},
        {key, std::move(value)}
    };
    if (!status.ok) body["exitCode"] = status.exitCode;
    return body;
}

nlohmann::json apiJobJson(const JobSnapshot& job)
{
    return apiOkJson("job", jobSnapshotToJson(job));
}

nlohmann::json apiJobErrorJson(const std::string& message, const JobSnapshot& job)
{
    nlohmann::json body = apiErrorJson(message);
    body["job"] = jobSnapshotToJson(job);
    return body;
}

nlohmann::json apiJobsJson(const std::vector<JobSnapshot>& jobs)
{
    nlohmann::json items = nlohmann::json::array();
    for (const auto& job : jobs) {
        items.push_back(jobSnapshotToJson(job));
    }
    return apiOkJson("jobs", items);
}

nlohmann::json apiRemovedJobsJson(int removed)
{
    return apiOkJson("removed", removed);
}

nlohmann::json apiEndpointsResponseJson(bool legacyRunEnabled)
{
    return apiOkJson("endpoints", apiEndpointsToJson(legacyRunEnabled));
}

nlohmann::json apiInfoResponseJson(bool legacyRunEnabled, const char* entryName)
{
    return apiOkJson("info", apiInfoToJson(legacyRunEnabled, entryName));
}

} // namespace mosaicraft
