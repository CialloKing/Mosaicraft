#pragma once
#include "MosaicOutput.h"
#include "MatchingBatch.h"
#include <atomic>
#include <cstdint>

namespace mosaicraft::detail
{
// 只保存观测数据，不参与后端选择或预算决策；计时位置仍由生成流程决定。
struct MosaicMetrics
{
    double msNeighbor = 0;
    double msSort = 0;
    double msAnalysis = 0;
    double msCPUFeatures = 0;
    double msFeat = 0;
    double msANNBuild = 0;
    double msANNLoad = 0;
    double msANNQuery = 0;
    double msGPUScore = 0;
    double msSelect = 0;
    double msPlace = 0;
    double msEncode = 0;
    double msCPUScore = 0, msBaseScore = 0;
    double msPrep = 0;
    double matchingWallMs = 0;

    std::atomic<int64_t> opResizeNs{0};
    std::atomic<int64_t> opLabNs{0};
    std::atomic<int64_t> opGridNs{0};
    std::atomic<int64_t> opTinyNs{0};
    std::atomic<int64_t> opEdgeNs{0};
    std::atomic<int64_t> opLbpNs{0};

    OutputStats outputStats;
    cuda::GpuTimings featureTimings, scoreTimings, libraryTimings;
    LibraryFeatureStore::Reader::ReadStats libraryReadStats, featureReadStats;
    size_t matchingBatches = 0, matchingPeakBytes = 0, maximumBatchTiles = 0;
    bool matchingPrefetch = false;
};

struct MosaicBenchmarkContext
{
    const MosaicEngine::Config &config;
    int totalTiles;
    int candidates;
    size_t compatibleTemporaryBytes;
    double totalMs;
};

void reportOutputBenchmark(const OutputStats &stats, const MosaicEngine::Config &config,
                           RuntimeContext &runtime, bool legacyRenderKeys = false);
void reportMosaicBenchmark(const MosaicMetrics &metrics, const MosaicBenchmarkContext &context,
                           RuntimeContext &runtime);
} // namespace mosaicraft::detail
