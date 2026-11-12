#include "../compute/PortableCompute.h"

TEST_CASE("portable backend options preserve defaults and reject invalid score modes")
{
    using namespace mosaicraft;
    CHECK(parseBackend("vulkan") == BackendPolicy::Vulkan);
    CHECK(parseBackend("metal") == BackendPolicy::Metal);
    CHECK(parseScoreMode("fast") == ScoreMode::Fast);
    CHECK_THROWS_AS(parseScoreMode("fp16"), std::invalid_argument);
    MosaicRequest request;
    RequestError error;
    REQUIRE(parseMosaicRequestJson(
        R"({"inputPath":"target.png","dbPath":"library.db","backend":"vulkan","device":"vulkan:1","scoreMode":"fast"})",
        request, error));
    CHECK(request.config.device == "vulkan:1");
    CHECK(request.config.scoreMode == ScoreMode::Fast);
    CHECK(mosaicRequestToJson(request)["scoreMode"] == "fast");
    CHECK_THROWS(compute::select(BackendPolicy::Vulkan, "vulkan:invalid"));
    CHECK_FALSE(parseMosaicRequestJson(
        R"({"inputPath":"target.png","dbPath":"library.db","backend":"cpu","device":"vulkan:0"})", request, error));
    error = RequestError{};
    CHECK_FALSE(
        parseMosaicRequestJson(R"({"inputPath":"target.png","dbPath":"library.db","device":""})", request, error));
}

TEST_CASE("portable GPU integer features reproduce the CPU definition")
{
    using namespace mosaicraft;
    const char *required = std::getenv("MOSAICRAFT_REQUIRE_DEVICE");
    bool tested = false;
    WorkerPool workers(2);
    for (const auto &device : compute::devices())
    {
        if (!device.available || (device.backend != BackendPolicy::Vulkan && device.backend != BackendPolicy::Metal) ||
            (required && device.id != required))
        {
            continue;
        }
        tested = true;
        CAPTURE(device.id);
        compute::FeatureWorkspace workspace(device);
        CHECK_FALSE(workspace.prepare(8192, 8192, 1, 1024, 1024));
        for (const auto size : {cv::Size(1, 1),
                                {7, 9},
                                {8, 8},
                                {9, 17},
                                {31, 17},
                                {180, 320},
                                {255, 257},
                                {256, 256},
                                {512, 512},
                                {1537, 97},
                                {8192, 1},
                                {1, 8192}})
        {
            CAPTURE(size.width);
            CAPTURE(size.height);
            REQUIRE(workspace.prepare(size.width, size.height, 3, 64 * 1024 * 1024, 64 * 1024 * 1024));
            cv::Mat padded(size.height + 2, size.width + 3, CV_8UC3);
            cv::RNG random(20261113);
            random.fill(padded, cv::RNG::UNIFORM, 0, 256);
            cv::Mat image = padded(cv::Rect(1, 1, size.width, size.height));
            std::vector<compute::Feature> output;
            workspace.extract({image, image}, output, workers);
            REQUIRE(output.size() == 2);
            ImageRecord expected;
            FeatureExtractor().compute(image, expected);
            cv::Mat gray;
            cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
            const auto tiny = computeTinyImageFromGray(gray);
            const auto lbp = computeLBPHistogramFromGray(gray);
            for (const auto &feature : output)
            {
                CHECK(feature.lab[0] == expected.avgL);
                CHECK(feature.lab[1] == expected.avgA);
                CHECK(feature.lab[2] == expected.avgB);
                CHECK(feature.edge == expected.edgeDensity);
                CHECK(std::equal(feature.grid.begin(), feature.grid.end(), expected.gridLab.begin()));
                CHECK(std::equal(feature.tiny.begin(), feature.tiny.end(), tiny.begin()));
                CHECK(std::equal(feature.lbp.begin(), feature.lbp.end(), lbp.begin()));
            }
            std::vector<compute::Feature> tail;
            workspace.extract({image}, tail, workers);
            REQUIRE(tail.size() == 1);
            CHECK(tail[0].lab == output[0].lab);
        }
    }
    if (required)
    {
        REQUIRE(tested);
    }
}

TEST_CASE("portable FP32 reference retains missing feature penalties")
{
    using namespace mosaicraft;
    std::vector<ImageRecord> records(2);
    for (size_t i = 0; i < records.size(); ++i)
    {
        records[i].id = int(i + 1);
        records[i].avgL = 123.123456789;
        records[i].gridLab.assign(192, 17.25f);
        records[i].useCount = 123456789;
    }
    FeaturePack::Reader reader(".", records);
    MatchingBuffers tiles;
    tiles.resize(1, 0, 4, false, false);
    tiles.l[0] = 121.987654321;
    tiles.labW[0] = 0.20;
    tiles.gridW[0] = 0.45;
    tiles.tinyW[0] = 0.25;
    tiles.edgeW[0] = tiles.lbpW[0] = 0.05;
    tiles.indices = {0, 1, -1, 2};
    CHECK_THROWS(reader.get(0));
    const auto distances = compute::fp32Distances(tiles, 0, records[0], FeaturePack::Reader::Row{});
    CHECK(distances[2] == 1.0f);
    CHECK(distances[4] == 1.0f);
}
