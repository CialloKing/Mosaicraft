#include "MosaicMetrics.h"
#include "RuntimeContext.h"
#include <iomanip>
#include <iostream>

namespace mosaicraft::detail
{
namespace
{
void metric(RuntimeContext &runtime, const std::string &key, const char *label, double value, const char *unit = " ms")
{
    runtime.metric(key, value);
    std::cout << "  " << label << ": " << value << unit << '\n';
}

void gpuBenchmark(RuntimeContext &runtime, const char *role, const cuda::GpuTimings &t)
{
    // 同步路径未采集设备事件；只发布已有观测，不把未采集的字段写成零。
    const auto emit = [&](const char *key, const char *label, double value, const char *unit = " ms")
    {
        if (value > 0)
        {
            const std::string name = std::string(role) + " " + label;
            metric(runtime, std::string(role) + key, name.c_str(), value, unit);
        }
    };
    emit("GpuAllocationMs", "GPU allocation", t.allocationMs);
    emit("GpuUploadMs", "GPU upload", t.uploadMs);
    emit("GpuComputeMs", "GPU compute", t.computeMs);
    emit("GpuDownloadMs", "GPU download", t.downloadMs);
    emit("GpuWaitMs", "GPU host wait", t.waitMs);
    emit("CpuCannyMs", "CPU Canny", t.cannyMs);
    emit("CudaEventUploadMs", "CUDA event upload", t.eventUploadMs);
    emit("CudaEventComputeMs", "CUDA event compute", t.eventComputeMs);
    emit("CudaEventDownloadMs", "CUDA event download", t.eventDownloadMs);
    emit("GpuUploadBytes", "upload bytes", t.uploadBytes, "");
    emit("GpuDownloadBytes", "download bytes", t.downloadBytes, "");
    emit("GpuPinnedBytes", "pinned bytes", t.pinnedBytes, "");
    emit("GpuResidentBytes", "resident bytes", t.residentBytes, "");
    emit("GpuPixelBytes", "device pixel bytes", t.pixelBytes, "");
    emit("GpuPeakTemporaryBytes", "peak temporary bytes", t.peakTemporaryBytes, "");
}
} // namespace

void reportOutputBenchmark(const OutputStats &stats, const MosaicEngine::Config &config,
                           RuntimeContext &runtime, bool legacyRenderKeys)
{
    if (!stats.workerSlots)
    {
        return; // HTML uses the project writer, not this image/tile pipeline.
    }
    std::cout << "  Output benchmark:\n" << std::fixed << std::setprecision(1);
    metric(runtime, "outputWorkerSlots", "Output worker slots", stats.workerSlots, "");
    metric(runtime, "placementMs", "Placement", stats.placementMs);
    if (config.outputMode == OutputMode::Image)
    {
        if (legacyRenderKeys || stats.decodeMs > 0)
        {
            metric(runtime, "decodeWorkerMs", "Decode (worker sum)", stats.decodeMs);
        }
        if (legacyRenderKeys || stats.resizeMs > 0)
        {
            metric(runtime, "resizeWorkerMs", "Resize (worker sum)", stats.resizeMs);
        }
        metric(runtime, "encodingMs", "Encoding", stats.encodingMs);
    }
    if (stats.rowCodec)
    {
        metric(runtime, "codecWriteMs", "Row encoding", stats.codecMs);
    }
    else if (legacyRenderKeys && config.outputMode == OutputMode::Image)
    {
        runtime.metric("codecWriteMs", stats.codecMs);
    }
    if (stats.rowColorConversion)
    {
        metric(runtime, "outputColorMs", "Output color conversion", stats.colorMs);
    }
    if (stats.parallelStream)
    {
        metric(runtime, "outputProducerWaitMs", "Producer wait (free stripe)", stats.producerWaitMs);
        metric(runtime, "outputConsumerWaitMs", "Consumer wait (ready stripe)", stats.consumerWaitMs);
    }
    if (!stats.previewPath.empty())
    {
        metric(runtime, "outputPreviewMs", "Preview (included in encoding)", stats.previewMs);
    }
    if (stats.overlayReferenceBytes)
    {
        metric(runtime, "overlaySamplingWorkerMs", "Overlay sampling (worker sum)", stats.overlaySamplingMs);
        metric(runtime, "overlayBlendWorkerMs", "Overlay blending (worker sum)", stats.overlayBlendMs);
        metric(runtime, "overlayReferenceBytes", "Overlay reference bytes", stats.overlayReferenceBytes, "");
        metric(runtime, "overlayWorkingBudgetBytes", "Overlay worker budget bytes", stats.overlayWorkingBytes, "");
    }
    metric(runtime, "outputLoadingWorkerMs", "Output loading (worker sum)", stats.loadingMs);
    metric(runtime, "cacheHits", "Image cache hits", stats.cacheHits, "");
    metric(runtime, "cacheDecodes", "Image decodes", stats.cacheDecodes, "");
    metric(runtime, "cachePeakBytes", "Image cache peak bytes", stats.cachePeakBytes, "");
    metric(runtime, "cacheSharedLoads", "Image shared loads", stats.cacheSharedLoads, "");
}

void reportMosaicBenchmark(const MosaicMetrics &metrics, const MosaicBenchmarkContext &context,
                           RuntimeContext &runtime)
{
    std::cout << "\n=== Benchmark single ===\n";
    std::cout << "  Total tiles:     " << context.totalTiles << "\n";
    std::cout << "  Candidates/tile: " << context.candidates << "\n";
    if (metrics.msPrep > 0)
    {
        std::cout << "  Prep (DB+GPU): " << std::fixed << std::setprecision(1) << metrics.msPrep << " ms\n";
    }
    std::cout << "  Features:    " << std::fixed << std::setprecision(1) << metrics.msFeat << " ms\n";
    if (metrics.opResizeNs > 0)
    {
        std::cout << "  CPU feature worker times (overlapping, not wall time):\n";
        metric(runtime, "cpuFeatureResizeWorkerMs", "CPU feature resize", metrics.opResizeNs / 1e6);
        metric(runtime, "cpuLabConvertWorkerMs", "BGR to Lab", metrics.opLabNs / 1e6);
        metric(runtime, "cpuGridMeanWorkerMs", "Grid + mean", metrics.opGridNs / 1e6);
        if (metrics.opTinyNs > 0)
        {
            metric(runtime, "cpuTinyWorkerMs", "Tiny", metrics.opTinyNs / 1e6);
        }
        if (metrics.opEdgeNs > 0)
        {
            metric(runtime, "cpuEdgeWorkerMs", "Edge", metrics.opEdgeNs / 1e6);
        }
        if (metrics.opLbpNs > 0)
        {
            metric(runtime, "cpuLbpWorkerMs", "LBP", metrics.opLbpNs / 1e6);
        }
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
    reportOutputBenchmark(metrics.outputStats, context.config, runtime);
    gpuBenchmark(runtime, "library", metrics.libraryTimings);
    gpuBenchmark(runtime, "feature", metrics.featureTimings);
    gpuBenchmark(runtime, "score", metrics.scoreTimings);
    if (context.compatibleTemporaryBytes > 0)
    {
        std::cout << "  Matching combined device temporary bytes: " << context.compatibleTemporaryBytes << '\n';
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
              << "  Feature cache reused: " << metrics.featureReadStats.sessionHit << "\n";
    std::cout << "  Library feature source reads: " << metrics.libraryReadStats.sourceReads << "\n";
    std::cout << "  === Total: " << context.totalMs << " ms ===\n";
    if (context.totalTiles > 0)
    {
        std::cout << "  Avg/tile:    " << (context.totalMs / context.totalTiles) << " ms\n";
    }
    std::cout << std::flush;
}
} // namespace mosaicraft::detail
