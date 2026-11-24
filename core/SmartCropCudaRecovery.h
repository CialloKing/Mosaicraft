#pragma once

#include "SmartCrop.h"
#include "WorkerPool.h"
#include <chrono>
#include <future>

namespace mosaicraft
{
class RuntimeContext;

// The same worker owns every GPU operation, including releasing its DNN handles.
class SmartCropCudaRecovery
{
public:
    using Clock = std::chrono::steady_clock;
    struct Operations
    {
        std::function<void()> probe;
        std::function<SmartCropAnalysis(const cv::Mat &)> analyze;
        std::function<void()> release;
    };

    SmartCropCudaRecovery(const SmartCropModelPair &models, RuntimeContext &runtime, bool reportFallback);
    SmartCropCudaRecovery(Operations operations, RuntimeContext &runtime,
                          std::function<Clock::time_point()> now, bool reportFallback = true);
    ~SmartCropCudaRecovery();
    bool initialize();
    void finish();
    std::optional<SmartCropAnalysis> tryAnalyze(const cv::Mat &image);

private:
    void startProbe();
    void finishProbe();
    void fallback(const SmartCropError &error);

    Operations m_operations;
    RuntimeContext &m_runtime;
    std::function<Clock::time_point()> m_now;
    bool m_reportFallback;
    WorkerPool m_worker{1};
    std::future<void> m_probe;
    Clock::time_point m_retryAt{};
    unsigned m_backoff = 0;
    size_t m_fallbacks = 0, m_recoveries = 0;
    bool m_gpuReady = false, m_fallingBack = false, m_canRetry = true;
};
}
