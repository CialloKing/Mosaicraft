// ============================================================
// Mosaicraft 核心单元测试 (doctest)
// 跨平台：Windows / Linux / macOS
// 构建: cmake --build build --target mosaicraft_tests
// 运行: ctest --test-dir build -C Release
// ============================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../core/ApiHandlers.h"
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
    CHECK(std::string(kVersion) == "1.13.7");
}

TEST_CASE("API endpoint metadata is shared and self-describing")
{
    auto endpoints = apiEndpointMetadata(false);

    CHECK(endpoints.size() >= 16);

    auto findEndpoint = [&](const std::string& path) {
        return std::find_if(endpoints.begin(), endpoints.end(),
            [&](const ApiEndpointMetadata& endpoint) { return endpoint.path == path; });
    };

    auto endpointDiscovery = findEndpoint("/api/endpoints");
    REQUIRE(endpointDiscovery != endpoints.end());
    CHECK(endpointDiscovery->operation == ApiOperation::Endpoints);
    CHECK(endpointDiscovery->requestShape == ApiRequestShape::None);
    CHECK(endpointDiscovery->httpPattern == "/api/endpoints");

    auto mosaicJob = findEndpoint("/api/jobs/mosaic");
    REQUIRE(mosaicJob != endpoints.end());
    CHECK(mosaicJob->operation == ApiOperation::SubmitMosaicJob);
    CHECK(mosaicJob->requestShape == ApiRequestShape::Body);
    CHECK(mosaicJob->methods == std::vector<std::string>{"POST"});
    CHECK(mosaicJob->category == "jobs");
    CHECK(std::find(mosaicJob->requestFields.begin(), mosaicJob->requestFields.end(),
        "inputPath") != mosaicJob->requestFields.end());
    CHECK(mosaicJob->requiredFields == std::vector<std::string>{"inputPath"});
    CHECK(mosaicJob->fieldAliases.at("inputPath") == std::vector<std::string>{"input"});
    CHECK(mosaicJob->fieldAliases.at("dbPath") == std::vector<std::string>{"db"});
    CHECK(mosaicJob->fieldAliases.at("outputPath") == std::vector<std::string>{"output"});
    CHECK(mosaicJob->successStatus == 202);
    CHECK(mosaicJob->responseKey == "job");
    CHECK(mosaicJob->errorStatuses == std::vector<int>{400, 500});
    CHECK(std::find(mosaicJob->errorShapes.begin(), mosaicJob->errorShapes.end(),
        "serviceResult") != mosaicJob->errorShapes.end());
    REQUIRE(mosaicJob->errorResponses.size() == 2);
    CHECK(mosaicJob->errorResponses[0].status == 400);
    CHECK(mosaicJob->errorResponses[0].shape == "serviceResult");
    CHECK(mosaicJob->sideEffects);
    CHECK(mosaicJob->longRunning);

    auto dbStats = findEndpoint("/api/db/stats");
    REQUIRE(dbStats != endpoints.end());
    CHECK(dbStats->method == "GET|POST");
    CHECK(dbStats->methods == std::vector<std::string>{"GET", "POST"});
    CHECK(dbStats->httpPattern == "/api/db/stats");
    CHECK(dbStats->queryKeys == std::vector<std::string>{"db"});
    CHECK(std::find(dbStats->acceptedQueryKeys.begin(), dbStats->acceptedQueryKeys.end(),
        "dbPath") != dbStats->acceptedQueryKeys.end());
    CHECK(dbStats->fieldAliases.at("dbPath") == std::vector<std::string>{"db"});
    CHECK(dbStats->successStatus == 200);
    CHECK(dbStats->responseKey == "stats");
    CHECK(dbStats->errorStatuses == std::vector<int>{400, 500});

    auto jobStatus = findEndpoint("/api/jobs/{id}");
    REQUIRE(jobStatus != endpoints.end());
    CHECK(jobStatus->httpPattern == R"(/api/jobs/([A-Za-z0-9_-]+))");
    CHECK(jobStatus->queryKeys.empty());
    CHECK(jobStatus->requiredFields == std::vector<std::string>{"id"});
    CHECK(jobStatus->errorStatuses == std::vector<int>{404});
    CHECK(jobStatus->errorShapes == std::vector<std::string>{"apiError"});
    CHECK_FALSE(jobStatus->sideEffects);

    auto cancelJob = std::find_if(endpoints.begin(), endpoints.end(),
        [](const ApiEndpointMetadata& endpoint) {
            return endpoint.path == "/api/jobs/{id}" && endpoint.method == "DELETE";
        });
    REQUIRE(cancelJob != endpoints.end());
    CHECK(cancelJob->errorStatuses == std::vector<int>{404, 409});
    CHECK(std::find(cancelJob->errorShapes.begin(), cancelJob->errorShapes.end(),
        "jobError") != cancelJob->errorShapes.end());
    CHECK(cancelJob->errorResponseKeys == std::vector<std::string>{"job"});
    REQUIRE(cancelJob->errorResponses.size() == 2);
    CHECK(cancelJob->errorResponses[1].status == 409);
    CHECK(cancelJob->errorResponses[1].responseKey == "job");

    auto legacyRun = findEndpoint("/api/run");
    REQUIRE(legacyRun != endpoints.end());
    CHECK(legacyRun->operation == ApiOperation::LegacyRunDisabled);
    CHECK(legacyRun->requestShape == ApiRequestShape::LegacyCommand);
    CHECK(legacyRun->requiredFields == std::vector<std::string>{"command"});
    CHECK(legacyRun->successStatus == 200);
    CHECK(legacyRun->responseKey.empty());
    CHECK(legacyRun->errorStatuses == std::vector<int>{404});
    CHECK(legacyRun->sideEffects);
    CHECK(legacyRun->longRunning);
    CHECK(legacyRun->legacy);
    CHECK_FALSE(legacyRun->enabled);

    auto enabledEndpoints = apiEndpointMetadata(true);
    auto enabledLegacyRun = std::find_if(enabledEndpoints.begin(), enabledEndpoints.end(),
        [](const ApiEndpointMetadata& endpoint) { return endpoint.path == "/api/run"; });
    REQUIRE(enabledLegacyRun != enabledEndpoints.end());
    CHECK(enabledLegacyRun->enabled);

    CHECK(std::string(apiOperationName(ApiOperation::DatabaseUsageExport)) == "databaseUsageExport");
    CHECK(std::string(apiRequestShapeName(ApiRequestShape::JobId)) == "jobId");
}

TEST_CASE("API feature metadata is shared")
{
    const auto entryPoints = apiEntryPointMetadata();
    CHECK(entryPoints.size() == 2);
    CHECK(entryPoints[0].name == "cli");
    CHECK(entryPoints[0].executable == "mosaicraft.exe");
    CHECK(entryPoints[1].name == "webui");
    CHECK(entryPoints[1].executable == "MosaicraftWebUI.exe");

    const auto features = apiFeatureList();
    CHECK(std::find(features.begin(), features.end(), "mosaic-jobs") != features.end());
    CHECK(std::find(features.begin(), features.end(), "database-maintenance") != features.end());
    CHECK(std::string(apiContractVersion()) == "1.0");
    CHECK(apiContractMajorVersion() == 1);
    CHECK(std::string(apiCompatibilityLevel()) == "stable");
    CHECK(apiContractStable());
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
    CHECK((*legacy)["operation"].get<std::string>() == "legacyRunDisabled");
    CHECK((*legacy)["methods"].size() == 1);
    CHECK((*legacy)["methods"][0].get<std::string>() == "POST");
    CHECK((*legacy)["httpPattern"].get<std::string>() == "/api/run");
    CHECK((*legacy)["queryKeys"].empty());
    CHECK((*legacy)["acceptedQueryKeys"].empty());
    CHECK((*legacy)["successStatus"].get<int>() == 200);
    CHECK((*legacy)["responseKey"].get<std::string>().empty());
    CHECK((*legacy)["errorStatuses"].size() == 1);
    CHECK((*legacy)["errorStatuses"][0].get<int>() == 404);
    CHECK((*legacy)["errorShapes"][0].get<std::string>() == "apiError");
    CHECK((*legacy)["errorResponseKeys"].empty());
    REQUIRE((*legacy)["errorResponses"].size() == 1);
    CHECK((*legacy)["errorResponses"][0]["status"].get<int>() == 404);
    CHECK((*legacy)["errorResponses"][0]["shape"].get<std::string>() == "apiError");
    CHECK((*legacy)["fieldAliases"].empty());
    CHECK((*legacy)["requiredFields"].size() == 1);
    CHECK((*legacy)["requiredFields"][0].get<std::string>() == "command");
    CHECK((*legacy)["sideEffects"].get<bool>());
    CHECK((*legacy)["longRunning"].get<bool>());
    CHECK((*legacy)["requestShape"].get<std::string>() == "legacyCommand");
    CHECK((*legacy)["legacy"].get<bool>());
    CHECK_FALSE((*legacy)["enabled"].get<bool>());

    auto infoJson = apiInfoToJson(false, "MosaicraftWebUI");
    CHECK(infoJson["version"].get<std::string>() == "1.13.7");
    CHECK(infoJson["entry"].get<std::string>() == "MosaicraftWebUI");
    REQUIRE(infoJson["entryPoints"].is_array());
    CHECK(infoJson["entryPoints"].size() == 2);
    CHECK(infoJson["entryPoints"][0]["executable"].get<std::string>() == "mosaicraft.exe");
    CHECK(infoJson["entryPoints"][1]["executable"].get<std::string>() == "MosaicraftWebUI.exe");
    CHECK(infoJson["api"]["contractVersion"].get<std::string>() == "1.0");
    CHECK(infoJson["api"]["contractMajorVersion"].get<int>() == 1);
    CHECK(infoJson["api"]["compatibility"].get<std::string>() == "stable");
    CHECK(infoJson["api"]["stable"].get<bool>());
    CHECK_FALSE(infoJson["api"]["legacyRunEnabled"].get<bool>());
    CHECK(infoJson["api"]["endpointCount"].get<int>() == 17);
    CHECK(infoJson["api"]["enabledEndpointCount"].get<int>() == 16);
    CHECK(infoJson["api"]["legacyEndpointCount"].get<int>() == 1);
    CHECK(infoJson["api"]["metadataValid"].get<bool>());
    CHECK(infoJson["api"]["metadataErrors"].empty());

    auto enabledInfoJson = apiInfoToJson(true, "MosaicraftWebUI");
    CHECK(enabledInfoJson["api"]["legacyRunEnabled"].get<bool>());
    CHECK(enabledInfoJson["api"]["endpointCount"].get<int>() == 17);
    CHECK(enabledInfoJson["api"]["enabledEndpointCount"].get<int>() == 17);
    CHECK(enabledInfoJson["api"]["metadataValid"].get<bool>());
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

TEST_CASE("API request parser follows endpoint field aliases")
{
    MosaicRequest mosaic;
    std::string error;
    CHECK(parseMosaicRequestJson(
        R"({"input":"target.jpg","db":"library.db","output":"out.webp","outputFormat":"webp","jpegQuality":75,"topNRandom":7,"penalty":0.2,"tiledOutput":true,"deepzoom":true})",
        mosaic,
        error));
    CHECK(mosaic.inputPath == "target.jpg");
    CHECK(mosaic.dbPath == "library.db");
    CHECK(mosaic.outputPath == "out.webp");
    CHECK(mosaic.config.outputFormat == "webp");
    CHECK(mosaic.config.jpegQuality == 75);
    CHECK(mosaic.config.topNrandom == 7);
    CHECK(mosaic.config.usePenalty == doctest::Approx(0.2));
    CHECK(mosaic.config.tiledOutput);
    CHECK(mosaic.config.deepZoom);

    BuildRequest build;
    error.clear();
    CHECK(parseBuildRequestJson(
        R"({"input":"photos","output":"library-out","db":"library.db","append":true,"force":true})",
        build,
        error));
    CHECK(build.inputDir == "photos");
    CHECK(build.outputDir == "library-out");
    CHECK(build.dbPath == "library.db");
    CHECK(build.appendMode);
    CHECK(build.forceMode);

    DatabaseUsageRequest usage;
    error.clear();
    CHECK(applyDatabaseUsageRequestJson(R"({"db":"usage.db","unused":true})", usage, error));
    CHECK(usage.dbPath == "usage.db");
    CHECK(usage.showUnused);
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

    error.clear();
    CHECK_FALSE(parseMosaicRequestJson(R"({"outputTile":"90x160bad"})", request, error));
    CHECK(error == "outputTile must use WxH format");

    error.clear();
    CHECK_FALSE(parseBuildRequestJson(R"({"normalizeSize":"180xbad"})", build, error));
    CHECK(error == "normalizeSize must use WxH format");

    error.clear();
    CHECK_FALSE(parseMosaicRequestJson(R"({"tileW":9.5})", request, error));
    CHECK(error == "tileW must be an integer");
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

TEST_CASE("API request parser accepts canonical query keys from metadata")
{
    std::string error;

    DatabaseUsageRequest usage;
    CHECK(parseDatabaseUsageRequestApi(
        {{"dbPath", "canonical.db"}, {"limit", "2"}, {"showUnused", "1"}},
        "",
        usage,
        error));
    CHECK(usage.dbPath == "canonical.db");
    CHECK(usage.limit == 2);
    CHECK(usage.showUnused);

    DatabaseUsageExportRequest exportRequest;
    CHECK(parseDatabaseUsageExportRequestApi(
        {{"dbPath", "export.db"}, {"outputDir", "used"}, {"confirm", "1"}},
        "",
        exportRequest,
        error));
    CHECK(exportRequest.dbPath == "export.db");
    CHECK(exportRequest.outputDir == "used");
    CHECK(exportRequest.confirm);

    InspectRequest inspect;
    CHECK(parseInspectRequestApi(
        {{"imagePath", "target.jpg"}, {"dbPath", "library.db"}},
        "",
        inspect,
        error));
    CHECK(inspect.imagePath == "target.jpg");
    CHECK(inspect.dbPath == "library.db");
}

TEST_CASE("API request parser rejects invalid query values")
{
    std::string error;

    DatabaseUsageRequest usage;
    CHECK_FALSE(parseDatabaseUsageRequestApi(
        {{"db", "usage.db"}, {"limit", "bad"}},
        "",
        usage,
        error));
    CHECK(error == "limit must be a number");

    error.clear();
    CHECK_FALSE(parseDatabaseUsageRequestApi(
        {{"db", "usage.db"}, {"unused", "maybe"}},
        "",
        usage,
        error));
    CHECK(error == "showUnused must be a boolean");

    DatabaseUsageExportRequest exportRequest;
    error.clear();
    CHECK_FALSE(parseDatabaseUsageExportRequestApi(
        {{"db", "usage.db"}, {"output", "used"}, {"confirm", "maybe"}},
        "",
        exportRequest,
        error));
    CHECK(error == "confirm must be a boolean");

    DatabasePurgeRequest purge;
    error.clear();
    CHECK_FALSE(parseDatabasePurgeRequestApi(
        {{"db", "purge.db"}, {"dryRun", "sometimes"}},
        "",
        purge,
        error));
    CHECK(error == "dryRun must be a boolean");
}

TEST_CASE("API handlers expose discovery and health without HTTP")
{
    auto info = apiInfo(false, "MosaicraftWebUI");
    CHECK(info.status == 200);
    CHECK(info.body["ok"].get<bool>());
    CHECK(info.body["info"]["entry"].get<std::string>() == "MosaicraftWebUI");
    CHECK_FALSE(info.body["info"]["api"]["legacyRunEnabled"].get<bool>());

    auto endpoints = apiEndpoints(true);
    CHECK(endpoints.status == 200);
    CHECK(endpoints.body["ok"].get<bool>());
    auto legacy = std::find_if(endpoints.body["endpoints"].begin(), endpoints.body["endpoints"].end(),
        [](const nlohmann::json& endpoint) { return endpoint["path"] == "/api/run"; });
    REQUIRE(legacy != endpoints.body["endpoints"].end());
    CHECK((*legacy)["enabled"].get<bool>());

    auto ping = apiPing();
    CHECK(ping.status == 200);
    CHECK(ping.body["ok"].get<bool>());
    CHECK(ping.body["message"].get<std::string>() == "pong");

    auto legacyDisabled = apiLegacyRunDisabled();
    CHECK(legacyDisabled.status == 404);
    CHECK_FALSE(legacyDisabled.body["ok"].get<bool>());
    CHECK(legacyDisabled.body["message"].get<std::string>().find("disabled") != std::string::npos);
}

TEST_CASE("API request dispatcher routes operations without HTTP")
{
    JobManager manager(false);

    auto ping = handleApiRequest(apiRequest(ApiOperation::Ping), manager);
    CHECK(ping.status == 200);
    CHECK(ping.body["message"].get<std::string>() == "pong");

    auto info = handleApiRequest(apiInfoRequest(false, "MosaicraftWebUI"), manager);
    CHECK(info.status == 200);
    CHECK(info.body["info"]["entry"].get<std::string>() == "MosaicraftWebUI");

    auto submitted = handleApiRequest(
        apiBodyRequest(ApiOperation::SubmitBuildJob,
            R"({"inputDir":"__unused_dispatch_input__","outputDir":"__unused_dispatch_output__"})"),
        manager);
    CHECK(submitted.status == 202);
    std::string jobId = submitted.body["job"]["id"].get<std::string>();

    auto fetched = handleApiRequest(apiJobRequest(ApiOperation::GetJob, jobId), manager);
    CHECK(fetched.status == 200);
    CHECK(fetched.body["job"]["type"].get<std::string>() == "build");

    auto badUsage = handleApiRequest(
        apiQueryRequest(ApiOperation::DatabaseUsage,
            {{"db", "__dispatch_status_test__.db"}},
            R"({"limit":"bad"})"),
        manager);
    CHECK(badUsage.status == 400);
    CHECK(badUsage.body["message"].get<std::string>() == "limit must be a number");
}

TEST_CASE("API request factories set semantic fields")
{
    ApiRequestContext context;
    context.query = {{"db", "library.db"}};
    context.body = "body";
    context.id = "job-1";
    context.legacyRunEnabled = true;
    context.entryName = "MosaicraftWebUI";

    auto operation = apiOperationRequest(ApiOperation::DatabaseUsage, std::move(context));
    CHECK(operation.operation == ApiOperation::DatabaseUsage);
    CHECK(operation.query.at("db") == "library.db");
    CHECK(operation.body == "body");
    CHECK(operation.id == "job-1");
    CHECK(operation.legacyRunEnabled);
    CHECK(std::string(operation.entryName) == "MosaicraftWebUI");

    auto body = apiBodyRequest(ApiOperation::Mosaic, "body");
    CHECK(body.operation == ApiOperation::Mosaic);
    CHECK(body.body == "body");

    auto query = apiQueryRequest(ApiOperation::DatabaseUsage, {{"db", "library.db"}}, "json");
    CHECK(query.operation == ApiOperation::DatabaseUsage);
    CHECK(query.query.at("db") == "library.db");
    CHECK(query.body == "json");

    auto job = apiJobRequest(ApiOperation::CancelJob, "job-1");
    CHECK(job.operation == ApiOperation::CancelJob);
    CHECK(job.id == "job-1");

    auto info = apiInfoRequest(true, "MosaicraftWebUI");
    CHECK(info.operation == ApiOperation::Info);
    CHECK(info.legacyRunEnabled);
    CHECK(std::string(info.entryName) == "MosaicraftWebUI");
}

TEST_CASE("API endpoint request factory applies endpoint context")
{
    auto endpoints = apiEndpointMetadata(true);
    auto findEndpoint = [&](const std::string& path) {
        return std::find_if(endpoints.begin(), endpoints.end(),
            [&](const ApiEndpointMetadata& endpoint) { return endpoint.path == path; });
    };

    auto infoEndpoint = findEndpoint("/api/info");
    REQUIRE(infoEndpoint != endpoints.end());
    ApiRequestContext infoContext;
    infoContext.legacyRunEnabled = true;
    infoContext.entryName = "CustomEntry";
    auto info = apiEndpointRequest(*infoEndpoint, std::move(infoContext));
    CHECK(info.operation == ApiOperation::Info);
    CHECK(info.legacyRunEnabled);
    CHECK(std::string(info.entryName) == "CustomEntry");

    auto usageEndpoint = findEndpoint("/api/db/usage");
    REQUIRE(usageEndpoint != endpoints.end());
    ApiRequestContext usageContext;
    usageContext.query = {{"db", "library.db"}};
    usageContext.body = R"({"limit":3})";
    auto usage = apiEndpointRequest(*usageEndpoint, std::move(usageContext));
    CHECK(usage.operation == ApiOperation::DatabaseUsage);
    CHECK(usage.query.at("db") == "library.db");
    CHECK(usage.body == R"({"limit":3})");

    auto jobEndpoint = findEndpoint("/api/jobs/{id}");
    REQUIRE(jobEndpoint != endpoints.end());
    ApiRequestContext jobContext;
    jobContext.id = "job-7";
    auto job = apiEndpointRequest(*jobEndpoint, std::move(jobContext));
    CHECK(job.operation == ApiOperation::GetJob);
    CHECK(job.id == "job-7");
}

TEST_CASE("API operation query keys are centralized")
{
    auto usageKeyList = apiQueryKeyList(ApiOperation::DatabaseUsage);
    CHECK(usageKeyList == std::vector<std::string>{"db", "limit", "unused"});

    auto usageKeys = apiQueryKeys(ApiOperation::DatabaseUsage);
    CHECK(std::find(usageKeys.begin(), usageKeys.end(), std::string("db")) != usageKeys.end());
    CHECK(std::find(usageKeys.begin(), usageKeys.end(), std::string("limit")) != usageKeys.end());
    CHECK(std::find(usageKeys.begin(), usageKeys.end(), std::string("unused")) != usageKeys.end());

    auto exportKeys = apiQueryKeys(ApiOperation::DatabaseUsageExport);
    CHECK(std::find(exportKeys.begin(), exportKeys.end(), std::string("output")) != exportKeys.end());
    CHECK(std::find(exportKeys.begin(), exportKeys.end(), std::string("confirm")) != exportKeys.end());

    auto inspectKeys = apiQueryKeys(ApiOperation::Inspect);
    CHECK(std::find(inspectKeys.begin(), inspectKeys.end(), std::string("input")) != inspectKeys.end());
    CHECK(std::find(inspectKeys.begin(), inspectKeys.end(), std::string("db")) != inspectKeys.end());

    CHECK(apiQueryKeys(ApiOperation::Ping).empty());
    CHECK(apiQueryKeyList(ApiOperation::Ping).empty());
    CHECK(apiQueryKeys(ApiOperation::SubmitBuildJob).empty());

    auto acceptedUsageKeys = apiAcceptedQueryKeyList(ApiOperation::DatabaseUsage);
    CHECK(std::find(acceptedUsageKeys.begin(), acceptedUsageKeys.end(), "db") != acceptedUsageKeys.end());
    CHECK(std::find(acceptedUsageKeys.begin(), acceptedUsageKeys.end(), "dbPath") != acceptedUsageKeys.end());
    CHECK(std::find(acceptedUsageKeys.begin(), acceptedUsageKeys.end(), "unused") != acceptedUsageKeys.end());
    CHECK(std::find(acceptedUsageKeys.begin(), acceptedUsageKeys.end(), "showUnused") != acceptedUsageKeys.end());

    auto acceptedInspectKeys = apiAcceptedQueryKeyList(ApiOperation::Inspect);
    CHECK(std::find(acceptedInspectKeys.begin(), acceptedInspectKeys.end(), "input") != acceptedInspectKeys.end());
    CHECK(std::find(acceptedInspectKeys.begin(), acceptedInspectKeys.end(), "imagePath") != acceptedInspectKeys.end());

    auto endpoints = apiEndpointMetadata(false);
    auto usageEndpoint = std::find_if(endpoints.begin(), endpoints.end(),
        [](const ApiEndpointMetadata& endpoint) { return endpoint.operation == ApiOperation::DatabaseUsage; });
    REQUIRE(usageEndpoint != endpoints.end());
    CHECK(usageEndpoint->acceptedQueryKeys == acceptedUsageKeys);
}

TEST_CASE("API endpoint metadata carries dispatch operations")
{
    for (const auto& endpoint : apiEndpointMetadata(false)) {
        CHECK(std::string(apiOperationName(endpoint.operation)) != "unknown");
        CHECK(std::string(apiRequestShapeName(endpoint.requestShape)) != "unknown");
        CHECK_FALSE(endpoint.methods.empty());
        CHECK_FALSE(endpoint.httpPattern.empty());
        if (endpoint.requestShape == ApiRequestShape::Query) {
            CHECK_FALSE(endpoint.queryKeys.empty());
        } else {
            CHECK(endpoint.queryKeys.empty());
        }
        auto keys = apiQueryKeys(endpoint.operation);
        if (endpoint.path == "/api/db/usage") {
            CHECK(endpoint.requestShape == ApiRequestShape::Query);
            CHECK(std::find(keys.begin(), keys.end(), std::string("limit")) != keys.end());
        }
        if (endpoint.path == "/api/inspect") {
            CHECK(endpoint.requestShape == ApiRequestShape::Query);
            CHECK(std::find(keys.begin(), keys.end(), std::string("input")) != keys.end());
        }
        if (endpoint.path == "/api/jobs/{id}") {
            CHECK(endpoint.requestShape == ApiRequestShape::JobId);
        }
    }
}

TEST_CASE("API endpoint metadata validation catches contract errors")
{
    CHECK(validateApiEndpointMetadata(apiEndpointMetadata(false)).empty());
    CHECK(validateApiEndpointMetadata(apiEndpointMetadata(true)).empty());

    auto endpoints = apiEndpointMetadata(false);
    endpoints.push_back(endpoints.front());
    auto duplicateErrors = validateApiEndpointMetadata(endpoints);
    CHECK(std::find_if(duplicateErrors.begin(), duplicateErrors.end(),
        [](const std::string& error) {
            return error.find("duplicate API operation") != std::string::npos ||
                   error.find("duplicate API route") != std::string::npos;
        }) != duplicateErrors.end());

    ApiEndpointMetadata bad;
    bad.operation = ApiOperation::Ping;
    bad.requestShape = ApiRequestShape::None;
    bad.methods = {"PATCH"};
    bad.queryKeys = {"db"};
    bad.acceptedQueryKeys = {"other"};
    bad.requiredFields = {"missing"};
    bad.fieldAliases = {{"missing", {"alias"}}};
    bad.successStatus = 500;
    bad.errorStatuses = {200};
    bad.errorShapes = {"unknown"};
    bad.errorResponses = {{200, "missing", "payload"}};
    auto badErrors = validateApiEndpointMetadata({bad});
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("unsupported method") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("empty path") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("non-query endpoint has queryKeys") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("non-success successStatus") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("non-error errorStatus") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("unsupported errorShape") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("non-error errorResponse status") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("errorResponses shape missing") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("errorResponses key missing") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("non-query endpoint has acceptedQueryKeys") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("acceptedQueryKeys missing query key") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("required field is not listed") != std::string::npos; }) != badErrors.end());
    CHECK(std::find_if(badErrors.begin(), badErrors.end(),
        [](const std::string& error) { return error.find("field alias target is not listed") != std::string::npos; }) != badErrors.end());
}

TEST_CASE("API response contract validation matches handlers")
{
    auto endpoints = apiEndpointMetadata(false);
    auto findEndpoint = [&](ApiOperation operation) {
        return std::find_if(endpoints.begin(), endpoints.end(),
            [&](const ApiEndpointMetadata& endpoint) { return endpoint.operation == operation; });
    };

    JobManager manager(false);

    auto pingEndpoint = findEndpoint(ApiOperation::Ping);
    REQUIRE(pingEndpoint != endpoints.end());
    CHECK(validateApiResponseContract(*pingEndpoint, apiPing()).empty());

    auto mosaicEndpoint = findEndpoint(ApiOperation::SubmitMosaicJob);
    REQUIRE(mosaicEndpoint != endpoints.end());
    auto badMosaic = apiSubmitMosaicJob(R"({"writeMode":"invalid"})", manager);
    CHECK(badMosaic.status == 400);
    CHECK(validateApiResponseContract(*mosaicEndpoint, badMosaic).empty());

    auto buildEndpoint = findEndpoint(ApiOperation::SubmitBuildJob);
    REQUIRE(buildEndpoint != endpoints.end());
    auto build = apiSubmitBuildJob(
        R"({"inputDir":"__contract_input__","outputDir":"__contract_output__"})",
        manager);
    CHECK(build.status == 202);
    CHECK(validateApiResponseContract(*buildEndpoint, build).empty());
    std::string jobId = build.body["job"]["id"].get<std::string>();

    auto cancelEndpoint = findEndpoint(ApiOperation::CancelJob);
    REQUIRE(cancelEndpoint != endpoints.end());
    auto canceled = apiCancelJob(jobId, manager);
    CHECK(canceled.status == 200);
    CHECK(validateApiResponseContract(*cancelEndpoint, canceled).empty());
    auto cancelAgain = apiCancelJob(jobId, manager);
    CHECK(cancelAgain.status == 409);
    CHECK(validateApiResponseContract(*cancelEndpoint, cancelAgain).empty());

    auto getEndpoint = findEndpoint(ApiOperation::GetJob);
    REQUIRE(getEndpoint != endpoints.end());
    auto missing = apiGetJob("missing", manager);
    CHECK(missing.status == 404);
    CHECK(validateApiResponseContract(*getEndpoint, missing).empty());

    auto exportEndpoint = findEndpoint(ApiOperation::DatabaseUsageExport);
    REQUIRE(exportEndpoint != endpoints.end());
    auto missingExportConfirm = apiDatabaseUsageExport(
        {{"db", "__contract_status_test__.db"}, {"output", "__contract_export__"}},
        "");
    CHECK(missingExportConfirm.status == 400);
    CHECK(validateApiResponseContract(*exportEndpoint, missingExportConfirm).empty());

    auto purgeEndpoint = findEndpoint(ApiOperation::DatabasePurge);
    REQUIRE(purgeEndpoint != endpoints.end());
    auto badPurge = apiDatabasePurge(
        {{"db", "__contract_status_test__.db"}},
        R"({"dryRun":"no"})");
    CHECK(badPurge.status == 400);
    CHECK(validateApiResponseContract(*purgeEndpoint, badPurge).empty());

    ApiResponse undeclared{418, apiErrorJson("teapot")};
    auto undeclaredErrors = validateApiResponseContract(*pingEndpoint, undeclared);
    CHECK(std::find_if(undeclaredErrors.begin(), undeclaredErrors.end(),
        [](const std::string& error) {
            return error.find("not declared") != std::string::npos;
        }) != undeclaredErrors.end());

    ApiResponse missingPayload{409, apiErrorJson("only queued jobs can be canceled")};
    auto shapeErrors = validateApiResponseContract(*cancelEndpoint, missingPayload);
    CHECK(std::find_if(shapeErrors.begin(), shapeErrors.end(),
        [](const std::string& error) {
            return error.find("shape does not match") != std::string::npos;
        }) != shapeErrors.end());
}

TEST_CASE("API handlers expose structured jobs without HTTP")
{
    JobManager manager(false);

    auto badMosaic = apiSubmitMosaicJob(R"({"writeMode":"invalid"})", manager);
    CHECK(badMosaic.status == 400);
    CHECK_FALSE(badMosaic.body["ok"].get<bool>());
    CHECK(badMosaic.body["message"].get<std::string>() == "writeMode must be auto, stream, or batch");

    auto submitted = apiSubmitBuildJob(
        R"({"inputDir":"__unused_input_for_api_handler_test__","outputDir":"__unused_output_for_api_handler_test__"})",
        manager);
    CHECK(submitted.status == 202);
    CHECK(submitted.body["ok"].get<bool>());
    std::string jobId = submitted.body["job"]["id"].get<std::string>();

    auto fetched = apiGetJob(jobId, manager);
    CHECK(fetched.status == 200);
    CHECK(fetched.body["job"]["type"].get<std::string>() == "build");

    auto listed = apiListJobs(manager);
    CHECK(listed.status == 200);
    CHECK(listed.body["jobs"].is_array());
    CHECK(listed.body["jobs"].size() == 1);

    auto canceled = apiCancelJob(jobId, manager);
    CHECK(canceled.status == 200);
    CHECK(canceled.body["job"]["state"].get<std::string>() == "canceled");

    auto missing = apiGetJob("missing", manager);
    CHECK(missing.status == 404);
    CHECK_FALSE(missing.body["ok"].get<bool>());

    auto cleared = apiClearFinishedJobs(manager);
    CHECK(cleared.status == 200);
    CHECK(cleared.body["removed"].get<int>() == 1);
}

TEST_CASE("API handlers centralize database and inspect status mapping")
{
    auto badUsage = apiDatabaseUsage(
        {{"db", "__api_handler_status_test__.db"}},
        R"({"limit":"bad"})");
    CHECK(badUsage.status == 400);
    CHECK_FALSE(badUsage.body["ok"].get<bool>());
    CHECK(badUsage.body["message"].get<std::string>() == "limit must be a number");

    auto badUsageQuery = apiDatabaseUsage(
        {{"db", "__api_handler_status_test__.db"}, {"limit", "bad"}},
        "");
    CHECK(badUsageQuery.status == 400);
    CHECK_FALSE(badUsageQuery.body["ok"].get<bool>());
    CHECK(badUsageQuery.body["message"].get<std::string>() == "limit must be a number");

    auto missingExportConfirm = apiDatabaseUsageExport(
        {{"db", "__api_handler_status_test__.db"}, {"output", "__unused_export__"}},
        "");
    CHECK(missingExportConfirm.status == 400);
    CHECK_FALSE(missingExportConfirm.body["ok"].get<bool>());
    CHECK(missingExportConfirm.body["export"]["outputDir"].get<std::string>() == "__unused_export__");

    auto badPurge = apiDatabasePurge(
        {{"db", "__api_handler_status_test__.db"}},
        R"({"dryRun":"no"})");
    CHECK(badPurge.status == 400);
    CHECK_FALSE(badPurge.body["ok"].get<bool>());
    CHECK(badPurge.body["message"].get<std::string>() == "dryRun must be a boolean");

    auto badPurgeQuery = apiDatabasePurge(
        {{"db", "__api_handler_status_test__.db"}, {"dryRun", "maybe"}},
        "");
    CHECK(badPurgeQuery.status == 400);
    CHECK_FALSE(badPurgeQuery.body["ok"].get<bool>());
    CHECK(badPurgeQuery.body["message"].get<std::string>() == "dryRun must be a boolean");

    auto badInspect = apiInspect({{"input", ""}}, R"({"imagePath":123})");
    CHECK(badInspect.status == 400);
    CHECK_FALSE(badInspect.body["ok"].get<bool>());
    CHECK(badInspect.body["message"].get<std::string>() == "imagePath must be a string");
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
