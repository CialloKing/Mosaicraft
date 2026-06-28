// ============================================================
// Mosaicraft 核心单元测试 (doctest)
// 跨平台：Windows / Linux / macOS
// 构建: cmake --build build --target mosaicraft_tests
// 运行: ctest --test-dir build -C Release
// ============================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../core/ApiJson.h"
#include "../core/ApiMetadata.h"
#include "../core/ApiRequestParser.h"
#include "../core/FeatureUtils.h"
#include "../core/JobManager.h"
#include "../core/LegacyRun.h"
#include "../core/Version.h"
#include <vector>
#include <cmath>
#include <cstdint>
#include <string>
#include <algorithm>

using namespace mosaicraft;

// ============================================================
// labDistance
// ============================================================

TEST_CASE("labDistance — identical colors")
{
    CHECK(labDistance(50, 0, 0, 50, 0, 0) == doctest::Approx(0.0));
}

TEST_CASE("labDistance — L channel difference")
{
    // dL=100, da=0, db=0 → dist = sqrt(10000)/100 = 1.0
    CHECK(labDistance(0, 0, 0, 100, 0, 0) == doctest::Approx(1.0));
}

TEST_CASE("labDistance — combined difference")
{
    // dL=30, da=40, db=0 → sqrt(900+1600+0)/100 = sqrt(2500)/100 = 50/100 = 0.5
    CHECK(labDistance(0, 0, 0, 30, 40, 0) == doctest::Approx(0.5));
}

TEST_CASE("labDistance — all channels")
{
    // dL=60, da=60, db=30 → sqrt(3600+3600+900)/100 = sqrt(8100)/100 = 90/100 = 0.9
    CHECK(labDistance(0, 0, 0, 60, 60, 30) == doctest::Approx(0.9));
}

// ============================================================
// gridDistance (generic)
// ============================================================

TEST_CASE("gridDistance — identical vectors (48-dim 4x4)")
{
    std::vector<float> a(48, 0.5f);
    std::vector<float> b(48, 0.5f);
    CHECK(gridDistance(a, b) == doctest::Approx(0.0));
}

TEST_CASE("gridDistance — size mismatch returns penalty")
{
    std::vector<float> a(48, 0.0f);
    std::vector<float> b(192, 0.0f);
    CHECK(gridDistance(a, b) == doctest::Approx(1e6));
}

TEST_CASE("gridDistance — non-multiple-of-3 returns penalty")
{
    std::vector<float> a(50, 0.0f);
    std::vector<float> b(50, 0.0f);
    CHECK(gridDistance(a, b) == doctest::Approx(1e6));
}

// ============================================================
// gridDistance8x8
// ============================================================

TEST_CASE("gridDistance8x8 — identical vectors")
{
    std::vector<float> a(192, 1.0f);
    std::vector<float> b(192, 1.0f);
    CHECK(gridDistance8x8(a, b, false) == doctest::Approx(0.0));
}

TEST_CASE("gridDistance8x8 — size mismatch returns penalty")
{
    std::vector<float> a(48, 0.0f);
    std::vector<float> b(192, 0.0f);
    CHECK(gridDistance8x8(a, b, false) == doctest::Approx(1e6));
}

TEST_CASE("gridDistance8x8 — with sqrt gives non-negative result")
{
    std::vector<float> a(192, 1.0f);
    std::vector<float> b(192, 2.0f);
    double d = gridDistance8x8(a, b, true);
    CHECK(d >= 0.0);
}

// ============================================================
// upsampleGrid4x4to8x8
// ============================================================

TEST_CASE("upsampleGrid4x4to8x8 — correct output size")
{
    std::vector<float> src(48, 0.1f);
    std::vector<float> dst;
    upsampleGrid4x4to8x8(src, dst);
    CHECK(dst.size() == 192);
}

TEST_CASE("upsampleGrid4x4to8x8 — nearest-neighbor duplication")
{
    // Each 4x4 cell maps to a 2x2 block in 8x8 via srcR=r/2, srcC=c/2.
    // 8x8 rows 0-1 map to 4x4 row 0; 8x8 cols 0-1 map to 4x4 col 0
    // 8x8 cols 2-3 map to 4x4 col 1, etc.
    std::vector<float> src(48, 0.0f);
    // Cell (0,0) = LAB(10,20,30), Cell (0,1) = LAB(40,50,60)
    src[0] = 10; src[1] = 20; src[2] = 30;   // cell 0
    src[3] = 40; src[4] = 50; src[5] = 60;   // cell 1

    std::vector<float> dst;
    upsampleGrid4x4to8x8(src, dst);

    // 8x8 (0,0) → 4x4 (0,0) → idx 0
    CHECK(dst[0] == 10); CHECK(dst[1] == 20); CHECK(dst[2] == 30);
    // 8x8 (0,2) → 4x4 (0,1) → idx (0*8+2)*3 = 6
    CHECK(dst[6] == 40); CHECK(dst[7] == 50); CHECK(dst[8] == 60);
    // 8x8 (1,0) → 4x4 (0,0) → idx (1*8+0)*3 = 24
    CHECK(dst[24] == 10); CHECK(dst[25] == 20); CHECK(dst[26] == 30);
}

// ============================================================
// tinyMSE
// ============================================================

TEST_CASE("tinyMSE — identical vectors")
{
    std::vector<uint8_t> a(256, 128);
    std::vector<uint8_t> b(256, 128);
    CHECK(tinyMSE(a, b) == doctest::Approx(0.0));
}

TEST_CASE("tinyMSE — maximum difference")
{
    // All 0 vs all 255 → diff=255 each, squared=65025*256/(256*65025)=1.0
    std::vector<uint8_t> a(256, 0);
    std::vector<uint8_t> b(256, 255);
    CHECK(tinyMSE(a, b) == doctest::Approx(1.0));
}

TEST_CASE("tinyMSE — size mismatch returns penalty")
{
    std::vector<uint8_t> a(200, 0);
    std::vector<uint8_t> b(256, 0);
    CHECK(tinyMSE(a, b) == doctest::Approx(1.0));
}

// ============================================================
// lbpDistance
// ============================================================

TEST_CASE("lbpDistance — identical histograms")
{
    std::vector<float> a(256, 0.01f);
    std::vector<float> b(256, 0.01f);
    CHECK(lbpDistance(a, b) == doctest::Approx(0.0));
}

TEST_CASE("lbpDistance — size mismatch returns penalty")
{
    std::vector<float> a(128, 0.0f);
    std::vector<float> b(256, 0.0f);
    CHECK(lbpDistance(a, b) == doctest::Approx(1.0));
}

TEST_CASE("lbpDistance — orthogonal histograms")
{
    // Two completely separate histograms → sum(|a-b|)/2 = 1.0
    std::vector<float> a(256, 0.0f);
    std::vector<float> b(256, 0.0f);
    a[0] = 1.0f;
    b[255] = 1.0f;
    // sum|a-b| = |1-0|*1 + |0-1|*1 + |0-0|*254 = 2.0
    // dist = 2.0 / 2.0 = 1.0
    CHECK(lbpDistance(a, b) == doctest::Approx(1.0));
}

// ============================================================
// gridDistance8x8 — spatial weight correctness
// ============================================================

TEST_CASE("gridDistance8x8 — center cell has higher weight than corner")
{
    // Construct vectors where only one cell differs by known amount.
    // Center cell (row3,col3 → idx 27) should have higher weight
    // than top-left corner (row0,col0 → idx 0).
    // Weight[0] = 0.85, Weight[27] = 1.09 (from the w[] array)
    // This test verifies the spatial weighting is applied correctly.

    std::vector<float> a(192, 0.0f);
    std::vector<float> b(192, 0.0f);

    // Case 1: difference at corner (0,0) → idx 0-2
    b[0] = 100.0f; b[1] = 0.0f; b[2] = 0.0f;
    double distCorner = gridDistance8x8(a, b, false);

    // Reset
    b[0] = 0.0f;

    // Case 2: difference at center (3,3) → idx (3*8+3)*3 = 27*3 = 81
    b[81] = 100.0f; b[82] = 0.0f; b[83] = 0.0f;
    double distCenter = gridDistance8x8(a, b, false);

    // Center weight (w[27]=1.09) > corner weight (w[0]=0.85)
    // So distCenter should be > distCorner for the same LAB difference
    CHECK(distCenter > distCorner);
}

// ============================================================
// API support classes
// ============================================================

TEST_CASE("Version is shared by CLI and API")
{
    CHECK(std::string(kVersion) == "1.12.3");
}

TEST_CASE("API endpoint metadata is shared and self-describing")
{
    auto endpoints = apiEndpointMetadata(false);

    CHECK(endpoints.size() >= 16);

    auto findEndpoint = [&](const std::string& path) {
        return std::find_if(endpoints.begin(), endpoints.end(),
            [&](const ApiEndpointMetadata& endpoint) { return endpoint.path == path; });
    };

    auto mosaicJob = findEndpoint("/api/jobs/mosaic");
    REQUIRE(mosaicJob != endpoints.end());
    CHECK(mosaicJob->category == "jobs");
    CHECK(std::find(mosaicJob->requestFields.begin(), mosaicJob->requestFields.end(),
        "inputPath") != mosaicJob->requestFields.end());

    auto legacyRun = findEndpoint("/api/run");
    REQUIRE(legacyRun != endpoints.end());
    CHECK(legacyRun->legacy);
    CHECK_FALSE(legacyRun->enabled);

    auto enabledEndpoints = apiEndpointMetadata(true);
    auto enabledLegacyRun = std::find_if(enabledEndpoints.begin(), enabledEndpoints.end(),
        [](const ApiEndpointMetadata& endpoint) { return endpoint.path == "/api/run"; });
    REQUIRE(enabledLegacyRun != enabledEndpoints.end());
    CHECK(enabledLegacyRun->enabled);
}

TEST_CASE("API feature metadata is shared")
{
    const auto features = apiFeatureList();
    CHECK(std::find(features.begin(), features.end(), "mosaic-jobs") != features.end());
    CHECK(std::find(features.begin(), features.end(), "database-maintenance") != features.end());
}

TEST_CASE("API JSON serialization is shared")
{
    ServiceResult result = ServiceResult::failure(7, "bad request");
    auto resultJson = serviceResultToJson(result);
    CHECK_FALSE(resultJson["ok"].get<bool>());
    CHECK(resultJson["exitCode"].get<int>() == 7);
    CHECK(resultJson["message"].get<std::string>() == "bad request");

    auto endpointsJson = apiEndpointsToJson(false);
    REQUIRE(endpointsJson.is_array());
    auto legacy = std::find_if(endpointsJson.begin(), endpointsJson.end(),
        [](const nlohmann::json& endpoint) { return endpoint["path"] == "/api/run"; });
    REQUIRE(legacy != endpointsJson.end());
    CHECK((*legacy)["legacy"].get<bool>());
    CHECK_FALSE((*legacy)["enabled"].get<bool>());

    auto infoJson = apiInfoToJson(false, "MosaicraftWebUI");
    CHECK(infoJson["version"].get<std::string>() == "1.12.3");
    CHECK(infoJson["entry"].get<std::string>() == "MosaicraftWebUI");
    CHECK_FALSE(infoJson["api"]["legacyRunEnabled"].get<bool>());
}

TEST_CASE("API JSON serialization covers jobs and database shapes")
{
    JobSnapshot job;
    job.id = "job-1";
    job.type = "mosaic";
    job.state = JobState::Succeeded;
    job.result = ServiceResult::success("done");
    job.inputPath = "in.jpg";
    job.outputPath = "out.jpg";
    auto jobJson = jobSnapshotToJson(job);
    CHECK(jobJson["id"].get<std::string>() == "job-1");
    CHECK(jobJson["state"].get<std::string>() == "succeeded");
    CHECK(jobJson["ok"].get<bool>());

    DatabaseUsage usage;
    usage.empty = false;
    usage.total = 3;
    usage.unusedCount = 1;
    usage.top.push_back({2, 5, 80});
    usage.unusedPreview.push_back({3, "unused.jpg"});
    auto usageJson = databaseUsageToJson(usage);
    CHECK(usageJson["total"].get<int>() == 3);
    CHECK(usageJson["top"][0]["tiles"].get<int>() == 80);
    CHECK(usageJson["unusedPreview"][0]["filePath"].get<std::string>() == "unused.jpg");
}

TEST_CASE("API JSON response envelopes are shared")
{
    JobSnapshot job;
    job.id = "job-42";
    job.type = "build";
    job.state = JobState::Running;
    job.result = ServiceResult::success("running");

    auto jobEnvelope = apiJobJson(job);
    CHECK(jobEnvelope["ok"].get<bool>());
    CHECK(jobEnvelope["job"]["id"].get<std::string>() == "job-42");

    auto jobError = apiJobErrorJson("only queued jobs can be canceled", job);
    CHECK_FALSE(jobError["ok"].get<bool>());
    CHECK(jobError["message"].get<std::string>() == "only queued jobs can be canceled");
    CHECK(jobError["job"]["state"].get<std::string>() == "running");

    auto payloadOk = apiPayloadJson(ServiceResult::success("ok"), "usage", nlohmann::json{{"total", 3}});
    CHECK(payloadOk["ok"].get<bool>());
    CHECK(payloadOk["message"].get<std::string>() == "ok");
    CHECK(payloadOk["usage"]["total"].get<int>() == 3);
    CHECK_FALSE(payloadOk.contains("exitCode"));

    auto payloadError = apiPayloadJson(ServiceResult::failure(2, "missing"), "purge", nlohmann::json{{"failedCount", 1}});
    CHECK_FALSE(payloadError["ok"].get<bool>());
    CHECK(payloadError["exitCode"].get<int>() == 2);
    CHECK(payloadError["purge"]["failedCount"].get<int>() == 1);

    auto endpoints = apiEndpointsResponseJson(false);
    CHECK(endpoints["ok"].get<bool>());
    CHECK(endpoints["endpoints"].is_array());

    auto info = apiInfoResponseJson(false, "MosaicraftWebUI");
    CHECK(info["ok"].get<bool>());
    CHECK(info["info"]["entry"].get<std::string>() == "MosaicraftWebUI");
}

TEST_CASE("API request parser builds mosaic requests")
{
    MosaicRequest request;
    std::string error;

    CHECK(parseMosaicRequestJson(
        R"({"inputPath":"in.jpg","dbPath":"db.sqlite","outputPath":"out.png","quality":120,"writeMode":"stream","deepZoom":true,"cpu":true,"outputTile":"90x160"})",
        request,
        error));

    CHECK(request.inputPath == "in.jpg");
    CHECK(request.dbPath == "db.sqlite");
    CHECK(request.outputPath == "out.png");
    CHECK(request.config.jpegQuality == 100);
    CHECK(request.config.writeMode == "stream");
    CHECK(request.config.deepZoom);
    CHECK(request.config.tiledOutput);
    CHECK_FALSE(request.config.useGpu);
    CHECK(request.config.nativeTileW == 90);
    CHECK(request.config.nativeTileH == 160);
}

TEST_CASE("API request parser reports invalid fields")
{
    MosaicRequest request;
    std::string error;

    CHECK_FALSE(parseMosaicRequestJson(R"({"writeMode":"invalid"})", request, error));
    CHECK(error == "writeMode must be auto, stream, or batch");

    error.clear();
    BuildRequest build;
    CHECK_FALSE(parseBuildRequestJson(R"({"threads":"many"})", build, error));
    CHECK(error == "threads must be a number");
}

TEST_CASE("API request parser applies database and inspect requests")
{
    std::string error;

    DatabaseUsageRequest usage;
    CHECK(applyDatabaseUsageRequestJson(
        R"({"dbPath":"library.db","limit":0,"showUnused":true})",
        usage,
        error));
    CHECK(usage.dbPath == "library.db");
    CHECK(usage.limit == 1);
    CHECK(usage.showUnused);

    InspectRequest inspect;
    CHECK(applyInspectRequestJson(
        R"({"imagePath":"target.jpg","dbPath":"library.db"})",
        inspect,
        error));
    CHECK(inspect.imagePath == "target.jpg");
    CHECK(inspect.dbPath == "library.db");
}

TEST_CASE("API request parser merges query and JSON body")
{
    std::string error;

    DatabaseUsageRequest usage;
    CHECK(parseDatabaseUsageRequestApi(
        {{"db", "query.db"}, {"limit", "0"}, {"unused", "1"}},
        R"({"dbPath":"body.db","limit":12,"showUnused":false})",
        usage,
        error));
    CHECK(usage.dbPath == "body.db");
    CHECK(usage.limit == 12);
    CHECK_FALSE(usage.showUnused);

    DatabasePurgeRequest purge;
    CHECK(parseDatabasePurgeRequestApi(
        {{"db", "query.db"}, {"dryRun", "0"}, {"confirm", "1"}},
        "",
        purge,
        error));
    CHECK(purge.dbPath == "query.db");
    CHECK_FALSE(purge.dryRun);
    CHECK(purge.confirm);

    InspectRequest inspect;
    CHECK(parseInspectRequestApi(
        {{"input", "query.jpg"}, {"db", "query.db"}},
        R"({"imagePath":"body.jpg"})",
        inspect,
        error));
    CHECK(inspect.imagePath == "body.jpg");
    CHECK(inspect.dbPath == "query.db");
}

TEST_CASE("Legacy run command validation is shared")
{
    auto valid = validateLegacyRunCommand("mosaicraft db-health -d library.db");
    CHECK(valid.ok);
    CHECK(valid.commandName == "db-health");
    CHECK(valid.subCommand == "db-health -d library.db");

    auto injected = validateLegacyRunCommand("mosaicraft mosaic\nrm -rf home");
    CHECK_FALSE(injected.ok);
    CHECK(injected.error == "ERROR: invalid characters in command");

    auto unknown = validateLegacyRunCommand("mosaicraft shell");
    CHECK_FALSE(unknown.ok);
    CHECK(unknown.error == "ERROR: unknown command: shell");
}

TEST_CASE("JobManager can cancel queued jobs and clear finished jobs")
{
    JobManager manager(false);

    BuildRequest request;
    request.inputDir = "__unused_input_for_job_test__";
    request.outputDir = "__unused_output_for_job_test__";
    request.allowPrompt = false;

    const std::string jobId = manager.submitBuild(request);

    JobSnapshot canceled;
    CHECK(manager.cancelQueuedJob(jobId, canceled));
    CHECK(canceled.id == jobId);
    CHECK(canceled.state == JobState::Canceled);

    CHECK(manager.clearFinishedJobs() == 1);

    JobSnapshot afterClear;
    CHECK_FALSE(manager.getJob(jobId, afterClear));
}
