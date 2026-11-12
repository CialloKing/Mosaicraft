#include "ApiJson.h"

#include "Version.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace mosaicraft
{
nlohmann::json colorDistributionToJson(const ColorDistribution &distribution)
{
    return {{"axes", ColorDistribution::axes}, {"ab", distribution.ab}, {"hue", distribution.hue},
            {"lightness", distribution.lightness}, {"gray", distribution.gray}, {"total", distribution.total}};
}

namespace
{

int64_t toUnixSeconds(const std::chrono::system_clock::time_point& time)
{
    if (time == std::chrono::system_clock::time_point{}) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(
        time.time_since_epoch()).count();
}

} // namespace

nlohmann::json diagnosticToJson(const Diagnostic &item)
{
    if (item.code.empty())
    {
        return nullptr;
    }
    return {
        {"sequence", item.sequence}, {"timestamp", item.timestamp}, {"code", item.code},   {"severity", item.severity},
        {"stage", item.stage},       {"message", item.message},     {"field", item.field}, {"path", item.path}};
}

nlohmann::json artifactToJson(const Artifact &item)
{
    return {{"id", item.id},     {"kind", item.kind},   {"path", item.path},
            {"mime", item.mime}, {"bytes", item.bytes}, {"directory", item.directory}};
}

nlohmann::json progressToJson(const ProgressSnapshot &value)
{
    auto activities = nlohmann::json::array();
    for (const auto &item : value.activities)
    {
        activities.push_back({{"id", item.id},
                              {"completed", item.completed},
                              {"total", item.total < 0 ? nlohmann::json(nullptr) : nlohmann::json(item.total)},
                              {"unit", item.unit}});
    }
    return {{"stage", value.stage},
            {"startedAt", value.startedAt},
            {"updatedAt", value.updatedAt},
            {"activities", activities}};
}

nlohmann::json runtimeSnapshotToJson(const RuntimeSnapshot &value)
{
    auto warnings = nlohmann::json::array();
    for (const auto &entry : value.warnings)
    {
        warnings.push_back({{"code", entry.first},
                            {"count", entry.second.count},
                            {"message", entry.second.message},
                            {"paths", entry.second.paths}});
    }
    auto artifacts = nlohmann::json::array();
    for (const auto &item : value.artifacts)
    {
        artifacts.push_back(artifactToJson(item));
    }
    nlohmann::json result = {{"diagnostic", diagnosticToJson(value.diagnostic)},
            {"warningCount", value.warningCount},
            {"warnings", warnings},
            {"artifacts", artifacts},
            {"progress", progressToJson(value.progress)},
            {"runtime",
             {{"featureBackend", value.featureBackend},
              {"scoreBackend", value.scoreBackend},
              {"fallbackReason", value.fallbackReason},
              {"format", value.format},
              {"width", value.width},
              {"height", value.height},
              {"matched", value.matched},
              {"failed", value.failed},
               {"metrics", value.metrics}}}};
    if (value.outputPlan)
    {
        const auto &plan = *value.outputPlan;
        result["runtime"]["outputPlan"] = {
            {"formatSource", plan.formatSource}, {"requestedFormat", plan.requestedFormat},
            {"originalWidth", plan.originalWidth}, {"originalHeight", plan.originalHeight},
            {"originalTileWidth", plan.originalTileWidth}, {"originalTileHeight", plan.originalTileHeight},
            {"format", plan.format}, {"path", plan.path}, {"width", plan.width}, {"height", plan.height},
            {"tileWidth", plan.tileWidth}, {"tileHeight", plan.tileHeight},
            {"scaled", plan.scaled}, {"formatChanged", plan.formatChanged}};
    }
    if (value.buildVisuals)
    {
        const auto &build = *value.buildVisuals;
        auto &report = result["runtime"]["build"];
        report = {{"scanned", build.scanned}, {"normalized", build.normalized},
                  {"duplicates", build.duplicates}, {"inserted", build.inserted}};
        if (build.hasColors)
        {
            report["before"] = colorDistributionToJson(build.before);
            report["after"] = colorDistributionToJson(build.after);
        }
    }
    if (!value.resourceSamples.empty())
    {
        auto samples = nlohmann::json::array();
        nlohmann::json summary = nlohmann::json::object();
        for (const auto &sample : value.resourceSamples)
        {
            nlohmann::json row{{"elapsedMs", sample.elapsedMs}};
            for (size_t i = 0; i < resourceNames.size(); ++i)
            {
                row[resourceNames[i]] = sample.values[i] >= 0 ? nlohmann::json(sample.values[i]) : nlohmann::json(nullptr);
            }
            samples.push_back(std::move(row));
        }
        for (size_t i = 0; i < resourceNames.size(); ++i)
        {
            const auto &stat = value.resources[i];
            if (stat.count)
            {
                summary[resourceNames[i]] = {{"peak", stat.peak}, {"mean", stat.sum / stat.count}};
            }
        }
        result["runtime"]["resources"] = {{"samples", std::move(samples)}, {"summary", std::move(summary)}};
    }
    return result;
}

nlohmann::json eventPageToJson(const EventPage &page)
{
    auto items = nlohmann::json::array();
    for (const auto &item : page.events)
    {
        items.push_back(diagnosticToJson(item));
    }
    return {{"ok", true},
            {"events", items},
            {"firstSequence", page.firstSequence},
            {"nextCursor", page.nextCursor},
            {"dropped", page.dropped},
            {"truncated", page.truncated}};
}

nlohmann::json serviceResultToJson(const ServiceResult &result)
{
    auto body = runtimeSnapshotToJson(result.runtime);
    body["ok"] = result.ok;
    body["exitCode"] = result.exitCode;
    body["message"] = result.message;
    if (!result.ok && result.runtime.diagnostic.code.empty())
    {
        Diagnostic diagnostic;
        diagnostic.code = "operation_failed";
        diagnostic.severity = "error";
        diagnostic.message = result.message;
        body["diagnostic"] = diagnosticToJson(diagnostic);
    }
    return body;
}

nlohmann::json jobSnapshotToJson(const JobSnapshot &job, bool detail)
{
    auto body = serviceResultToJson(job.result);
    if (job.state != JobState::Failed)
    {
        // 排队、运行和取消不是业务失败；默认 ServiceResult 不能制造终止诊断。
        body["diagnostic"] = diagnosticToJson(job.result.runtime.diagnostic);
    }
    body.update({{"id", job.id},
                 {"type", job.type},
                 {"state", jobStateName(job.state)},
                 {"inputPath", job.inputPath},
                 {"outputPath", job.outputPath},
                 {"createdAt", toUnixSeconds(job.createdAt)},
                 {"startedAt", toUnixSeconds(job.startedAt)},
                 {"finishedAt", toUnixSeconds(job.finishedAt)},
                 {"canCancel", job.state == JobState::Queued}});
    if (detail && !job.requestJson.empty())
    {
        body["request"] = nlohmann::json::parse(job.requestJson);
    }
    if (!detail)
    {
        body.erase("runtime");
        body.erase("artifacts");
        body.erase("warnings");
    }
    return body;
}

nlohmann::json mosaicRequestToJson(const MosaicRequest &request)
{
    const auto &cfg = request.config;
    nlohmann::json value = {
        {"inputPath", request.inputPath}, {"dbPath", request.dbPath}, {"outputPath", request.outputPath}};
    value["tileW"] = cfg.tileW;
    value["tileH"] = cfg.tileH;
    value["candidates"] = cfg.candidates;
    value["lRange"] = cfg.lRange;
    value["usePenalty"] = cfg.usePenalty;
    value["outW"] = cfg.outW;
    value["outH"] = cfg.outH;
    value["nativeTileW"] = cfg.nativeTileW;
    value["nativeTileH"] = cfg.nativeTileH;
    value["labWeight"] = cfg.labWeight;
    value["gridWeight"] = cfg.gridWeight;
    value["tinyWeight"] = cfg.tinyWeight;
    value["edgeWeight"] = cfg.edgeWeight;
    value["lbpWeight"] = cfg.lbpWeight;
    value["neighborWindow"] = cfg.neighborWindow;
    value["neighborPenalty"] = cfg.neighborPenalty;
    value["topNRandom"] = cfg.topNRandom;
    value["useGpu"] = cfg.useGpu;
    value["deepZoom"] = cfg.deepZoom;
    value["generatePreview"] = cfg.generatePreview;
    value["colorAdjust"] = cfg.colorAdjust;
    value["colorStrength"] = cfg.colorStrength;
    value["overlayStrength"] = cfg.overlayStrength;
    value["benchmark"] = cfg.benchmark;
    value["resourceMonitor"] = request.resourceMonitor;
    value["adaptiveWeights"] = cfg.adaptiveWeights;
    value["analyze"] = cfg.analyze;
    value["pngLevel"] = cfg.pngCompressionLevel;
    value["writeMode"] = cfg.writeMode;
    value["quality"] = cfg.jpegQuality;
    value["tiled"] = cfg.tiledOutput;
    if (cfg.formatExplicit)
    {
        value["format"] = cfg.outputFormat;
    }
    if (cfg.upscale > 0)
    {
        value["upscale"] = cfg.upscale;
    }
    return value;
}

nlohmann::json buildRequestToJson(const BuildRequest &request)
{
    return {{"inputDir", request.inputDir},
            {"outputDir", request.outputDir},
            {"dbPath", request.dbPath},
            {"threads", request.threads},
            {"appendMode", request.appendMode},
            {"normalizeOnly", request.normalizeOnly},
            {"forceMode", request.forceMode},
            {"recursive", request.recursive},
            {"normalizeWidth", request.normalizeWidth},
            {"normalizeHeight", request.normalizeHeight},
            {"resourceMonitor", request.resourceMonitor}};
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
        {"colors", colorDistributionToJson(stats.colors)},
        {"coverageGaps", stats.coverageGaps}
    };
}

nlohmann::json databaseHealthToJson(const DatabaseHealth& health)
{
    return {
        {"missingFiles", health.missingFiles},
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
            {"lbpPath", item.lbpPath}
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
    nlohmann::json requiredFields = nlohmann::json::array();
    for (const auto& field : endpoint.requiredFields) requiredFields.push_back(field);
    nlohmann::json errorResponses = nlohmann::json::array();
    for (const auto& item : endpoint.errorResponses) {
        errorResponses.push_back({
            {"status", item.status},
            {"shape", item.shape},
            {"responseKey", item.responseKey}
        });
    }
    return {{"operation", apiOperationName(endpoint.operation)},
            {"requestShape", apiRequestShapeName(endpoint.requestShape)},
            {"method", endpoint.method},
            {"methods", endpoint.methods},
            {"path", endpoint.path},
            {"httpPattern", endpoint.httpPattern},
            {"description", endpoint.description},
            {"category", endpoint.category},
            {"requestFields", fields},
            {"requiredFields", requiredFields},
            {"queryKeys", endpoint.queryKeys},
            {"acceptedQueryKeys", endpoint.acceptedQueryKeys},
            {"successStatus", endpoint.successStatus},
            {"responseKey", endpoint.responseKey},
            {"errorStatuses", endpoint.errorStatuses},
            {"errorShapes", endpoint.errorShapes},
            {"errorResponseKeys", endpoint.errorResponseKeys},
            {"responseFormat", endpoint.operation == ApiOperation::GetJobArtifact ? "file" : "json"},
            {"errorResponses", errorResponses},
            {"sideEffects", endpoint.sideEffects},
            {"longRunning", endpoint.longRunning},
            {"enabled", endpoint.enabled}};
}

nlohmann::json apiEndpointsToJson()
{
    nlohmann::json endpoints = nlohmann::json::array();
    for (const auto& endpoint : apiEndpointMetadata()) {
        endpoints.push_back(apiEndpointToJson(endpoint));
    }
    return endpoints;
}

nlohmann::json apiInfoToJson(const char* entryName)
{
    const auto endpoints = apiEndpointMetadata();
    const auto metadataErrors = validateApiEndpointMetadata(endpoints);
    int enabledCount = 0;
    for (const auto& endpoint : endpoints) {
        if (endpoint.enabled) ++enabledCount;
    }
    nlohmann::json entryPoints = nlohmann::json::array();
    for (const auto& entryPoint : apiEntryPointMetadata()) {
        entryPoints.push_back({
            {"name", entryPoint.name},
            {"executable", entryPoint.executable},
            {"role", entryPoint.role}
        });
    }

    return {
        {"name", "Mosaicraft"},
        {"version", kVersion},
        {"entry", entryName},
        {"entryPoints", entryPoints},
        {"versions", {{"library", kLibraryVersion}, {"feature", kFeatureVersion},
                      {"ann", kAnnVersion}, {"featurePack", kFeaturePackVersion},
                      {"colorOrder", kColorOrderVersion}, {"journal", kJournalVersion},
                      {"studySchema", kStudySchemaVersion}, {"checkpointPaths", kCheckpointPathsVersion}}},
        {"api", {
            {"structured", true},
            {"contractVersion", apiContractVersion()},
            {"contractMajorVersion", apiContractMajorVersion()},
            {"compatibility", apiCompatibilityLevel()},
            {"stable", apiContractStable()},
            {"endpointCount", static_cast<int>(endpoints.size())},
            {"enabledEndpointCount", enabledCount},
            {"metadataValid", metadataErrors.empty()},
            {"metadataErrors", metadataErrors}
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
    return serviceResultToJson(ServiceResult::failure(1, message, "request_failed"));
}

nlohmann::json apiPayloadJson(const ServiceResult& status, const std::string& key, nlohmann::json value)
{
    nlohmann::json body = serviceResultToJson(status);
    body[key] = std::move(value);
    if (status.ok)
    {
        body.erase("exitCode");
    }
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
        items.push_back(jobSnapshotToJson(job, false));
    }
    return apiOkJson("jobs", items);
}

nlohmann::json apiRemovedJobsJson(int removed)
{
    return apiOkJson("removed", removed);
}

nlohmann::json apiEndpointsResponseJson()
{
    return apiOkJson("endpoints", apiEndpointsToJson());
}

nlohmann::json apiInfoResponseJson(const char* entryName)
{
    return apiOkJson("info", apiInfoToJson(entryName));
}

} // namespace mosaicraft
