// Mosaicraft Web UI — 本地 HTTP 服务器
// 提供命令生成页面 + 结构化 API，兼容旧的 mosaicraft.exe 命令入口
#include "core/httplib.h"
#include "core/ApiMetadata.h"
#include "core/ApiRequestParser.h"
#include "core/DatabaseService.h"
#include "core/JobManager.h"
#include "core/json.hpp"
#include "core/InspectService.h"
#include "core/MosaicService.h"
#include "core/Version.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#endif

namespace
{

using json = nlohmann::json;

static bool envFlagEnabled(const char* name)
{
    const char* value = std::getenv(name);
    if (!value) return false;
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

static void setJsonResult(httplib::Response& res, const mosaicraft::ServiceResult& result)
{
    json body = {
        {"ok", result.ok},
        {"exitCode", result.exitCode},
        {"message", result.message}
    };
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

static int64_t toUnixSeconds(const std::chrono::system_clock::time_point& time)
{
    if (time == std::chrono::system_clock::time_point{}) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(
        time.time_since_epoch()).count();
}

static json jobToJson(const mosaicraft::JobSnapshot& job)
{
    return {
        {"id", job.id},
        {"type", job.type},
        {"state", mosaicraft::jobStateName(job.state)},
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

static json statsToJson(const mosaicraft::DatabaseStats& stats)
{
    json histogram = json::array();
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

static json healthToJson(const mosaicraft::DatabaseHealth& health)
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

static json usageToJson(const mosaicraft::DatabaseUsage& usage)
{
    json top = json::array();
    for (const auto& item : usage.top) {
        top.push_back({{"id", item.id}, {"runs", item.runs}, {"tiles", item.tiles}});
    }
    json unused = json::array();
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

static json usageExportToJson(const mosaicraft::DatabaseUsageExport& info)
{
    json exported = json::array();
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

static json purgeToJson(const mosaicraft::DatabasePurge& purge)
{
    json orphans = json::array();
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

static json inspectToJson(const mosaicraft::InspectResult& info)
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

static json endpointToJson(const mosaicraft::ApiEndpointMetadata& endpoint)
{
    json fields = json::array();
    for (const auto& field : endpoint.requestFields) fields.push_back(field);
    return {
        {"method", endpoint.method},
        {"path", endpoint.path},
        {"description", endpoint.description},
        {"category", endpoint.category},
        {"requestFields", fields},
        {"legacy", endpoint.legacy},
        {"enabled", endpoint.enabled}
    };
}

static json apiEndpointsJson(bool legacyRunEnabled)
{
    json endpoints = json::array();
    for (const auto& endpoint : mosaicraft::apiEndpointMetadata(legacyRunEnabled)) {
        endpoints.push_back(endpointToJson(endpoint));
    }
    return endpoints;
}

static json apiInfoJson(bool legacyRunEnabled)
{
    return {
        {"name", "Mosaicraft"},
        {"version", mosaicraft::kVersion},
        {"entry", "MosaicraftWebUI"},
        {"api", {
            {"structured", true},
            {"legacyRunEnabled", legacyRunEnabled}
        }},
        {"features", mosaicraft::apiFeatureList()}
    };
}

static void setJsonBody(httplib::Response& res, const json& body)
{
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

static bool buildMosaicRequest(const std::string& body,
                               mosaicraft::MosaicRequest& request,
                               std::string& error)
{
    return mosaicraft::parseMosaicRequestJson(body, request, error);
}

static bool buildBuildRequest(const std::string& body,
                              mosaicraft::BuildRequest& request,
                              std::string& error)
{
    return mosaicraft::parseBuildRequestJson(body, request, error);
}

static bool buildDatabaseRequest(const httplib::Request& req,
                                 mosaicraft::DatabaseRequest& request,
                                 std::string& error)
{
    if (req.has_param("db")) {
        request.dbPath = req.get_param_value("db");
    }
    return mosaicraft::applyDatabaseRequestJson(req.body, request, error);
}

static bool buildDatabaseUsageRequest(const httplib::Request& req,
                                      mosaicraft::DatabaseUsageRequest& request,
                                      std::string& error)
{
    if (req.has_param("db")) request.dbPath = req.get_param_value("db");
    if (req.has_param("limit")) request.limit = std::max(1, std::atoi(req.get_param_value("limit").c_str()));
    if (req.has_param("unused")) request.showUnused = req.get_param_value("unused") == "1";
    return mosaicraft::applyDatabaseUsageRequestJson(req.body, request, error);
}

static bool buildDatabaseUsageExportRequest(const httplib::Request& req,
                                            mosaicraft::DatabaseUsageExportRequest& request,
                                            std::string& error)
{
    if (req.has_param("db")) request.dbPath = req.get_param_value("db");
    if (req.has_param("output")) request.outputDir = req.get_param_value("output");
    if (req.has_param("confirm")) request.confirm = req.get_param_value("confirm") == "1";
    return mosaicraft::applyDatabaseUsageExportRequestJson(req.body, request, error);
}

static bool buildDatabasePurgeRequest(const httplib::Request& req,
                                      mosaicraft::DatabasePurgeRequest& request,
                                      std::string& error)
{
    if (req.has_param("db")) request.dbPath = req.get_param_value("db");
    if (req.has_param("dryRun")) request.dryRun = req.get_param_value("dryRun") != "0";
    if (req.has_param("confirm")) request.confirm = req.get_param_value("confirm") == "1";
    return mosaicraft::applyDatabasePurgeRequestJson(req.body, request, error);
}

static bool buildInspectRequest(const httplib::Request& req,
                                mosaicraft::InspectRequest& request,
                                std::string& error)
{
    if (req.has_param("input")) request.imagePath = req.get_param_value("input");
    if (req.has_param("db")) request.dbPath = req.get_param_value("db");
    return mosaicraft::applyInspectRequestJson(req.body, request, error);
}

} // namespace

#ifdef _WIN32
static std::string wideToUtf8(const std::wstring& wide)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring utf8ToWide(const std::string& utf8)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), len);
    return out;
}
#endif

static std::string readFile(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "File not found: " + path;
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

static std::string findHtml()
{
    std::vector<std::string> candidates;

#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
#else
    std::filesystem::path exeDir = std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
    // exe 同目录（发布版）
    candidates.push_back((exeDir / "index.html").string());
    candidates.push_back((exeDir / "tools/command-builder/index.html").string());
    // 开发目录：build/Release → ../../tools/command-builder/
    candidates.push_back((exeDir / "../../tools/command-builder/index.html").string());
    // 开发目录：build → ../tools/command-builder/
    candidates.push_back((exeDir / "../tools/command-builder/index.html").string());
    // 当前工作目录下的常见路径
    candidates.push_back("index.html");
    candidates.push_back("tools/command-builder/index.html");
    candidates.push_back("../tools/command-builder/index.html");
    candidates.push_back("../../tools/command-builder/index.html");

    for (const auto& c : candidates) {
        try {
            if (std::filesystem::exists(c)) {
                return std::filesystem::canonical(c).string();
            }
        } catch (...) {}
    }
    return "";  // not found
}

static std::string findMosaicraft()
{
#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path candidate = exeDir / "mosaicraft.exe";
    if (std::filesystem::exists(candidate)) return wideToUtf8(candidate.wstring());
#else
    // Linux: /proc/self/exe → exe directory
    std::filesystem::path exePath = std::filesystem::canonical("/proc/self/exe");
    std::filesystem::path exeDir = exePath.parent_path();
    std::filesystem::path candidate = exeDir / "mosaicraft";
    if (std::filesystem::exists(candidate)) return candidate.string();
#endif
    return "mosaicraft";  // hope it's in PATH
}

int main(int argc, char* argv[])
{
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (port < 1 || port > 65535) {
            std::cerr << "ERROR: Invalid port " << argv[1] << " (1-65535)" << std::endl;
            return 1;
        }
    }

    const bool legacyRunEnabled = envFlagEnabled("MOSAICRAFT_ENABLE_LEGACY_RUN");
    std::string mosaicPath = findMosaicraft();
    std::string htmlPath = findHtml();
    if (htmlPath.empty()) {
        std::cerr << "ERROR: Cannot find index.html" << std::endl;
        return 1;
    }

    std::cout << "Mosaicraft Web UI" << std::endl;
    std::cout << "  Server: http://localhost:" << port << std::endl;
    std::cout << "  Mosaicraft: " << mosaicPath << std::endl;
    std::cout << "  HTML: " << htmlPath << std::endl;
    std::cout << "  Legacy /api/run: " << (legacyRunEnabled ? "enabled" : "disabled") << std::endl;

    std::string htmlContent = readFile(htmlPath);
    std::mutex htmlMutex;
    std::mutex runMutex;
    mosaicraft::JobManager jobManager;

    httplib::Server svr;

    // 长任务超时设置（建库可能需要数分钟）
    svr.set_read_timeout(1800);   // 30 min
    svr.set_write_timeout(1800);

    // 主页
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(htmlMutex);
        res.set_content(htmlContent, "text/html; charset=utf-8");
    });

    // 重启时重新加载 HTML（开发用）
    svr.Get("/reload", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(htmlMutex);
        htmlContent = readFile(htmlPath);
        res.set_content("HTML reloaded", "text/plain");
    });

    svr.Get("/api/endpoints", [legacyRunEnabled](const httplib::Request&, httplib::Response& res) {
        setJsonBody(res, json{{"ok", true}, {"endpoints", apiEndpointsJson(legacyRunEnabled)}});
    });

    svr.Get("/api/info", [legacyRunEnabled](const httplib::Request&, httplib::Response& res) {
        setJsonBody(res, json{{"ok", true}, {"info", apiInfoJson(legacyRunEnabled)}});
    });

    svr.Post("/api/mosaic", [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::MosaicRequest request;
        std::string error;
        if (!buildMosaicRequest(req.body, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }

        try {
            std::string jobId = jobManager.submitMosaic(request);
            mosaicraft::JobSnapshot snapshot;
            if (!jobManager.waitJob(jobId, snapshot)) {
                res.status = 500;
                setJsonResult(res, mosaicraft::ServiceResult::failure(1, "job not found"));
                return;
            }
            if (!snapshot.result.ok) res.status = 500;
            setJsonResult(res, snapshot.result);
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Exception in /api/mosaic: " << e.what() << std::endl;
            res.status = 500;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, e.what()));
        } catch (...) {
            std::cerr << "[ERROR] Unknown exception in /api/mosaic" << std::endl;
            res.status = 500;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, "internal error"));
        }
    });

    svr.Post("/api/jobs/mosaic", [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::MosaicRequest request;
        std::string error;
        if (!buildMosaicRequest(req.body, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }

        try {
            std::string jobId = jobManager.submitMosaic(request);
            mosaicraft::JobSnapshot snapshot;
            jobManager.getJob(jobId, snapshot);
            res.status = 202;
            setJsonBody(res, json{{"ok", true}, {"job", jobToJson(snapshot)}});
        } catch (const std::exception& e) {
            res.status = 500;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, e.what()));
        } catch (...) {
            res.status = 500;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, "internal error"));
        }
    });

    svr.Post("/api/jobs/build", [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::BuildRequest request;
        std::string error;
        if (!buildBuildRequest(req.body, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }

        try {
            std::string jobId = jobManager.submitBuild(request);
            mosaicraft::JobSnapshot snapshot;
            jobManager.getJob(jobId, snapshot);
            res.status = 202;
            setJsonBody(res, json{{"ok", true}, {"job", jobToJson(snapshot)}});
        } catch (const std::exception& e) {
            res.status = 500;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, e.what()));
        } catch (...) {
            res.status = 500;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, "internal error"));
        }
    });

    svr.Get("/api/jobs", [&](const httplib::Request&, httplib::Response& res) {
        json jobs = json::array();
        for (const auto& job : jobManager.listJobs()) {
            jobs.push_back(jobToJson(job));
        }
        setJsonBody(res, json{{"ok", true}, {"jobs", jobs}});
    });

    svr.Delete("/api/jobs", [&](const httplib::Request&, httplib::Response& res) {
        int removed = jobManager.clearFinishedJobs();
        setJsonBody(res, json{{"ok", true}, {"removed", removed}});
    });

    svr.Get(R"(/api/jobs/([A-Za-z0-9_-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::JobSnapshot snapshot;
        std::string jobId = req.matches.size() > 1 ? req.matches[1].str() : "";
        if (!jobManager.getJob(jobId, snapshot)) {
            res.status = 404;
            setJsonBody(res, json{{"ok", false}, {"message", "job not found"}});
            return;
        }
        setJsonBody(res, json{{"ok", true}, {"job", jobToJson(snapshot)}});
    });

    svr.Delete(R"(/api/jobs/([A-Za-z0-9_-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::JobSnapshot snapshot;
        std::string jobId = req.matches.size() > 1 ? req.matches[1].str() : "";
        if (jobManager.cancelQueuedJob(jobId, snapshot)) {
            setJsonBody(res, json{{"ok", true}, {"job", jobToJson(snapshot)}});
            return;
        }
        if (snapshot.id.empty()) {
            res.status = 404;
            setJsonBody(res, json{{"ok", false}, {"message", "job not found"}});
            return;
        }
        res.status = 409;
        setJsonBody(res, json{{"ok", false}, {"message", "only queued jobs can be canceled"}, {"job", jobToJson(snapshot)}});
    });

    auto handleDbStats = [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::DatabaseRequest request;
        std::string error;
        if (!buildDatabaseRequest(req, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }
        mosaicraft::DatabaseService service;
        auto result = service.stats(request);
        if (!result.status.ok) {
            res.status = 500;
            setJsonResult(res, result.status);
            return;
        }
        setJsonBody(res, json{{"ok", true}, {"message", result.status.message}, {"stats", statsToJson(result.stats)}});
    };

    auto handleDbHealth = [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::DatabaseRequest request;
        std::string error;
        if (!buildDatabaseRequest(req, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }
        mosaicraft::DatabaseService service;
        auto result = service.health(request);
        if (!result.status.ok) {
            res.status = 500;
            setJsonResult(res, result.status);
            return;
        }
        setJsonBody(res, json{{"ok", true}, {"message", result.status.message}, {"health", healthToJson(result.health)}});
    };

    svr.Get("/api/db/stats", handleDbStats);
    svr.Post("/api/db/stats", handleDbStats);
    svr.Get("/api/db/health", handleDbHealth);
    svr.Post("/api/db/health", handleDbHealth);

    auto handleDbUsage = [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::DatabaseUsageRequest request;
        std::string error;
        if (!buildDatabaseUsageRequest(req, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }
        mosaicraft::DatabaseService service;
        auto result = service.usage(request);
        if (!result.status.ok) {
            res.status = 500;
            setJsonResult(res, result.status);
            return;
        }
        setJsonBody(res, json{{"ok", true}, {"message", result.status.message}, {"usage", usageToJson(result.usage)}});
    };

    auto handleDbUsageExport = [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::DatabaseUsageExportRequest request;
        std::string error;
        if (!buildDatabaseUsageExportRequest(req, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }
        mosaicraft::DatabaseService service;
        auto result = service.exportUsage(request);
        if (!result.status.ok) {
            res.status = result.status.exitCode == 2 ? 500 : 400;
            setJsonBody(res, json{
                {"ok", false},
                {"exitCode", result.status.exitCode},
                {"message", result.status.message},
                {"export", usageExportToJson(result.exportInfo)}
            });
            return;
        }
        setJsonBody(res, json{{"ok", true}, {"message", result.status.message}, {"export", usageExportToJson(result.exportInfo)}});
    };

    auto handleDbPurge = [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::DatabasePurgeRequest request;
        std::string error;
        if (!buildDatabasePurgeRequest(req, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }
        mosaicraft::DatabaseService service;
        auto result = service.purge(request);
        if (!result.status.ok) {
            res.status = result.status.exitCode == 2 ? 500 : 400;
            setJsonBody(res, json{
                {"ok", false},
                {"exitCode", result.status.exitCode},
                {"message", result.status.message},
                {"purge", purgeToJson(result.purge)}
            });
            return;
        }
        setJsonBody(res, json{{"ok", true}, {"message", result.status.message}, {"purge", purgeToJson(result.purge)}});
    };

    auto handleInspect = [&](const httplib::Request& req, httplib::Response& res) {
        mosaicraft::InspectRequest request;
        std::string error;
        if (!buildInspectRequest(req, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }
        mosaicraft::InspectService service;
        auto result = service.inspect(request);
        if (!result.status.ok) {
            res.status = 400;
            setJsonResult(res, result.status);
            return;
        }
        setJsonBody(res, json{{"ok", true}, {"message", result.status.message}, {"inspect", inspectToJson(result)}});
    };

    svr.Get("/api/db/usage", handleDbUsage);
    svr.Post("/api/db/usage", handleDbUsage);
    svr.Post("/api/db/usage/export", handleDbUsageExport);
    svr.Get("/api/db/purge", handleDbPurge);
    svr.Post("/api/db/purge", handleDbPurge);
    svr.Get("/api/inspect", handleInspect);
    svr.Post("/api/inspect", handleInspect);

    // 执行命令
    svr.Post("/api/run", [&, legacyRunEnabled](const httplib::Request& req, httplib::Response& res) {
        if (!legacyRunEnabled) {
            res.status = 404;
            setJsonBody(res, json{
                {"ok", false},
                {"message", "legacy /api/run is disabled; set MOSAICRAFT_ENABLE_LEGACY_RUN=1 to enable compatibility mode"}
            });
            return;
        }

        std::unique_lock<std::mutex> runLock(runMutex, std::try_to_lock);
        if (!runLock.owns_lock()) {
            res.status = 429;
            res.set_content("ERROR: another mosaicraft job is already running", "text/plain");
            return;
        }

        std::string cmd = req.body;
        if (cmd.empty()) {
            res.set_content("ERROR: empty command", "text/plain");
            return;
        }
        // 安全检查：拒绝含shell元字符的命令，防注入
        const std::string forbidden = "&|;$`(){}<>";
        bool hasControlChar = false;
        for (unsigned char ch : cmd) {
            if (ch < 0x20 || ch == 0x7f) {
                hasControlChar = true;
                break;
            }
        }
        if (cmd.find_first_of(forbidden) != std::string::npos || hasControlChar) {
            res.set_content("ERROR: invalid characters in command", "text/plain");
            return;
        }
        // 只允许已知的mosaicraft子命令
        const std::string prefix = "mosaicraft ";
        if (cmd.compare(0, prefix.size(), prefix) != 0) {
            res.set_content("ERROR: command must start with 'mosaicraft'", "text/plain");
            return;
        }
        std::string subCmd = cmd.substr(prefix.size());
        // 提取子命令名（第一个空格前的词）
        auto spacePos = subCmd.find(' ');
        std::string cmdName = (spacePos != std::string::npos) ? subCmd.substr(0, spacePos) : subCmd;
        const std::string validCmds[] = {"build","mosaic","inspect","db-stats","db-purge","db-usage","db-health"};
        bool valid = false;
        for (const auto& vc : validCmds) { if (cmdName == vc) { valid = true; break; } }
        if (!valid) {
            res.set_content("ERROR: unknown command: " + cmdName, "text/plain");
            return;
        }

        // 使用 CreateProcess 替代 popen，避免 cmd.exe 对 Unicode 路径的编码问题
        std::string output;
        try {
            std::cout << "[RUN] " << mosaicPath << " " << subCmd << std::endl;
#ifdef _WIN32
            HANDLE hReadPipe, hWritePipe;
            SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
                res.set_content("ERROR: CreatePipe failed", "text/plain");
                return;
            }
            SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

            PROCESS_INFORMATION pi = {};
            STARTUPINFOW si = {sizeof(STARTUPINFOW)};
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = hWritePipe;
            si.hStdError = hWritePipe;

            std::wstring wCmd = utf8ToWide(mosaicPath);
            if (wCmd.empty()) {
                CloseHandle(hReadPipe);
                CloseHandle(hWritePipe);
                res.set_content("ERROR: invalid executable path", "text/plain");
                return;
            }
            // subCmd is UTF-8 from the browser request.
            std::wstring wArgs = utf8ToWide(subCmd);
            std::wstring cmdLine = L"\"" + wCmd + L"\" " + wArgs;

            // 确保命令行缓冲区足够大
            std::vector<wchar_t> cmdBuf(cmdLine.size() + 1);
            wcscpy(cmdBuf.data(), cmdLine.c_str());

            if (!CreateProcessW(wCmd.c_str(), cmdBuf.data(),
                               nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(hReadPipe);
                CloseHandle(hWritePipe);
                std::cerr << "[ERROR] CreateProcess failed (" << GetLastError() << ")" << std::endl;
                res.set_content("ERROR: failed to start process", "text/plain");
                return;
            }
            CloseHandle(hWritePipe);
            CloseHandle(pi.hThread);

            char buf[4096];
            DWORD bytesRead;
            while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                std::cout.write(buf, bytesRead);
                output.append(buf, bytesRead);
            }
            CloseHandle(hReadPipe);

            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);

            if (exitCode == 0)
                res.set_content(output.empty() ? "OK" : output, "text/plain; charset=utf-8");
            else
                res.set_content("EXIT " + std::to_string(exitCode) + "\n" + output, "text/plain; charset=utf-8");
#else
            // Linux/macOS: popen 可以正确处理 UTF-8 路径
            std::string fullCmd = "\"" + mosaicPath + "\" " + subCmd;
            FILE* pipe = popen((fullCmd + " 2>&1").c_str(), "r");
            if (!pipe) {
                std::cerr << "[ERROR] popen failed: " << fullCmd << std::endl;
                res.set_content("ERROR: failed to start process", "text/plain");
                return;
            }
            setvbuf(pipe, nullptr, _IONBF, 0);
            char ch;
            while (fread(&ch, 1, 1, pipe) == 1) {
                std::cout << ch;
                output += ch;
            }
            int rc = pclose(pipe);
            if (rc == 0)
                res.set_content(output.empty() ? "OK" : output, "text/plain; charset=utf-8");
            else
                res.set_content("EXIT " + std::to_string(rc) + "\n" + output, "text/plain; charset=utf-8");
#endif
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Exception in /api/run: " << e.what() << std::endl;
            res.set_content(std::string("ERROR: ") + e.what(), "text/plain");
        } catch (...) {
            std::cerr << "[ERROR] Unknown exception in /api/run" << std::endl;
            res.set_content("ERROR: internal error", "text/plain");
        }
    });

    // 健康检查
    svr.Get("/api/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });

    std::cout << "  Listening on http://localhost:" << port << std::endl;
    std::cout << "  Press Ctrl+C to stop" << std::endl;

    // 尝试打开浏览器
#ifdef _WIN32
    std::string url = "http://localhost:" + std::to_string(port);
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string url = "http://localhost:" + std::to_string(port);
    std::system(("xdg-open " + url + " &").c_str());
#endif

    if (!svr.listen("localhost", port)) {
        std::cerr << "ERROR: Failed to start server on port " << port << std::endl;
        std::cerr << "  (port may be in use. Try: MosaicraftWebUI " << (port+1) << ")" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 1;
    }
    // listen() 只在出错或 Ctrl+C 时返回
    std::cout << std::endl << "Server stopped." << std::endl;
    // 短暂暂停让用户看到停止信息（Windows 双击启动时控制台会立即关闭）
#ifdef _WIN32
    std::this_thread::sleep_for(std::chrono::seconds(3));
#endif
    return 0;
}
