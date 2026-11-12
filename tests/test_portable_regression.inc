#include "../compute/PortableCompute.h"
#include <numeric>

TEST_CASE("portable scoring and selection meet FP64 quality and ID gates")
{
    const char *required = std::getenv("MOSAICRAFT_REQUIRE_DEVICE");
    bool tested = false;
    for (const auto &device : compute::devices())
    {
        if (!device.available || (device.backend != BackendPolicy::Vulkan && device.backend != BackendPolicy::Metal) ||
            (required && device.id != required))
        {
            continue;
        }
        tested = true;
        CAPTURE(device.id);
        for (const auto size : {cv::Size(31, 17), cv::Size(180, 320)})
        {
            TempWorkspace workspace;
            writeLibraryImages(workspace.inputDir());
            cv::Mat target(320, 180, CV_8UC3);
            cv::RNG random(20261113);
            random.fill(target, cv::RNG::UNIFORM, 0, 256);
            requireWriteImage(workspace.targetPath(), target);
            auto build = makeBuildRequest(workspace);
            build.backend = device.backend;
            build.device = device.id;
            build.normalizeWidth = size.width;
            build.normalizeHeight = size.height;
            REQUIRE(BuildService().run(build).ok);
            auto request = makeMosaicRequest(workspace);
            request.config.tileW = 9;
            request.config.tileH = 16;
            request.config.outW = 0;
            request.config.outH = 0;
            request.config.useGpu = true;
            request.config.candidates = 150;
            request.config.lRange = 255;
            request.config.threads = 2;
            Database database(utf8(workspace.dbPath()), Database::OpenMode::Existing);
            const auto records = database.allRecords();
            FeaturePack::Reader reader(utf8(workspace.libraryDir() / "features"), records, 64 * 1024 * 1024);
            std::vector<int> all(records.size());
            std::iota(all.begin(), all.end(), 0);
            reader.prepare(all);
            MatchingBuffers probe;
            probe.resize(1, 0, 4, false, false);
            probe.labW[0] = probe.gridW[0] = probe.tinyW[0] = probe.edgeW[0] = probe.lbpW[0] = 0.2;
            probe.indices = {0, 0, -1, int(records.size())};
            compute::ScoreWorkspace scorer(device);
            CHECK_FALSE(scorer.prepare(1, 4, records.size(), 1, 1));
            REQUIRE(scorer.prepare(1, 4, records.size(), 1024 * 1024, 1024 * 1024));
            scorer.uploadLibrary(records, reader);
            double output[4]{};
            scorer.score(probe, 1, 0, output);
            const auto distances = compute::fp32Distances(probe, 0, records[0], reader.get(0));
            CHECK(output[0] == doctest::Approx(std::accumulate(distances.begin(), distances.end(), 0.0) * 0.2).epsilon(1e-6));
            CHECK(output[1] == output[0]);
            CHECK(output[2] == std::numeric_limits<double>::max());
            CHECK(output[3] == std::numeric_limits<double>::max());
            for (const auto seed : {42u, 20261113u})
            {
                for (const auto topN : {1, 10})
                {
                    CAPTURE(seed);
                    CAPTURE(topN);
                    CAPTURE(size.width);
                    CAPTURE(size.height);
                    request.config.topNRandom = topN;
                    std::vector<int> baseline, compatible, fast;
                    for (int run = 0; run < 3; ++run)
                    {
                        auto config = request.config;
                        config.backend = run ? device.backend : BackendPolicy::Cpu;
                        config.device = run ? device.id : "auto";
                        config.scoreMode = run == 2 ? ScoreMode::Fast : ScoreMode::Compatible;
                        MosaicEngine engine;
                        MatchingAccess::configure(engine, MatchingLimits{}, true, seed);
                        MatchingAccess::matchOnly(engine);
                        MatchingAccess::captureSelections(engine, run == 2 ? fast : run == 1 ? compatible : baseline);
                        RuntimeContext runtime(false);
                        REQUIRE(
                            engine.generate(request.inputPath, request.dbPath, request.outputPath, config, runtime));
                        if (run)
                        {
                            CHECK(runtime.snapshot().featureBackend == backendName(device.backend));
                            CHECK(runtime.snapshot().scoreBackend == (run == 2 ? backendName(device.backend) : "cpu"));
                            CHECK(runtime.snapshot().computeDevice == device.id);
                        }
                    }
                    REQUIRE(baseline.size() == 400);
                    REQUIRE(fast.size() == baseline.size());
                    CHECK(compatible == baseline);
                    size_t changed = 0;
                    std::vector<double> errors[2];
                    MatchingBuffers tile;
                    tile.resize(1, 0, 1, false, false);
                    tile.labW[0] = request.config.labWeight;
                    tile.gridW[0] = request.config.gridWeight;
                    tile.tinyW[0] = request.config.tinyWeight;
                    tile.edgeW[0] = request.config.edgeWeight;
                    tile.lbpW[0] = request.config.lbpWeight;
                    for (size_t i = 0; i < baseline.size(); ++i)
                    {
                        REQUIRE(baseline[i] >= 0);
                        REQUIRE(fast[i] >= 0);
                        changed += records[baseline[i]].id != records[fast[i]].id;
                        cv::Mat image, lab, gray;
                        cv::resize(target(cv::Rect(int(i % 20) * 9, int(i / 20) * 16, 9, 16)), image, size);
                        cv::cvtColor(image, lab, cv::COLOR_BGR2Lab);
                        const auto average = cv::mean(lab);
                        tile.l[0] = average[0];
                        tile.a[0] = average[1];
                        tile.b[0] = average[2];
                        tile.grid[0] = computeGrid8x8FromLab(lab);
                        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
                        tile.tiny[0] = computeTinyImageFromGray(gray);
                        tile.lbp[0] = computeLBPHistogramFromGray(gray);
                        tile.edge[0] = computeEdgeDensityFromGray(gray);
                        for (int run = 0; run < 2; ++run)
                        {
                            const int index = run ? fast[i] : baseline[i];
                            errors[run].push_back(cpuBaseScore(tile, 0, records[index], reader.get(index), 0));
                        }
                    }
                    CHECK(changed <= baseline.size() / 100);
                    for (auto &values : errors)
                    {
                        std::sort(values.begin(), values.end());
                    }
                    const auto mean = [](const std::vector<double> &values)
                    { return std::accumulate(values.begin(), values.end(), 0.0) / values.size(); };
                    CHECK(mean(errors[1]) <= mean(errors[0]) * 1.01 + 1e-6);
                    for (double quantile : {0.90, 0.99})
                    {
                        const size_t index = std::min(errors[0].size() - 1, size_t(quantile * errors[0].size()));
                        CHECK(errors[1][index] <= errors[0][index] * 1.01 + 1e-6);
                    }
                    std::cout << "GPU gate " << device.id << " " << size.width << "x" << size.height << " seed=" << seed
                              << " topN=" << topN << " changed=" << changed << "/" << baseline.size()
                              << " mean=" << mean(errors[0]) << "/" << mean(errors[1]) << '\n';
                }
            }
        }
    }
    if (required)
    {
        REQUIRE(tested);
    }
}
