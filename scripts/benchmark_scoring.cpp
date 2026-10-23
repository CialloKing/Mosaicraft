#include "compute/CompatibleScoring.h"
#include "core/FeatureIndex.h"
#include "core/MatchingScore.h"
#include "core/UnicodeIO.h"
#include "core/json.hpp"
#include <chrono>
#include <iostream>

using namespace mosaicraft;

// 显式基准：只读私有图库，真实特征和 ANN 候选；CPU 与 CUDA 交替、逐分数精确验收。
int main(int argc, char **argv)
{
    if (argc != 2 || !cuda::isCudaAvailable())
    {
        std::cerr << "usage: benchmark_scoring <private-snapshot.db>\n";
        return 1;
    }
    Database db(argv[1], Database::OpenMode::Existing);
    const auto records = db.allRecords();
    if (records.empty())
    {
        return 1;
    }
    const auto directory = pathToUtf8(u8path(records.front().tinyPath).parent_path());
    FeaturePack::Reader reader(directory, records);
    FeatureIndex index;
    if (!index.build(records))
    {
        return 1;
    }
    constexpr int count = 256, repeats = 32;
    const int candidates = static_cast<int>(std::min<size_t>(150, records.size()));
    MatchingBuffers tiles;
    tiles.resize(count, 0, candidates, false, false);
    std::vector<float> query;
    for (int row = 0; row < count; ++row)
    {
        const int chosen = static_cast<int>((size_t(row) * 997) % records.size());
        const auto &record = records[chosen];
        const auto &features = reader.get(chosen);
        tiles.l[row] = record.avgL;
        tiles.a[row] = record.avgA;
        tiles.b[row] = record.avgB;
        tiles.edge[row] = record.edgeDensity;
        tiles.grid[row] = record.grid4x4;
        std::copy(features.tiny.begin(), features.tiny.end(), tiles.tiny[row].data());
        std::copy(features.lbp.begin(), features.lbp.end(), tiles.lbp[row].data());
        tiles.labW[row] = 0.20;
        tiles.gridW[row] = 0.45;
        tiles.tinyW[row] = 0.25;
        tiles.edgeW[row] = tiles.lbpW[row] = 0.05;
        buildTileVector(record.avgL, record.avgA, record.avgB, tiles.grid[row], tiles.tiny[row],
                         record.edgeDensity, tiles.lbp[row], query);
        auto nearest = index.query(query.data(), candidates);
        if (nearest.size() != size_t(candidates))
        {
            return 2;
        }
        for (int j = 0; j < candidates; ++j)
        {
            tiles.indices[size_t(row) * candidates + j] = index.idToAllRecordsIndex(nearest[j]);
        }
    }
    cuda::CompatibleScoreWorkspace scoring;
    if (!scoring.prepare(count, candidates, static_cast<int>(records.size()), 16ULL * 1024 * 1024) ||
        scoring.capacity() != count)
    {
        std::cerr << scoring.preparationFailure() << '\n';
        return 2;
    }
    cuda::GpuTimings upload;
    scoring.uploadLibrary(records, reader, &upload);
    std::vector<double> reference(size_t(count) * candidates), result(reference.size());
    nlohmann::json report;
    report["library_rows"] = records.size();
    report["tiles_per_run"] = count * repeats;
    report["candidates"] = candidates;
    report["library_upload_ms"] = upload.uploadMs;
    report["resident_bytes"] = scoring.libraryBytes();
    report["temporary_bytes"] = scoring.deviceBytes();
    report["pinned_bytes"] = upload.pinnedBytes;
    report["host_bytes"] = scoring.hostBytes();
    using Clock = std::chrono::steady_clock;
    for (int run = 0; run < 4; ++run)
    {
        for (int variant = 0; variant < 2; ++variant)
        {
            const bool gpu = ((variant + run) % 2) != 0;
            cuda::GpuTimings timing;
            const auto start = Clock::now();
            for (int repeat = 0; repeat < repeats; ++repeat)
            {
                if (gpu)
                {
                    scoring.score(tiles, count, 0.01, result.data(), &timing);
                }
                else
                {
                    for (int row = 0; row < count; ++row)
                    {
                        for (int j = 0; j < candidates; ++j)
                        {
                            const size_t position = size_t(row) * candidates + j;
                            const int li = tiles.indices[position];
                            reference[position] = cpuBaseScore(tiles, row, records[li], reader.get(li), 0.01);
                        }
                    }
                }
            }
            const double wall = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            report["runs"].push_back({{"run", run}, {"warmup", run == 0}, {"gpu", gpu}, {"wall_ms", wall},
                {"pack_ms", timing.uploadMs + timing.downloadMs}, {"upload_ms", timing.eventUploadMs},
                {"kernel_ms", timing.eventComputeMs}, {"download_ms", timing.eventDownloadMs}});
        }
        if (result != reference)
        {
            std::cerr << "CPU-compatible score mismatch\n";
            return 3;
        }
    }
    report["exact_scores_per_comparison"] = reference.size();
    std::cout << report.dump(2) << '\n';
    return 0;
}
