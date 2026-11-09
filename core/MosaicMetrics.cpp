#include "MosaicMetrics.h"
#include <iomanip>
#include <iostream>

namespace mosaicraft::detail
{
void printMosaicBenchmark(const MosaicMetrics &metrics, const MosaicBenchmarkContext &context, const char *label)
{
    const auto &cfg = context.config;
    const auto &backends = context.backends;
    const bool templateCuda = backends.scoring == ScoreBackend::TemplateCuda;
    std::cout << "\n=== Benchmark " << label << " ===\n";
    std::cout << "  Total tiles:     " << context.totalTiles << "\n";
    std::cout << "  Candidates/tile: " << context.candidates << "\n";
    if (metrics.msPrep > 0)
    {
        std::cout << "  Prep (DB+GPU): " << std::fixed << std::setprecision(1) << metrics.msPrep << " ms\n";
    }
    std::cout << "  Features:    " << std::fixed << std::setprecision(1) << metrics.msFeat << " ms\n";
    if (metrics.opResizeNs > 0)
    {
        auto toMs = [](int64_t ns)
        {
            return ns / 1000000.0;
        };
        std::cout << "    Resize:   " << std::setprecision(1) << toMs(metrics.opResizeNs) << " ms\n";
        std::cout << "    LAB:      " << std::setprecision(1) << toMs(metrics.opLabNs) << " ms\n";
        std::cout << "    Grid:     " << std::setprecision(1) << toMs(metrics.opGridNs) << " ms\n";
        std::cout << "    Tiny:     " << std::setprecision(1) << toMs(metrics.opTinyNs) << " ms\n";
        std::cout << "    Edge:     " << std::setprecision(1) << toMs(metrics.opEdgeNs) << " ms\n";
        std::cout << "    LBP:      " << std::setprecision(1) << toMs(metrics.opLbpNs) << " ms\n";
    }
    if (metrics.msANNBuild > 0)
    {
        std::cout << "  ANN build:   " << metrics.msANNBuild << " ms\n";
    }
    std::cout << "  ANN load:    " << metrics.msANNLoad << " ms\n";
    std::cout << "  ANN query:   " << metrics.msANNQuery << " ms\n";
    if (metrics.msGPUScore > 0)
    {
        std::cout << "  GPU scoring: " << metrics.msGPUScore << " ms\n";
        std::cout << "  GPU speed:   " << std::fixed << std::setprecision(0)
                  << (context.totalTiles * static_cast<double>(context.candidates) / metrics.msGPUScore * 1000.0)
                  << " scores/s\n";
    }
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  CPU scoring: " << metrics.msCPUScore << " ms\n";
    std::cout << "  Selection:   " << (metrics.msSelect - metrics.msCPUScore) << " ms\n";
    std::cout << "  CPU feature loading: " << metrics.msCPUFeatures << " ms\n";
    std::cout << "  Neighbor penalties: " << metrics.msNeighbor << " ms\n";
    std::cout << "  Candidate sorting: " << metrics.msSort << " ms\n";
    std::cout << "  Analysis: " << metrics.msAnalysis << " ms\n";
    std::cout << "  Output loading (worker sum): " << metrics.outputStats.loadingMs << " ms\n";
    std::cout << "  Overlay sampling (worker sum): " << metrics.outputStats.overlaySamplingMs << " ms\n"
              << "  Overlay blending (worker sum): " << metrics.outputStats.overlayBlendMs << " ms\n"
              << "  Overlay reference bytes: " << metrics.outputStats.overlayReferenceBytes << "\n"
              << "  Overlay worker budget bytes: " << metrics.outputStats.overlayWorkingBytes << "\n";
    std::cout << "  Color conversion: " << metrics.outputStats.colorMs << " ms\n";
    std::cout << "  Row encoding: " << metrics.outputStats.codecMs << " ms\n";
    if (cfg.generatePreview)
    {
        std::cout << "  Preview (included in encoding): " << metrics.outputStats.previewMs << " ms\n";
    }
    std::cout << "  Producer wait: " << metrics.outputStats.producerWaitMs << " ms\n";
    std::cout << "  Consumer wait: " << metrics.outputStats.consumerWaitMs << " ms\n";
    std::cout << "  Image cache hits: " << metrics.outputStats.cacheHits << "\n"
              << "  Image decodes: " << metrics.outputStats.cacheDecodes << "\n"
              << "  Image cache peak bytes: " << metrics.outputStats.cachePeakBytes << "\n"
              << "  Image shared loads: " << metrics.outputStats.cacheSharedLoads << "\n";
    std::cout << "  Placement:   " << metrics.msPlace << " ms\n";
    std::cout << "  Encoding:    " << metrics.msEncode << " ms\n";
    if (metrics.opPlaceDecodeNs > 0)
    {
        auto toMs = [](int64_t ns)
        {
            return ns / 1000000.0;
        };
        std::cout << "    Decode:   " << std::setprecision(1) << toMs(metrics.opPlaceDecodeNs) << " ms\n";
        std::cout << "    Resize:   " << std::setprecision(1) << toMs(metrics.opPlaceResizeNs) << " ms\n";
        std::cout << "    Copy:     " << std::setprecision(1) << toMs(metrics.opPlaceCopyNs) << " ms\n";
    }
    if (templateCuda)
    {
        std::cout << "  Library GPU allocation: " << metrics.libraryTimings.allocationMs << " ms\n"
                  << "  Library GPU upload: " << metrics.libraryTimings.uploadMs << " ms\n"
                  << "  Library device bytes: " << metrics.libraryTimings.residentBytes << "\n"
                  << "  Feature GPU allocation: " << metrics.featureTimings.allocationMs << " ms\n"
                  << "  Feature GPU upload: " << metrics.featureTimings.uploadMs << " ms\n"
                  << "  Feature GPU compute: " << metrics.featureTimings.computeMs << " ms\n"
                  << "  Feature GPU download: " << metrics.featureTimings.downloadMs << " ms\n"
                  << "  Feature CPU Canny: " << metrics.featureTimings.cannyMs << " ms\n"
                  << "  Feature GPU wait: " << metrics.featureTimings.waitMs << " ms\n"
                  << "  Feature CUDA event upload: " << metrics.featureTimings.eventUploadMs << " ms\n"
                  << "  Feature CUDA event compute: " << metrics.featureTimings.eventComputeMs << " ms\n"
                  << "  Feature CUDA event download: " << metrics.featureTimings.eventDownloadMs << " ms\n"
                  << "  Feature upload bytes: " << metrics.featureTimings.uploadBytes << "\n"
                  << "  Feature download bytes: " << metrics.featureTimings.downloadBytes << "\n"
                  << "  Feature pinned bytes: " << metrics.featureTimings.pinnedBytes << "\n"
                  << "  Feature resident bytes: " << metrics.featureTimings.residentBytes << "\n"
                  << "  Feature device pixel bytes: " << metrics.featureTimings.pixelBytes << "\n"
                  << "  Score GPU allocation: " << metrics.scoreTimings.allocationMs << " ms\n"
                  << "  Score GPU upload: " << metrics.scoreTimings.uploadMs << " ms\n"
                  << "  Score GPU compute: " << metrics.scoreTimings.computeMs << " ms\n"
                  << "  Score GPU download: " << metrics.scoreTimings.downloadMs << " ms\n"
                  << "  Score GPU wait: " << metrics.scoreTimings.waitMs << " ms\n"
                  << "  Score CUDA event upload: " << metrics.scoreTimings.eventUploadMs << " ms\n"
                  << "  Score CUDA event compute: " << metrics.scoreTimings.eventComputeMs << " ms\n"
                  << "  Score CUDA event download: " << metrics.scoreTimings.eventDownloadMs << " ms\n"
                  << "  Score upload bytes: " << metrics.scoreTimings.uploadBytes << "\n"
                  << "  Score download bytes: " << metrics.scoreTimings.downloadBytes << "\n"
                  << "  Score pinned bytes: " << metrics.scoreTimings.pinnedBytes << "\n"
                  << "  Score temporary bytes: " << metrics.scoreTimings.peakTemporaryBytes << "\n";
    }
    if (backends.scoring == ScoreBackend::CompatibleCuda)
    {
        std::cout << "  Library GPU upload: " << metrics.libraryTimings.uploadMs << " ms\n"
                  << "  Library device bytes: " << metrics.libraryTimings.residentBytes << "\n"
                  << "  Score GPU allocation: " << metrics.scoreTimings.allocationMs << " ms\n"
                  << "  Score packing and copy: " << metrics.scoreTimings.uploadMs + metrics.scoreTimings.downloadMs
                  << " ms\n"
                  << "  Score CUDA event upload: " << metrics.scoreTimings.eventUploadMs << " ms\n"
                  << "  Score CUDA event compute: " << metrics.scoreTimings.eventComputeMs << " ms\n"
                  << "  Score CUDA event download: " << metrics.scoreTimings.eventDownloadMs << " ms\n"
                  << "  Score pinned bytes: " << metrics.scoreTimings.pinnedBytes << "\n"
                  << "  Score temporary bytes: " << metrics.scoreTimings.peakTemporaryBytes << "\n"
                  << "  Matching combined device temporary bytes: " << context.compatibleTemporaryBytes << "\n";
    }
    std::cout << "  Matching batches: " << metrics.matchingBatches << "\n"
              << "  Matching prefetch: " << metrics.matchingPrefetch << "\n"
              << "  Matching wall: " << metrics.matchingWallMs << " ms\n"
              << "  Matching max tiles: " << metrics.maximumBatchTiles << "\n"
              << "  Matching buffer peak bytes: " << metrics.matchingPeakBytes << "\n"
              << "  Feature cache peak bytes: " << metrics.featureReadStats.peakBytes << "\n"
              << "  Feature cache hits: " << metrics.featureReadStats.hits << "\n"
              << "  Feature source reads: " << metrics.featureReadStats.sourceReads << "\n"
              << "  Feature cache evictions: " << metrics.featureReadStats.evictions << "\n"
              << "  Feature cache preloaded: " << metrics.featureReadStats.preloaded << "\n";
    std::cout << "  Library feature source reads: " << metrics.libraryReadStats.sourceReads << "\n"
              << "  Library feature pack hits: " << metrics.libraryReadStats.packedRows << "\n";
    std::cout << "  === Total: " << context.totalMs << " ms ===\n";
    if (context.totalTiles > 0)
    {
        std::cout << "  Avg/tile:    " << (context.totalMs / context.totalTiles) << " ms\n";
    }
    std::cout << std::flush;
}
} // namespace mosaicraft::detail
