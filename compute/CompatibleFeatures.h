#pragma once

#include "CudaBackend.h"
#include "core/Database.h"
#include "core/WorkerPool.h"
#include <opencv2/core.hpp>
#include <array>

namespace mosaicraft
{
namespace cuda
{
inline bool hasTemplateFeatures(int width, int height)
{
    return (width == 180 && height == 320) || (width == 320 && height == 180) ||
           (width == 360 && height == 640) || (width == 640 && height == 360);
}

struct CompatibleFeature
{
    std::array<double, 3> lab{};
    std::array<float, 192> grid{};
    std::array<uint8_t, 256> tiny{};
    std::array<float, 256> lbp{};
    double edge = 0;
};

struct CompatibleTimings
{
    GpuTimings gpu;
    double prepareMs = 0, cpuMs = 0;
    size_t images = 0, batches = 0;
};

// 通用核只替换整数统计；颜色转换及舍入继续使用 CPU 的 v6 语义，评分独立选择后端。
class CompatibleFeatureWorkspace
{
  public:
    CompatibleFeatureWorkspace();
    ~CompatibleFeatureWorkspace();
    CompatibleFeatureWorkspace(const CompatibleFeatureWorkspace &) = delete;
    CompatibleFeatureWorkspace &operator=(const CompatibleFeatureWorkspace &) = delete;

    // 预算包含调用方的 BGR 和输出数组，以及可见的 OpenCV 临时图；超预算时缩小批次。
    // false 只表示计算前资源不足。运行中 CUDA 错误必须抛出，不能静默重算。
    bool prepare(int width, int height, int maxImages, size_t hostBudget, bool statistics = false,
                 size_t deviceBudget = 256ULL * 1024 * 1024, PipelineFault fault = PipelineFault::None);
    int capacity() const;
    size_t hostBytes() const;
    size_t deviceBytes() const;
    void extract(const std::vector<cv::Mat> &images, std::vector<CompatibleFeature> &features,
                 WorkerPool &workers, const std::vector<ImageRecord *> &statistics = {},
                 CompatibleTimings *timings = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace cuda
} // namespace mosaicraft
