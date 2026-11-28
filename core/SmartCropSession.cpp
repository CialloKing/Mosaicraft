#include "SmartCropSession.h"
#include "RuntimeContext.h"
#if MOSAICRAFT_SMART_CROP_CUDA
#include "SmartCropCudaRecovery.h"
#endif

namespace mosaicraft
{
SmartCropSession::SmartCropSession(CropMode mode, CropDevice policy, RuntimeContext &runtime, bool standalone, std::optional<int> vulkan)
    : m_runtime(runtime), m_policy(policy), m_standalone(standalone), m_vulkan(vulkan),
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
    if (!m_vulkan && m_policy == CropDevice::Auto)
    {
        m_autoCuda = std::make_unique<SmartCropCudaRecovery>(m_models, m_runtime, standalone);
        m_initial = m_autoCuda->initialize() ? CropDevice::Cuda : CropDevice::Cpu;
    }
    if (!m_autoCuda || m_initial == CropDevice::Cpu)
#endif
    {
        useSession(m_initial);
    }
    m_primaryCuda = m_initial == CropDevice::Cuda;
    reportDevice(m_vulkan ? "vulkan" : cropDeviceName(m_initial));
}

SmartCropSession::~SmartCropSession() = default;

void SmartCropSession::useSession(CropDevice device)
{
    auto next = std::make_unique<SmartCropInference>(m_models, device, m_vulkan);
    next->probe();
    m_session = std::move(next);
}

void SmartCropSession::reportDevice(const char *device) const
{
    if (m_standalone)
    {
        m_runtime.computeIdentity(device, "fp32", m_vulkan ? "ncnn-vulkan" : "opencv-dnn");
    }
}

SmartCropAnalysis SmartCropSession::analyze(const cv::Mat &image)
{
#if MOSAICRAFT_SMART_CROP_CUDA
    if (m_autoCuda)
    {
        if (auto result = m_autoCuda->tryAnalyze(image))
        {
            m_primaryCuda = true;
            m_runtime.metric("smartCropCudaImages", static_cast<double>(++m_cudaImages));
            m_session.reset(); // 释放回退时 CPU 会话留下的输入缓冲区。
            reportDevice("cuda");
            return std::move(*result);
        }
        m_primaryCuda = false;
        reportDevice("cpu");
    }
#endif
    if (!m_session)
    {
        useSession(CropDevice::Cpu);
    }
    auto result = m_session->analyze(image);
    if (m_vulkan)
    {
        m_runtime.metric("smartCropVulkanImages", static_cast<double>(++m_vulkanImages));
    }
    else if (m_policy == CropDevice::Cuda)
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

ImageNormalizer SmartCropSession::secondCudaNormalizer(int width, int height)
{
    return ImageNormalizer(width, height, [this](const cv::Mat &image)
    {
        if (!m_secondCuda)
        {
            auto session = std::make_unique<SmartCropInference>(m_models, CropDevice::Cuda);
            session->probe();
            m_secondCuda = std::move(session);
        }
        auto result = m_secondCuda->analyze(image);
        ++m_secondCudaImages;
        return result;
    }, &m_secondCudaStats);
}

bool SmartCropSession::primaryUsesCuda() const noexcept
{
    return m_primaryCuda;
}

void SmartCropSession::releaseSecondCuda() noexcept
{
    m_secondCuda.reset();
}

void SmartCropSession::stopCudaForDeviceFault([[maybe_unused]] const SmartCropError &error)
{
#if MOSAICRAFT_SMART_CROP_CUDA
    if (m_autoCuda)
    {
        m_primaryCuda = false;
        m_autoCuda->stopForDeviceFault(error);
    }
#endif
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

void SmartCropSession::report(const SmartCropSession *secondary) const
{
    m_runtime.metric("smartCropCpuImages", static_cast<double>(m_cpuImages));
    const auto cudaImages = m_cudaImages + m_secondCudaImages;
    m_runtime.metric("smartCropCudaImages", static_cast<double>(cudaImages));
    m_runtime.metric("smartCropCudaLanes", m_secondCudaImages ? 2 : (cudaImages ? 1 : 0));
    m_runtime.metric("smartCropCudaSecondLaneImages", static_cast<double>(m_secondCudaImages));
    if (m_cpuImages && cudaImages)
    {
        reportDevice("mixed");
    }
    if (m_vulkan || secondary)
    {
        m_runtime.metric("smartCropVulkanImages", static_cast<double>(secondary ? secondary->m_vulkanImages : m_vulkanImages));
    }
    const SmartCropStats &other = secondary ? secondary->m_stats : m_secondCudaStats;
    if (secondary)
    {
        m_runtime.metric("smartCropCudaSaliencyMs", m_stats.saliencyMs);
        m_runtime.metric("smartCropCudaFaceMs", m_stats.faceMs);
        m_runtime.metric("smartCropVulkanSaliencyMs", other.saliencyMs);
        m_runtime.metric("smartCropVulkanFaceMs", other.faceMs);
    }
    m_runtime.metric("smartCropFacesFound", static_cast<double>(m_stats.facesFound + other.facesFound));
    m_runtime.metric("smartCropMoved", static_cast<double>(m_stats.moved + other.moved));
    m_runtime.metric("smartCropCentered", static_cast<double>(m_stats.centered + other.centered));
    m_runtime.metric("smartCropMainFaceClipped", static_cast<double>(m_stats.mainFaceClipped + other.mainFaceClipped));
    m_runtime.metric("smartCropInferenceWorkerMs", m_stats.inferenceMs + other.inferenceMs);
    m_runtime.metric("smartCropSaliencyMs", m_stats.saliencyMs + other.saliencyMs);
    m_runtime.metric("smartCropFaceMs", m_stats.faceMs + other.faceMs);
}
}
