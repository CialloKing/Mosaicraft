#include "core/Database.h"
#include "core/FeatureIndex.h"
#include "core/FeatureMatrix.h"
#include "core/FeaturePack.h"
#include "core/GridDuplicateCache.h"
#include "core/MosaicEngine.h"
#include "core/UnicodeIO.h"
#include "compute/CudaBackend.h"
#include <opencv2/imgproc.hpp>
#include <deque>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <set>
#include <unordered_map>

using namespace mosaicraft;

// 复用五场景中的 3600x6400、9x16 输入块，定位首次分歧；不会修改数据库或选图公式。
int main(int argc, char **argv)
{
    if (argc != 5 || !cuda::isCudaAvailable())
    {
        std::cerr << "usage: benchmark_cuda_replay <target> <snapshot.db> <history.indices.bin> <tile>\n";
        return 1;
    }
    const int tile = std::stoi(argv[4]);
    if (tile < 0 || tile >= 160000)
    {
        return 1;
    }
    Database db(argv[2], Database::OpenMode::Existing);
    const auto records = db.allRecords();
    if (records.size() < 150)
    {
        return 1;
    }
    const auto directory = pathToUtf8(u8path(records.front().tinyPath).parent_path());
    FeatureIndex index;
    if (!index.load(directory + "/lib.ann196", FeatureIndex::kDimension, records))
    {
        return 1;
    }
    cv::Mat target;
    cv::resize(imreadUnicode(argv[1]), target, {3600, 6400}, 0, 0, cv::INTER_AREA);
    constexpr int count = 256, candidates = 150;
    const int first = tile / count * count, local = tile - first;
    const int rows = std::min(count, 160000 - first);
    std::vector<uint8_t> input(rows * 180 * 320 * 3), tiny(rows * 256), libraryTiny, tinyValid, lbpValid;
    std::vector<double> lab(rows * 3), edge(rows);
    std::vector<float> grid(rows * 192), lbp(rows * 256), query, libraryLbp;
    for (int i = 0; i < rows; ++i)
    {
        const int ti = first + i;
        cv::Mat resized(320, 180, CV_8UC3, input.data() + static_cast<size_t>(i) * 180 * 320 * 3);
        cv::resize(target(cv::Rect((ti % 400) * 9, (ti / 400) * 16, 9, 16)), resized, {180, 320}, 0, 0,
                    cv::INTER_LINEAR);
    }
    std::vector<int> history(160000);
    std::ifstream historyFile(u8path(argv[3]), std::ios::binary);
    if (!historyFile.read(reinterpret_cast<char *>(history.data()), history.size() * sizeof(int)))
    {
        return 1;
    }
    std::deque<int> recent;
    std::unordered_map<int, int> frequency, last;
    for (int i = 0; i < tile; ++i)
    {
        if (history[i] < 0)
        {
            continue;
        }
        if (static_cast<size_t>(history[i]) >= records.size())
        {
            return 1;
        }
        recent.push_back(history[i]);
        ++frequency[history[i]];
        last[history[i]] = i;
        if (recent.size() > 800)
        {
            --frequency[recent.front()];
            recent.pop_front();
        }
    }
    GridDuplicateCache duplicates(records);
    const int libraryCount = static_cast<int>(records.size());
    std::vector<double> libraryLab(libraryCount * 3), libraryEdge(libraryCount);
    std::vector<float> libraryGrid(libraryCount * 192);
    std::vector<int> use(libraryCount), needed(libraryCount);
    std::iota(needed.begin(), needed.end(), 0);
    for (int i = 0; i < libraryCount; ++i)
    {
        if (records[i].grid4x4.size() != 192)
        {
            return 1;
        }
        libraryLab[i * 3] = records[i].avgL;
        libraryLab[i * 3 + 1] = records[i].avgA;
        libraryLab[i * 3 + 2] = records[i].avgB;
        std::copy(records[i].grid4x4.begin(), records[i].grid4x4.end(), libraryGrid.begin() + i * 192);
        libraryEdge[i] = records[i].edgeDensity;
        use[i] = records[i].useCount;
    }
    if (!FeaturePack::load(directory, records, needed, libraryTiny, libraryLbp, tinyValid, lbpValid))
    {
        return 1;
    }
    cuda::GpuLibrary library;
    struct Cleanup
    {
        cuda::GpuLibrary &library;
        ~Cleanup()
        {
            cuda::freeLibrary(library);
        }
    } cleanup{library};
    if (!cuda::uploadLibrary(library, libraryLab.data(), libraryGrid.data(), libraryTiny.data(), libraryEdge.data(),
                             libraryLbp.data(), use.data(), libraryCount, nullptr, tinyValid.data(), lbpValid.data()))
    {
        return 1;
    }
    cuda::FeatureWorkspace legacy, pipeline;
    cuda::ScoreWorkspace scoring;
    if (!pipeline.preparePipeline({}) || !scoring.preparePipeline(1, candidates, 65536, pipeline.residentBytes()))
    {
        return 1;
    }
    MosaicEngine::Config cfg;
    double weights[] = {cfg.labWeight, cfg.gridWeight, cfg.tinyWeight, cfg.edgeWeight, cfg.lbpWeight};
    const double sum = std::accumulate(std::begin(weights), std::end(weights), 0.0);
    for (auto &weight : weights)
    {
        weight /= sum;
    }
    std::vector<double> referenceEdge;
    std::vector<float> referenceLbp;
    std::vector<uint8_t> referenceTiny;
    std::set<int> legacyWinners, pipelineWinners;
    size_t exactReplays = 0;
    for (int run = 0; run < 64; ++run)
    {
        const bool async = run % 2 != 0;
        if (async)
        {
            std::copy(input.begin(), input.end(), pipeline.pipelineInput());
            pipeline.beginBatch(rows);
            pipeline.extract(0, rows, lab.data(), grid.data(), tiny.data(), edge.data(), lbp.data());
        }
        else if (cuda::extractFeaturesRaw(input.data(), rows, 180, 320, lab.data(), grid.data(), tiny.data(),
                                          edge.data(), lbp.data(), &legacy) < 0)
        {
            return 2;
        }
        if (run == 0)
        {
            referenceEdge = edge;
            referenceLbp = lbp;
            referenceTiny = tiny;
        }
        if (edge != referenceEdge || lbp != referenceLbp || tiny != referenceTiny)
        {
            std::cerr << "Canny/TinyImage/LBP changed\n";
            return 2;
        }
        const double *l = lab.data() + local * 3;
        buildTileVector(l[0], l[1], l[2], FeatureMatrix<float, 192>::Row<const float>(grid.data() + local * 192),
                         FeatureMatrix<uint8_t, 256>::Row<const uint8_t>(tiny.data() + local * 256), edge[local],
                         FeatureMatrix<float, 256>::Row<const float>(lbp.data() + local * 256), query);
        auto positions = index.query(query.data(), candidates);
        if (positions.size() != candidates)
        {
            return 2;
        }
        for (auto &id : positions)
        {
            id = index.idToAllRecordsIndex(id);
            if (id >= 0 && std::abs(records[id].avgL - l[0]) > cfg.lRange)
            {
                id = -1;
            }
        }
        std::vector<double> original(candidates), resident(candidates);
        if (!cuda::scoreBatch(1, l, l + 1, l + 2, grid.data() + local * 192, tiny.data() + local * 256,
                              edge.data() + local, lbp.data() + local * 256, positions.data(), candidates, library,
                              weights, weights + 1, weights + 2, weights + 3, weights + 4, cfg.usePenalty,
                              original.data()))
        {
            return 2;
        }
        if (async)
        {
            // 在同一次提取上回放，隔离原子累加波动；全部 150 个评分必须逐位一致。
            if (!cuda::scoreDeviceBatch(pipeline.deviceView(local, 1), l, l + 1, l + 2, edge.data() + local,
                                        positions.data(), candidates, library, weights, weights + 1, weights + 2,
                                        weights + 3, weights + 4, cfg.usePenalty, resident.data(), scoring) ||
                std::memcmp(resident.data(), original.data(), original.size() * sizeof(double)) != 0)
            {
                std::cerr << "fixed-feature resident replay mismatch\n";
                return 2;
            }
            exactReplays += candidates;
        }
        for (int j = 0; j < candidates; ++j)
        {
            const int id = positions[j];
            if (id < 0)
            {
                continue;
            }
            const int n = frequency[id];
            original[j] += n >= 3 ? cfg.neighborPenalty : n == 2 ? cfg.neighborPenalty * 0.4 :
                                                               n == 1 ? cfg.neighborPenalty * 0.1 : 0;
            if (last.count(id) && tile - last[id] < 400)
            {
                original[j] += 500;
            }
            for (auto it = recent.rbegin(); it != recent.rend() && it - recent.rbegin() < 50; ++it)
            {
                if (duplicates.similar(id, *it, 0.010))
                {
                    original[j] += 200;
                    break;
                }
            }
        }
        std::vector<int> order(candidates);
        std::iota(order.begin(), order.end(), 0);
        std::partial_sort(order.begin(), order.begin() + 1, order.end(),
                          [&](int a, int b)
                          {
                              return original[a] < original[b];
                          });
        const int winner = positions[order[0]];
        (async ? pipelineWinners : legacyWinners).insert(winner);
        std::cout << (async ? "pipeline" : "legacy") << ' ' << run << " L=" << std::setprecision(17) << l[0]
                  << " winner=" << winner << " score=" << original[order[0]] << '\n';
    }
    std::cout << "exact_replayed_scores=" << exactReplays << " stable_Canny_Tiny_LBP=1 legacy_winners=";
    for (int winner : legacyWinners)
    {
        std::cout << winner << ',';
    }
    std::cout << " pipeline_winners=";
    for (int winner : pipelineWinners)
    {
        std::cout << winner << ',';
    }
    std::cout << '\n';
}
