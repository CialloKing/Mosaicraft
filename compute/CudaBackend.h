#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mosaicraft
{
namespace cuda
{

struct GpuTimings
{
    double allocationMs = 0, uploadMs = 0, computeMs = 0, downloadMs = 0;
    double cannyMs = 0, waitMs = 0;
    // Event 时间是设备时间；上面的同步路径计时保持原有主机墙钟口径。
    double eventUploadMs = 0, eventComputeMs = 0, eventDownloadMs = 0;
    std::size_t uploadBytes = 0, downloadBytes = 0, pinnedBytes = 0, residentBytes = 0, pixelBytes = 0;
    std::size_t peakTemporaryBytes = 0;
};

// 仅内部回归注入；生产配置不暴露故障或流水线开关。
enum class PipelineFault
{
    None, PinnedAllocation, DeviceAllocation, AfterUpload, CpuTask, BeforeWait
};

struct FeaturePipelineConfig
{
    int maxTiles = 256, width = 180, height = 320;
    bool downloadTiny = true, downloadLbp = true;
    std::size_t hostBudget = 256ULL * 1024 * 1024;
    std::size_t deviceBudget = 256ULL * 1024 * 1024;
    PipelineFault testFault = PipelineFault::None;
};

struct DeviceFeatureState;
struct DeviceFeatureView
{
    // 弱引用防止任务结束后的视图继续访问已释放显存；generation 防止跨批误用。
    std::weak_ptr<const DeviceFeatureState> state;
    std::uint64_t generation = 0;
    int first = 0, count = 0;
};

// 每次建库或生成任务独占，不跨线程共享。
class FeatureWorkspace
{
  public:
    struct Impl;
    std::unique_ptr<Impl> buffers;
    FeatureWorkspace();
    ~FeatureWorkspace();
    FeatureWorkspace(const FeatureWorkspace &) = delete;
    FeatureWorkspace &operator=(const FeatureWorkspace &) = delete;
    // 只有计算开始前的可选资源不足返回 false；运行期错误抛出并收束本任务 stream。
    bool preparePipeline(const FeaturePipelineConfig &config, GpuTimings *timings = nullptr);
    void resetPipeline();
    std::uint8_t *pipelineInput();
    std::size_t pinnedBytes() const;
    std::size_t residentBytes() const;
    void beginBatch(int count);
    void extract(int first, int count, double *lab, float *grid, std::uint8_t *tiny, double *edge, float *lbp,
                 GpuTimings *timings = nullptr);
    DeviceFeatureView deviceView(int first, int count) const;
};

// 生成任务跨主机批次复用评分显存，仍由 scoreBatch 检查可用显存和预算。
class ScoreWorkspace
{
  public:
    struct Impl;
    std::unique_ptr<Impl> buffers;
    ScoreWorkspace();
    ~ScoreWorkspace();
    ScoreWorkspace(const ScoreWorkspace &) = delete;
    ScoreWorkspace &operator=(const ScoreWorkspace &) = delete;
    bool preparePipeline(int maxTiles, int candidates, std::size_t hostBudget, std::size_t residentBytes,
                         std::size_t temporaryBudget = 256ULL * 1024 * 1024,
                         PipelineFault testFault = PipelineFault::None);
    void resetPipeline();
    std::size_t pinnedBytes() const;
};

struct CandidateData
{
    const double *lab;
    const float *grid;
    const std::uint8_t *tiny;
    const double *edge;
    const float *lbp;
    const int *use;
    int count;
};

struct GpuLibrary
{
    double *d_lab = nullptr;
    float *d_grid = nullptr;
    std::uint8_t *d_tiny = nullptr;
    double *d_edge = nullptr;
    float *d_lbp = nullptr;
    int *d_use = nullptr;
    std::uint8_t *d_tinyValid = nullptr;
    std::uint8_t *d_lbpValid = nullptr;
    int count = 0;
};

bool uploadLibrary(GpuLibrary &lib, const double *h_lab, const float *h_grid, const std::uint8_t *h_tiny,
                   const double *h_edge, const float *h_lbp, const int *h_use, int N, GpuTimings *timings = nullptr,
                   const std::uint8_t *tinyValid = nullptr, const std::uint8_t *lbpValid = nullptr);
void freeLibrary(GpuLibrary &lib);

int matchAgainstLibrary(double tL, double tA, double tB, const float *tileGrid, const std::uint8_t *tileTiny,
                        double tileEdge, const float *tileLBP, const GpuLibrary &lib, double labW, double gridW,
                        double tinyW, double edgeW, double lbpW, double usePenalty);

// 返回候选索引子集的全部评分（通过 outScores 输出参数）
void scoreIndices(double tL, double tA, double tB, const float *tileGrid, const std::uint8_t *tileTiny, double tileEdge,
                  const float *tileLBP, const GpuLibrary &lib, const int *indices, int N, double labW, double gridW,
                  double tinyW, double edgeW, double lbpW, double usePenalty, double *outScores);

int matchWithIndices(double tL, double tA, double tB, const float *tileGrid, const std::uint8_t *tileTiny,
                     double tileEdge, const float *tileLBP, const GpuLibrary &lib, const int *indices, int numIndices,
                     double labW, double gridW, double tinyW, double edgeW, double lbpW, double usePenalty);

// 批量评分：按显存预算分批处理全部候选；任何批次失败返回 false
bool scoreBatch(int totalTiles, const double *h_tileL, const double *h_tileA, const double *h_tileB,
                const float *h_tileGrid, const std::uint8_t *h_tileTiny, const double *h_tileEdge,
                const float *h_tileLBP, const int *h_indices, int N, const GpuLibrary &lib, const double *h_labW,
                const double *h_gridW, const double *h_tinyW, const double *h_edgeW, const double *h_lbpW,
                double usePenalty, double *outScores, GpuTimings *timings = nullptr,
                std::size_t temporaryBudget = 256ULL * 1024 * 1024, ScoreWorkspace *workspace = nullptr);

// Grid/Tiny/LBP 只引用当前批设备特征；弱引用和 generation 校验生命周期，不改变 kernel 算术。
bool scoreDeviceBatch(const DeviceFeatureView &features, const double *l, const double *a, const double *b,
                      const double *edge, const int *indices, int candidates, const GpuLibrary &library,
                      const double *labW, const double *gridW, const double *tinyW, const double *edgeW,
                      const double *lbpW, double usePenalty, double *scores, ScoreWorkspace &workspace,
                      GpuTimings *timings = nullptr, std::size_t temporaryBudget = 256ULL * 1024 * 1024);

int matchOnGpu(double tL, double tA, double tB, const float *tileGrid, const std::uint8_t *tileTiny, double tileEdge,
               const float *tileLBP, const CandidateData &candidates, double labW, double gridW, double tinyW,
               double edgeW, double lbpW, double usePenalty);

bool isCudaAvailable();

int extractTileFeatures(const std::uint8_t *h_tiles, int N, int imgW, int imgH, double *h_avgLAB, float *h_grid,
                        std::uint8_t *h_tiny, double *h_edge, float *h_lbp);
int extractTileFeatures(const std::uint8_t *h_tiles180, int N, double *h_avgLAB, float *h_grid, std::uint8_t *h_tiny,
                        double *h_edge, float *h_lbp);

// 原始特征提取（从 FeatureExtractorCuda，不写文件）
int extractFeaturesRaw(const std::uint8_t *h_images, int N, double *h_avgLAB, float *h_grid, std::uint8_t *h_tiny,
                       double *h_edge, float *h_lbp);

// 带特征尺寸的版本（支持 180x320 / 320x180 / 360x640 / 640x360）
int extractFeaturesRaw(const std::uint8_t *h_images, int N, int imgW, int imgH, double *h_avgLAB, float *h_grid,
                       std::uint8_t *h_tiny, double *h_edge, float *h_lbp, FeatureWorkspace *workspace = nullptr,
                       GpuTimings *timings = nullptr);

} // namespace cuda
} // namespace mosaicraft
