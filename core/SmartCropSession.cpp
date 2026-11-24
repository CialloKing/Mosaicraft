#include "SmartCropSession.h"
#include "RuntimeContext.h"
#if MOSAICRAFT_SMART_CROP_CUDA
#include "SmartCropCudaRecovery.h"
#endif

namespace mosaicraft
{
SmartCropSession::SmartCropSession(CropMode mode, CropDevice policy, RuntimeContext &runtime, bool standalone)
    : m_runtime(runtime), m_policy(policy), m_standalone(standalone),
      m_initial(policy == CropDevice::Cuda ? CropDevice::Cuda : CropDevice::Cpu)
{
    m_runtime.metric("smartCropCudaFallbacks", 0);
    m_runtime.metric("smartCropCudaRecoveries", 0);
    m_runtime.stage("loading_crop_models");
    if (m_policy == CropDevice::Cuda)
    {
        requireSmartCropDevice(CropDevice::Cuda);
    }
    m_models = loadSmartCropModelPair(mode);
#if MOSAICRAFT_SMART_CROP_CUDA
    if (m_policy == CropDevice::Auto)
    {
        m_autoCuda = std::make_unique<SmartCropCudaRecovery>(m_models, m_runtime, standalone);
        m_initial = m_autoCuda->initialize() ? CropDevice::Cuda : CropDevice::Cpu;
    }
    if (!m_autoCuda || m_initial == CropDevice::Cpu)
#endif
    {
        useSession(m_initial);
    }
    reportDevice(cropDeviceName(m_initial));
}

SmartCropSession::~SmartCropSession() = default;

void SmartCropSession::useSession(CropDevice device)
{
    auto next = std::make_unique<SmartCropInference>(m_models, device);
    next->probe();
    m_session = std::move(next);
}

void SmartCropSession::reportDevice(const char *device) const
{
    if (m_standalone)
    {
        m_runtime.computeIdentity(device, "fp32", "opencv-dnn");
    }
}

SmartCropAnalysis SmartCropSession::analyze(const cv::Mat &image)
{
#if MOSAICRAFT_SMART_CROP_CUDA
    if (m_autoCuda)
    {
        if (auto result = m_autoCuda->tryAnalyze(image))
        {
            m_runtime.metric("smartCropCudaImages", static_cast<double>(++m_cudaImages));
            m_session.reset(); // 释放回退时 CPU 会话留下的输入缓冲区。
            reportDevice("cuda");
            return std::move(*result);
        }
        reportDevice("cpu");
    }
#endif
    if (!m_session)
    {
        useSession(CropDevice::Cpu);
    }
    auto result = m_session->analyze(image);
    if (m_policy == CropDevice::Cuda)
    {
        m_runtime.metric("smartCropCudaImages", static_cast<double>(++m_cudaImages));
    }
    else
    {
        m_runtime.metric("smartCropCpuImages", static_cast<double>(++m_cpuImages));
    }
    return result;
}

ImageNormalizer SmartCropSession::normalizer(int width, int height)
{
    return ImageNormalizer(width, height, [this](const cv::Mat &image)
    {
        return analyze(image);
    }, &m_stats);
}

CropDevice SmartCropSession::initialDevice() const
{
    return m_initial;
}

void SmartCropSession::finish()
{
#if MOSAICRAFT_SMART_CROP_CUDA
    if (m_autoCuda)
    {
        m_autoCuda->finish();
    }
#endif
}

void SmartCropSession::report() const
{
    m_runtime.metric("smartCropCpuImages", static_cast<double>(m_cpuImages));
    m_runtime.metric("smartCropCudaImages", static_cast<double>(m_cudaImages));
    if (m_cpuImages && m_cudaImages)
    {
        reportDevice("mixed");
    }
    m_runtime.metric("smartCropFacesFound", static_cast<double>(m_stats.facesFound));
    m_runtime.metric("smartCropMoved", static_cast<double>(m_stats.moved));
    m_runtime.metric("smartCropCentered", static_cast<double>(m_stats.centered));
    m_runtime.metric("smartCropMainFaceClipped", static_cast<double>(m_stats.mainFaceClipped));
    m_runtime.metric("smartCropInferenceWorkerMs", m_stats.inferenceMs);
    m_runtime.metric("smartCropSaliencyMs", m_stats.saliencyMs);
    m_runtime.metric("smartCropFaceMs", m_stats.faceMs);
}
}
