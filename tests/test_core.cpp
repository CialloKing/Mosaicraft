// ============================================================
// Mosaicraft 核心单元测试 (doctest)
// 跨平台：Windows / Linux / macOS
// 构建: cmake --build build --target mosaicraft_tests
// 运行: ctest --test-dir build -C Release
// ============================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../core/FeatureUtils.h"
#include "../core/JobManager.h"
#include "../core/Version.h"
#include <vector>
#include <cmath>
#include <cstdint>
#include <string>

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
