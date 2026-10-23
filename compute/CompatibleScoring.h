#pragma once
#include "CudaBackend.h"
#include "core/MatchingBatch.h"

namespace mosaicraft
{
namespace cuda
{
class CompatibleScoreWorkspace
{
  public:
    CompatibleScoreWorkspace();
    ~CompatibleScoreWorkspace();
    CompatibleScoreWorkspace(const CompatibleScoreWorkspace &) = delete;
    CompatibleScoreWorkspace &operator=(const CompatibleScoreWorkspace &) = delete;

    // 只分配，不提交上传或计算。设备预算仅为暂存；常驻图库另计并检查实际空闲显存。
    bool prepare(int maxTiles, int candidates, int libraryCount, size_t hostBudget,
                 size_t deviceBudget = 256ULL * 1024 * 1024,
                 CompatibleScoreFault fault = CompatibleScoreFault::None);
    int capacity() const;
    size_t hostBytes() const;
    size_t deviceBytes() const;
    size_t libraryBytes() const;
    const char *preparationFailure() const;
    void uploadLibrary(const std::vector<ImageRecord> &records, FeaturePack::Reader &reader,
                       GpuTimings *timings = nullptr);
    // MatchingBuffers 中仅使用固定维度特征、候选索引及权重，与图像宽高无关。
    void score(const MatchingBuffers &tiles, int count, double usePenalty, double *scores,
               GpuTimings *timings = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    const char *m_failure = "not prepared";
};
} // namespace cuda
} // namespace mosaicraft
