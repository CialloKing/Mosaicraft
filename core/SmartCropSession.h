#pragma once

#include "ImageNormalizer.h"
#include "SmartCrop.h"

namespace mosaicraft
{
class RuntimeContext;
class SmartCropCudaRecovery;

// 同一批次串行推理，复用模型和检测器缓冲区。
class SmartCropSession
{
public:
    SmartCropSession(CropMode mode, CropDevice policy, RuntimeContext &runtime, bool standalone);
    ~SmartCropSession();
    ImageNormalizer normalizer(int width, int height);
    CropDevice initialDevice() const;
    void finish();
    void report() const;

private:
    SmartCropAnalysis analyze(const cv::Mat &image);
    void useSession(CropDevice device);
    void reportDevice(const char *device) const;

    RuntimeContext &m_runtime;
    CropDevice m_policy;
    bool m_standalone;
    SmartCropModelPair m_models{};
    std::unique_ptr<SmartCropInference> m_session;
#if MOSAICRAFT_SMART_CROP_CUDA
    std::unique_ptr<SmartCropCudaRecovery> m_autoCuda;
#endif
    CropDevice m_initial;
    SmartCropStats m_stats;
    size_t m_cpuImages = 0, m_cudaImages = 0;
};
}
