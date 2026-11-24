#include "SmartCropCudaRecovery.h"
#include "RuntimeContext.h"
#include <array>

namespace mosaicraft
{
SmartCropCudaRecovery::SmartCropCudaRecovery(const SmartCropModelPair &models, RuntimeContext &runtime)
    : SmartCropCudaRecovery([&]()
      {
          auto session = std::make_shared<std::unique_ptr<SmartCropInference>>();
          return Operations{
              [session, &models]()
              {
                  requireSmartCropDevice(CropDevice::Cuda);
                  *session = std::make_unique<SmartCropInference>(models, CropDevice::Cuda);
                  (*session)->probe();
              },
              [session](const cv::Mat &image)
              {
                  return (*session)->analyze(image);
              },
              [session]() { session->reset(); }};
      }(), runtime, Clock::now)
{
}

SmartCropCudaRecovery::SmartCropCudaRecovery(Operations operations, RuntimeContext &runtime,
                                             std::function<Clock::time_point()> now)
    : m_operations(std::move(operations)), m_runtime(runtime), m_now(std::move(now))
{
}

SmartCropCudaRecovery::~SmartCropCudaRecovery()
{
    if (m_probe.valid())
    {
        m_worker.wait(); // The packaged task owns any probe exception.
    }
    m_worker.run(1, [&](size_t) { m_operations.release(); });
}

bool SmartCropCudaRecovery::initialize()
{
    startProbe();
    finishProbe(); // Choose the initial backend before opening the output directory.
    return m_gpuReady;
}

void SmartCropCudaRecovery::finish()
{
    if (m_probe.valid())
    {
        // A model contract error must still surface if the final CPU image finished first.
        finishProbe();
    }
}

void SmartCropCudaRecovery::startProbe()
{
    const auto task = std::make_shared<std::packaged_task<void()>>([&]()
    {
        try
        {
            m_operations.probe();
        }
        catch (...)
        {
            m_operations.release();
            throw;
        }
    });
    auto pending = task->get_future();
    m_worker.start(1, [task](size_t) { (*task)(); });
    m_probe = std::move(pending);
}

void SmartCropCudaRecovery::finishProbe()
{
    m_worker.wait();
    try
    {
        m_probe.get();
        m_gpuReady = true;
    }
    catch (const SmartCropError &error)
    {
        fallback(error);
    }
}

void SmartCropCudaRecovery::fallback(const SmartCropError &error)
{
    if (error.cudaFault() == SmartCropCudaFault::None)
    {
        throw error;
    }
    static constexpr std::array delays{5, 10, 20, 40, 60};
    m_gpuReady = false;
    m_canRetry = error.cudaFault() != SmartCropCudaFault::RestartRequired;
    m_retryAt = m_now() + std::chrono::seconds(delays[m_backoff]);
    m_backoff = std::min<unsigned>(m_backoff + 1, delays.size() - 1);
    if (!m_fallingBack)
    {
        m_fallingBack = true;
        m_runtime.metric("smartCropCudaFallbacks", static_cast<double>(++m_fallbacks));
        m_runtime.emit("warning", "smart_crop_device_fallback", error.what(), "cropDevice");
    }
    else if (!m_canRetry)
    {
        m_runtime.emit("warning", "smart_crop_cuda_restart_required", error.what(), "cropDevice");
    }
    m_runtime.backends({}, {}, error.what());
}

std::optional<SmartCropAnalysis> SmartCropCudaRecovery::tryAnalyze(const cv::Mat &image)
{
    if (m_probe.valid() && m_probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        finishProbe();
    }
    if (!m_gpuReady)
    {
        if (m_canRetry && !m_probe.valid() && m_now() >= m_retryAt)
        {
            startProbe(); // Do not make the CPU image wait for the recovery probe.
        }
        return std::nullopt;
    }
    SmartCropAnalysis result;
    try
    {
        m_worker.run(1, [&](size_t)
        {
            try
            {
                result = m_operations.analyze(image);
            }
            catch (...)
            {
                m_operations.release();
                throw;
            }
        });
    }
    catch (const SmartCropError &error)
    {
        fallback(error);
        return std::nullopt;
    }
    if (m_fallingBack)
    {
        m_fallingBack = false;
        m_backoff = 0;
        m_runtime.metric("smartCropCudaRecoveries", static_cast<double>(++m_recoveries));
        m_runtime.emit("info", "smart_crop_device_recovered", "CUDA smart crop inference recovered", "cropDevice");
    }
    return result;
}
}
