#include "compute/CudaBackend.h"
#include "core/Database.h"
#include "core/FeatureIndex.h"
#include "core/FeatureUtils.h"
#include "core/UnicodeIO.h"
#include <chrono>
#include <iostream>

using namespace mosaicraft;

// 独立比较同一批查询的检索耗时，避免把加载/建索引计入旧版查询时间。
int main(int argc, char **argv)
{
    if (argc != 5 && argc != 6)
    {
        std::cerr << "usage: mosaicraft_ann_benchmark DB TARGET OLD_ANN NEW_ANN [--build]\n";
        return 1;
    }
    try
    {
        Database db(argv[1]);
        auto records = db.allRecords();
        using Clock = std::chrono::steady_clock;
        const bool build = argc == 6 && std::string(argv[5]) == "--build";
        double oldBuildMs = 0, currentBuildMs = 0;
        FeatureIndex current;
        auto buildStart = Clock::now();
        if (build)
        {
            if (!current.build(records))
            {
                throw std::runtime_error("index build failed");
            }
            currentBuildMs = std::chrono::duration<double, std::milli>(Clock::now() - buildStart).count();
        }
        if (!build && !current.load(argv[4], 196, records))
        {
            throw std::runtime_error("new cache unavailable; run mosaic first");
        }
        hnswlib::L2Space space(708);
        std::unique_ptr<hnswlib::HierarchicalNSW<float>> old;
        if (build)
        {
            buildStart = Clock::now();
            old = std::make_unique<hnswlib::HierarchicalNSW<float>>(&space, records.size(), 16, 200);
            std::vector<float> data(records.size() * 708, 0);
            for (size_t i = 0; i < records.size(); ++i)
            {
                auto* row = data.data() + i * 708;
                const auto& record = records[i];
                row[0] = static_cast<float>(record.avgL / 255.0);
                row[1] = static_cast<float>(record.avgA / 255.0);
                row[2] = static_cast<float>(record.avgB / 255.0);
                for (size_t j = 0; j < std::min<size_t>(192, record.grid4x4.size()); ++j)
                {
                    row[3 + j] = record.grid4x4[j] / 255.0f;
                }
                row[451] = static_cast<float>(record.edgeDensity);
            }
            for (size_t i = 0; i < records.size(); ++i)
            {
                old->addPoint(data.data() + i * 708, records[i].id);
            }
            oldBuildMs = std::chrono::duration<double, std::milli>(Clock::now() - buildStart).count();
        }
        else
        {
            old = std::make_unique<hnswlib::HierarchicalNSW<float>>(&space, argv[3], false, records.size());
        }
        auto target = imreadUnicode(argv[2], cv::IMREAD_COLOR);
        if (target.empty())
        {
            throw std::runtime_error("target unavailable");
        }
        const int nx = (target.cols + 8) / 9, ny = (target.rows + 15) / 16;
        cv::copyMakeBorder(target, target, 0, ny * 16 - target.rows, 0, nx * 9 - target.cols, cv::BORDER_REFLECT);
        const int count = nx * ny, candidates = std::min(150, static_cast<int>(records.size()));
        std::vector<float> queries(static_cast<size_t>(count) * 196), oldQueries(static_cast<size_t>(count) * 708, 0);
        const bool gpu = cuda::isCudaAvailable();
        cuda::FeatureWorkspace workspace;
        constexpr int batch = 256;
        std::vector<uint8_t> images(batch * 180 * 320 * 3), tiny(batch * 256);
        std::vector<float> grid(batch * 192), lbp(batch * 256);
        std::vector<double> lab(batch * 3), edge(batch);
        for (int first = 0; first < count; first += batch)
        {
            const int size = std::min(batch, count - first);
            for (int i = 0; i < size; ++i)
            {
                const int ti = first + i;
                cv::Mat resized;
                cv::resize(target(cv::Rect((ti % nx) * 9, (ti / nx) * 16, 9, 16)), resized, cv::Size(180, 320), 0, 0,
                           cv::INTER_LINEAR);
                if (gpu)
                {
                    std::memcpy(images.data() + i * 180 * 320 * 3, resized.data, 180 * 320 * 3);
                }
                else
                {
                    cv::Mat labImage;
                    cv::cvtColor(resized, labImage, cv::COLOR_BGR2Lab);
                    auto mean = cv::mean(labImage);
                    for (int c = 0; c < 3; ++c)
                    {
                        lab[i * 3 + c] = mean[c];
                    }
                    auto row = computeGrid8x8FromLab(labImage);
                    std::copy(row.begin(), row.end(), grid.begin() + i * 192);
                    edge[i] = computeEdgeDensity(resized);
                }
            }
            if (gpu && cuda::extractFeaturesRaw(images.data(), size, 180, 320, lab.data(), grid.data(), tiny.data(),
                                                edge.data(), lbp.data(), &workspace) != 0)
            {
                throw std::runtime_error("feature extraction failed");
            }
            for (int i = 0; i < size; ++i)
            {
                float *q = queries.data() + static_cast<size_t>(first + i) * 196;
                float *previous = oldQueries.data() + static_cast<size_t>(first + i) * 708;
                for (int c = 0; c < 3; ++c)
                {
                    q[c] = static_cast<float>(lab[i * 3 + c] / 255.0);
                }
                for (int c = 0; c < 192; ++c)
                {
                    q[3 + c] = grid[i * 192 + c] / 255.0f;
                }
                q[195] = static_cast<float>(edge[i]);
                std::copy_n(q, 195, previous);
                previous[451] = q[195];
            }
        }
        std::vector<int> serial;
        std::cout << "{\"tiles\":" << count << ",\"library\":" << records.size()
                  << ",\"old_build_ms\":" << oldBuildMs << ",\"current_build_ms\":" << currentBuildMs
                  << ",\"gpu_features\":" << (gpu ? "true" : "false") << ",\"runs\":[";
        for (int run = 0; run < 4; ++run)
        {
            auto start = Clock::now();
            std::vector<int> previous(static_cast<size_t>(count) * candidates);
            for (int i = 0; i < count; ++i)
            {
                auto found = old->searchKnn(oldQueries.data() + static_cast<size_t>(i) * 708, candidates);
                for (size_t j = found.size(); j > 0; --j)
                {
                    previous[static_cast<size_t>(i) * candidates + j - 1] =
                        current.idToAllRecordsIndex(static_cast<int>(found.top().second));
                    found.pop();
                }
            }
            const double oldMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            start = Clock::now();
            serial = current.queryBatch(queries, candidates, 1);
            const double serialMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            start = Clock::now();
            auto parallel = current.queryBatch(queries, candidates);
            const double parallelMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (serial != parallel)
            {
                throw std::runtime_error("parallel query order differs");
            }
            size_t changed = 0;
            for (size_t i = 0; i < serial.size(); ++i)
            {
                changed += serial[i] != previous[i];
            }
            std::cout << (run ? "," : "") << "{\"run\":" << run << ",\"old_ms\":" << oldMs
                      << ",\"serial_ms\":" << serialMs << ",\"parallel_ms\":" << parallelMs
                      << ",\"changed_candidate_positions\":" << changed << "}";
        }
        std::cout << "]}\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
