#include "CudaBackend.h"

#include <cuda_runtime.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <vector>

namespace mosaicraft {
namespace cuda {

namespace {

template <typename T>
class DeviceBuffer
{
public:
    DeviceBuffer() = default;
    ~DeviceBuffer() { reset(); }

    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    cudaError_t allocate(std::size_t bytes)
    {
        reset();
        return cudaMalloc(reinterpret_cast<void**>(&m_ptr), bytes);
    }

    void reset()
    {
        if (m_ptr)
        {
            cudaFree(m_ptr);
            m_ptr = nullptr;
        }
    }

    T* get() const { return m_ptr; }

private:
    T* m_ptr = nullptr;
};

bool checkCuda(cudaError_t err, const char* file, int line)
{
    if (err == cudaSuccess)
    {
        return true;
    }
    fprintf(stderr, "CUDA error at %s:%d: %s\n", file, line, cudaGetErrorString(err));
    return false;
}

bool hasValidLibrary(const GpuLibrary& lib)
{
    return lib.count > 0 &&
        lib.d_lab && lib.d_grid && lib.d_tiny &&
        lib.d_edge && lib.d_lbp && lib.d_use;
}

void fillFailedScores(double* outScores, int count)
{
    if (!outScores || count <= 0)
    {
        return;
    }
    for (int i = 0; i < count; ++i)
    {
        outScores[i] = DBL_MAX;
    }
}

} // namespace

#define CUDA_OK(call) checkCuda((call), __FILE__, __LINE__)

// ============================================================
// GPU kernel: 每个线程计算一个候选的加权距离
// ============================================================
// 空间权重：来源于 15K tile 实测 Grid 贡献分析
// 中心 cell 匹配最稳定，底行最不可靠
// ============================================================
__device__ const double kGridWeight[64] = {
    0.85,0.92,0.96,0.99,1.00,0.99,0.94,0.89,
    0.96,1.02,1.06,1.11,1.11,1.10,1.05,0.98,
    0.97,1.03,1.07,1.10,1.11,1.09,1.05,0.98,
    0.96,1.02,1.06,1.09,1.09,1.07,1.02,0.96,
    0.97,1.03,1.08,1.13,1.13,1.10,1.05,0.98,
    0.98,1.05,1.10,1.14,1.14,1.10,1.06,0.98,
    0.97,1.02,1.06,1.10,1.11,1.06,1.02,0.94,
    0.46,0.47,0.48,0.48,0.48,0.47,0.47,0.46
};

__global__ void scoreKernel(
    // tile
    double tL, double tA, double tB,
    const float* __restrict__ tileGrid,
    const std::uint8_t* __restrict__ tileTiny,
    double tileEdge,
    const float* __restrict__ tileLBP,
    // candidates (device arrays)
    const double* __restrict__ d_candLAB,
    const float* __restrict__ d_candGrid,
    const std::uint8_t* __restrict__ d_candTiny,
    const double* __restrict__ d_candEdge,
    const float* __restrict__ d_candLBP,
    const int* __restrict__ d_candUseCount,
    int numCandidates,
    // weights
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty,
    // output
    double* __restrict__ d_scores)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numCandidates)
    {
        return;
    }

    // --- LAB distance ---
    double labDist = 0.0;
    {
        const double* lab = d_candLAB + idx * 3;
        double dl = tL - lab[0];
        double da = tA - lab[1];
        double db = tB - lab[2];
        labDist = sqrt(dl * dl + da * da + db * db) / 100.0;
    }

    // --- Grid distance (mean of 64 cell LAB distances) ---
    double gridDist = 0.0;
    {
        const float* candGrid = d_candGrid + idx * 192;
        const float* tGrid = tileGrid;

        // 检查 grid 是否可用（至少第一个值非零或全零都算合法；
        // 非法数据由 host 端保证不传入，这里直接计算）
        double sum = 0.0;
        for (int i = 0; i < 64; ++i)
        {
            int off = i * 3;
            double dl = static_cast<double>(tGrid[off])     - static_cast<double>(candGrid[off]);
            double da = static_cast<double>(tGrid[off + 1]) - static_cast<double>(candGrid[off + 1]);
            double db = static_cast<double>(tGrid[off + 2]) - static_cast<double>(candGrid[off + 2]);
            sum += sqrt(dl * dl + da * da + db * db) * kGridWeight[i];
        }
        gridDist = sum / 64.0 / 100.0;
    }

    // --- TinyImage MSE ---
    double tinyDist = 0.0;
    {
        const std::uint8_t* candTiny = d_candTiny + idx * 256;
        double sum = 0.0;
        for (int i = 0; i < 256; ++i)
        {
            double diff = static_cast<double>(tileTiny[i])
                        - static_cast<double>(candTiny[i]);
            sum += diff * diff;
        }
        // 归一化：MSE / (255^2)
        tinyDist = sum / (256.0 * 255.0 * 255.0);
    }

    // --- Edge distance ---
    double edgeDist = fabs(tileEdge - d_candEdge[idx]);

    // --- LBP distance (L1 / 2) ---
    double lbpDist = 0.0;
    {
        const float* candLBP = d_candLBP + idx * 256;
        double sum = 0.0;
        for (int i = 0; i < 256; ++i)
        {
            sum += fabs(static_cast<double>(tileLBP[i])
                      - static_cast<double>(candLBP[i]));
        }
        lbpDist = sum / 2.0;
    }

    // --- Weighted score ---
    double score = labW  * labDist
                 + gridW * gridDist
                 + tinyW * tinyDist
                 + edgeW * edgeDist
                 + lbpW  * lbpDist
                 + static_cast<double>(d_candUseCount[idx]) * usePenalty;

    d_scores[idx] = score;
}

// ============================================================
// GPU kernel: 按索引从常驻库中评分（仅 N 个候选，不评全库）
// ============================================================
__global__ void scoreIndexedKernel(
    // tile 特征
    double tL, double tA, double tB,
    const float* __restrict__ tileGrid,
    const std::uint8_t* __restrict__ tileTiny,
    double tileEdge,
    const float* __restrict__ tileLBP,
    // 常驻库数组（全量，按索引跳转访问）
    const double* __restrict__ libLAB,
    const float* __restrict__ libGrid,
    const std::uint8_t* __restrict__ libTiny,
    const double* __restrict__ libEdge,
    const float* __restrict__ libLBP,
    const int* __restrict__ libUseCount,
    // 候选索引
    const int* __restrict__ indices,
    int N,
    int libCount,
    // 权重
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty,
    // 输出：N 个评分
    double* __restrict__ d_scores)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= N)
    {
        return;
    }

    int libIdx = indices[idx];  // 映射到库中的实际偏移
    if (libIdx < 0 || libIdx >= libCount) { d_scores[idx] = 1e30; return; }

    // --- LAB distance ---
    double labDist = 0.0;
    {
        const double* lab = libLAB + libIdx * 3;
        double dl = tL - lab[0];
        double da = tA - lab[1];
        double db = tB - lab[2];
        labDist = sqrt(dl * dl + da * da + db * db) / 100.0;
    }

    // --- Grid distance ---
    double gridDist = 0.0;
    {
        const float* candGrid = libGrid + libIdx * 192;
        double sum = 0.0;
        for (int i = 0; i < 64; ++i)
        {
            int off = i * 3;
            double dl2 = static_cast<double>(tileGrid[off])     - static_cast<double>(candGrid[off]);
            double da2 = static_cast<double>(tileGrid[off + 1]) - static_cast<double>(candGrid[off + 1]);
            double db2 = static_cast<double>(tileGrid[off + 2]) - static_cast<double>(candGrid[off + 2]);
            sum += sqrt(dl2 * dl2 + da2 * da2 + db2 * db2) * kGridWeight[i];
        }
        gridDist = sum / 64.0 / 100.0;
    }

    // --- TinyImage MSE ---
    double tinyDist = 0.0;
    {
        const std::uint8_t* candTiny = libTiny + libIdx * 256;
        double sum = 0.0;
        for (int i = 0; i < 256; ++i)
        {
            double diff = static_cast<double>(tileTiny[i])
                        - static_cast<double>(candTiny[i]);
            sum += diff * diff;
        }
        tinyDist = sum / (256.0 * 255.0 * 255.0);
    }

    // --- Edge distance ---
    double edgeDist = fabs(tileEdge - libEdge[libIdx]);

    // --- LBP distance ---
    double lbpDist = 0.0;
    {
        const float* candLBP = libLBP + libIdx * 256;
        double sum = 0.0;
        for (int i = 0; i < 256; ++i)
        {
            sum += fabs(static_cast<double>(tileLBP[i])
                      - static_cast<double>(candLBP[i]));
        }
        lbpDist = sum / 2.0;
    }

    // --- Weighted score ---
    double score = labW  * labDist
                 + gridW * gridDist
                 + tinyW * tinyDist
                 + edgeW * edgeDist
                 + lbpW  * lbpDist
                 + static_cast<double>(libUseCount[libIdx]) * usePenalty;

    d_scores[idx] = score;
}

// ============================================================
// GPU kernel: 批量评分 — 一次 kernel 处理全部 tile × N 候选
// 消除逐 tile 启动 kernel 的巨大开销（38k 次 → 1 次）
// ============================================================
__global__ void scoreBatchKernel(
    int totalTiles,
    const double* __restrict__ tileL,     // [totalTiles]
    const double* __restrict__ tileA,
    const double* __restrict__ tileB,
    const float* __restrict__ tileGrid,   // [totalTiles * 192]
    const std::uint8_t* __restrict__ tileTiny, // [totalTiles * 256]
    const double* __restrict__ tileEdge,   // [totalTiles]
    const float* __restrict__ tileLBP,     // [totalTiles * 256]
    const int* __restrict__ indices,       // [totalTiles * N] 库索引，-1 表示无效
    int N,
    // 常驻库
    const double* __restrict__ libLAB,
    const float* __restrict__ libGrid,
    const std::uint8_t* __restrict__ libTiny,
    const double* __restrict__ libEdge,
    const float* __restrict__ libLBP,
    const int* __restrict__ libUseCount,
    // 权重（每 tile 独立，支持自适应）
    const double* __restrict__ tileLabW,   // [totalTiles]
    const double* __restrict__ tileGridW,
    const double* __restrict__ tileTinyW,
    const double* __restrict__ tileEdgeW,
    const double* __restrict__ tileLbpW,
    double usePenalty,
    // 输出：[totalTiles * N]
    double* __restrict__ d_scores)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    int totalWork = totalTiles * N;
    if (idx >= totalWork) { return; }

    int libIdx = indices[idx];
    if (libIdx < 0) { d_scores[idx] = 1e30; return; }
    int tileIdx = idx / N;

    // --- LAB distance ---
    double labDist = 0.0;
    {
        double tL = tileL[tileIdx];
        double tA = tileA[tileIdx];
        double tB = tileB[tileIdx];
        const double* lab = libLAB + libIdx * 3;
        double dl = tL - lab[0];
        double da = tA - lab[1];
        double db = tB - lab[2];
        labDist = sqrt(dl * dl + da * da + db * db) / 100.0;
    }

    // --- Grid distance ---
    double gridDist = 0.0;
    {
        const float* tGrid = tileGrid + tileIdx * 192;
        const float* cGrid = libGrid + libIdx * 192;
        double sum = 0.0;
        for (int i = 0; i < 64; ++i)
        {
            int off = i * 3;
            double dl2 = static_cast<double>(tGrid[off])     - static_cast<double>(cGrid[off]);
            double da2 = static_cast<double>(tGrid[off + 1]) - static_cast<double>(cGrid[off + 1]);
            double db2 = static_cast<double>(tGrid[off + 2]) - static_cast<double>(cGrid[off + 2]);
            sum += sqrt(dl2 * dl2 + da2 * da2 + db2 * db2) * kGridWeight[i];
        }
        gridDist = sum / 64.0 / 100.0;
    }

    // --- TinyImage MSE ---
    double tinyDist = 0.0;
    {
        const std::uint8_t* tTiny = tileTiny + tileIdx * 256;
        const std::uint8_t* cTiny = libTiny + libIdx * 256;
        double sum = 0.0;
        for (int i = 0; i < 256; ++i)
        {
            double diff = static_cast<double>(tTiny[i])
                        - static_cast<double>(cTiny[i]);
            sum += diff * diff;
        }
        tinyDist = sum / (256.0 * 255.0 * 255.0);
    }

    // --- Edge distance ---
    double edgeDist = fabs(tileEdge[tileIdx] - libEdge[libIdx]);

    // --- LBP distance ---
    double lbpDist = 0.0;
    {
        const float* tLBP = tileLBP + tileIdx * 256;
        const float* cLBP = libLBP + libIdx * 256;
        double sum = 0.0;
        for (int i = 0; i < 256; ++i)
        {
            sum += fabs(static_cast<double>(tLBP[i])
                      - static_cast<double>(cLBP[i]));
        }
        lbpDist = sum / 2.0;
    }

    // --- Weighted score（自适应权重）---
    double score = tileLabW[tileIdx]  * labDist
                 + tileGridW[tileIdx] * gridDist
                 + tileTinyW[tileIdx] * tinyDist
                 + tileEdgeW[tileIdx] * edgeDist
                 + tileLbpW[tileIdx]  * lbpDist
                 + static_cast<double>(libUseCount[libIdx]) * usePenalty;

    d_scores[idx] = score;
}

// ============================================================
// Host 接口
// ============================================================
int findBestMatch(
    double tL, double tA, double tB,
    const float* tileGrid,
    const std::uint8_t* tileTiny,
    double tileEdge,
    const float* tileLBP,
    const double* d_candLAB,
    const float* d_candGrid,
    const std::uint8_t* d_candTiny,
    const double* d_candEdge,
    const float* d_candLBP,
    const int* d_candUseCount,
    int numCandidates,
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty)
{
    if (numCandidates <= 0)
    {
        return -1;
    }

    DeviceBuffer<double> d_scores;
    if (!CUDA_OK(d_scores.allocate(static_cast<std::size_t>(numCandidates) * sizeof(double))))
        return -1;

    // 启动 kernel：256 线程/块
    int blockSize = 256;
    int gridSize = (numCandidates + blockSize - 1) / blockSize;

    scoreKernel<<<gridSize, blockSize>>>(
        tL, tA, tB,
        tileGrid, tileTiny, tileEdge, tileLBP,
        d_candLAB, d_candGrid, d_candTiny, d_candEdge, d_candLBP,
        d_candUseCount, numCandidates,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty,
        d_scores.get());

    if (!CUDA_OK(cudaGetLastError())) return -1;
    if (!CUDA_OK(cudaDeviceSynchronize())) return -1;

    // CPU 侧找 argmin
    std::vector<double> scores(static_cast<std::size_t>(numCandidates));
    if (!CUDA_OK(cudaMemcpy(scores.data(), d_scores.get(),
               static_cast<std::size_t>(numCandidates) * sizeof(double),
               cudaMemcpyDeviceToHost))) return -1;

    int bestIdx = 0;
    double bestScore = scores[0];
    for (int i = 1; i < numCandidates; ++i)
    {
        if (scores[i] < bestScore)
        {
            bestScore = scores[i];
            bestIdx = i;
        }
    }

    return bestIdx;
}

// ============================================================
// 高级封装：管理 GPU 内存生命周期
// ============================================================
int matchOnGpu(
    double tL, double tA, double tB,
    const float* tileGrid,
    const std::uint8_t* tileTiny,
    double tileEdge,
    const float* tileLBP,
    const CandidateData& cd,
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty)
{
    int N = cd.count;
    if (N <= 0) { return -1; }

    DeviceBuffer<double> d_lab;
    DeviceBuffer<float> d_grid;
    DeviceBuffer<std::uint8_t> d_tiny;
    DeviceBuffer<double> d_edge;
    DeviceBuffer<float> d_lbp;
    DeviceBuffer<int> d_use;

    if (!CUDA_OK(d_lab.allocate(N * 3 * sizeof(double)))) return -1;
    if (!CUDA_OK(d_grid.allocate(N * 192 * sizeof(float)))) return -1;
    if (!CUDA_OK(d_tiny.allocate(N * 256))) return -1;
    if (!CUDA_OK(d_edge.allocate(N * sizeof(double)))) return -1;
    if (!CUDA_OK(d_lbp.allocate(N * 256 * sizeof(float)))) return -1;
    if (!CUDA_OK(d_use.allocate(N * sizeof(int)))) return -1;

    if (!CUDA_OK(cudaMemcpy(d_lab.get(), cd.lab, N * 3 * sizeof(double), cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_grid.get(), cd.grid, N * 192 * sizeof(float), cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_tiny.get(), cd.tiny, N * 256, cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_edge.get(), cd.edge, N * sizeof(double), cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_lbp.get(), cd.lbp, N * 256 * sizeof(float), cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_use.get(), cd.use, N * sizeof(int), cudaMemcpyHostToDevice))) return -1;

    // 上传 tile 特征到 GPU（kernel 只能访问 device memory）
    DeviceBuffer<float> d_tileGrid;
    DeviceBuffer<std::uint8_t> d_tileTiny;
    DeviceBuffer<float> d_tileLBP;

    if (!CUDA_OK(d_tileGrid.allocate(192 * sizeof(float)))) return -1;
    if (!CUDA_OK(d_tileTiny.allocate(256))) return -1;
    if (!CUDA_OK(d_tileLBP.allocate(256 * sizeof(float)))) return -1;

    if (!CUDA_OK(cudaMemcpy(d_tileGrid.get(), tileGrid, 192 * sizeof(float), cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_tileTiny.get(), tileTiny, 256, cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_tileLBP.get(), tileLBP, 256 * sizeof(float), cudaMemcpyHostToDevice))) return -1;

    int bestIdx = findBestMatch(
        tL, tA, tB,
        d_tileGrid.get(), d_tileTiny.get(), tileEdge, d_tileLBP.get(),
        d_lab.get(), d_grid.get(), d_tiny.get(), d_edge.get(), d_lbp.get(), d_use.get(), N,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty);

    return bestIdx;
}

// ============================================================
// GPU 常驻图库
// ============================================================
bool uploadLibrary(GpuLibrary& lib,
                   const double* h_lab, const float* h_grid,
                   const std::uint8_t* h_tiny, const double* h_edge,
                   const float* h_lbp, const int* h_use, int N)
{
    if (N <= 0) return false;
    lib.count = 0;  // 先清零，失败时 freeLibrary 不会释放垃圾指针

    #define CUDA_ALLOC(p, size) \
        if (cudaMalloc((void**)&(p), size) != cudaSuccess) { freeLibrary(lib); return false; }
    #define CUDA_UPLOAD(p, h, size) \
        if (cudaMemcpy(p, h, size, cudaMemcpyHostToDevice) != cudaSuccess) { freeLibrary(lib); return false; }

    CUDA_ALLOC(lib.d_lab,  N * 3 * sizeof(double));
    CUDA_ALLOC(lib.d_grid, N * 192 * sizeof(float));
    CUDA_ALLOC(lib.d_tiny, N * 256);
    CUDA_ALLOC(lib.d_edge, N * sizeof(double));
    CUDA_ALLOC(lib.d_lbp,  N * 256 * sizeof(float));
    CUDA_ALLOC(lib.d_use,  N * sizeof(int));
    CUDA_UPLOAD(lib.d_lab,  h_lab,  N * 3 * sizeof(double));
    CUDA_UPLOAD(lib.d_grid, h_grid, N * 192 * sizeof(float));
    CUDA_UPLOAD(lib.d_tiny, h_tiny, N * 256);
    CUDA_UPLOAD(lib.d_edge, h_edge, N * sizeof(double));
    CUDA_UPLOAD(lib.d_lbp,  h_lbp,  N * 256 * sizeof(float));
    CUDA_UPLOAD(lib.d_use,  h_use,  N * sizeof(int));
    lib.count = N;
    return true;
}

void freeLibrary(GpuLibrary& lib)
{
    if (lib.d_lab)  { cudaFree(lib.d_lab);  lib.d_lab  = nullptr; }
    if (lib.d_grid) { cudaFree(lib.d_grid); lib.d_grid = nullptr; }
    if (lib.d_tiny) { cudaFree(lib.d_tiny); lib.d_tiny = nullptr; }
    if (lib.d_edge) { cudaFree(lib.d_edge); lib.d_edge = nullptr; }
    if (lib.d_lbp)  { cudaFree(lib.d_lbp);  lib.d_lbp  = nullptr; }
    if (lib.d_use)  { cudaFree(lib.d_use);  lib.d_use  = nullptr; }
    lib.count = 0;
}

int matchAgainstLibrary(
    double tL, double tA, double tB,
    const float* tileGrid, const std::uint8_t* tileTiny,
    double tileEdge, const float* tileLBP,
    const GpuLibrary& lib,
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty)
{
    int N = lib.count;
    if (!hasValidLibrary(lib)) return -1;

    // 上传 tile 特征
    DeviceBuffer<float> d_tileGrid;
    DeviceBuffer<std::uint8_t> d_tileTiny;
    DeviceBuffer<float> d_tileLBP;
    if (!CUDA_OK(d_tileGrid.allocate(192 * sizeof(float)))) return -1;
    if (!CUDA_OK(d_tileTiny.allocate(256))) return -1;
    if (!CUDA_OK(d_tileLBP.allocate(256 * sizeof(float)))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_tileGrid.get(), tileGrid, 192 * sizeof(float), cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_tileTiny.get(), tileTiny, 256, cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_tileLBP.get(), tileLBP, 256 * sizeof(float), cudaMemcpyHostToDevice))) return -1;

    DeviceBuffer<double> d_scores;
    if (!CUDA_OK(d_scores.allocate(static_cast<std::size_t>(N) * sizeof(double)))) return -1;

    int blockSize = 256;
    int gridSize = (N + blockSize - 1) / blockSize;
    scoreKernel<<<gridSize, blockSize>>>(
        tL, tA, tB,
        d_tileGrid.get(), d_tileTiny.get(), tileEdge, d_tileLBP.get(),
        lib.d_lab, lib.d_grid, lib.d_tiny, lib.d_edge, lib.d_lbp, lib.d_use, N,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty,
        d_scores.get());

    if (!CUDA_OK(cudaGetLastError())) return -1;
    if (!CUDA_OK(cudaDeviceSynchronize())) return -1;
    std::vector<double> scores(N);
    if (!CUDA_OK(cudaMemcpy(scores.data(), d_scores.get(), static_cast<std::size_t>(N) * sizeof(double), cudaMemcpyDeviceToHost))) return -1;

    int bestIdx = 0;
    for (int i = 1; i < N; ++i)
        if (scores[i] < scores[bestIdx]) bestIdx = i;
    return bestIdx;
}

void scoreIndices(
    double tL, double tA, double tB,
    const float* tileGrid, const std::uint8_t* tileTiny,
    double tileEdge, const float* tileLBP,
    const GpuLibrary& lib,
    const int* indices, int N,
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty,
    double* outScores)
{
    // 仅对 N 个候选评分，而非全库
    if (N <= 0) { return; }
    if (!outScores || !indices) { return; }
    fillFailedScores(outScores, N);
    if (!hasValidLibrary(lib)) { return; }

    // 上传 tile 特征
    DeviceBuffer<float> d_tileGrid;
    DeviceBuffer<std::uint8_t> d_tileTiny;
    DeviceBuffer<float> d_tileLBP;
    if (!CUDA_OK(d_tileGrid.allocate(192 * sizeof(float)))) return;
    if (!CUDA_OK(d_tileTiny.allocate(256))) return;
    if (!CUDA_OK(d_tileLBP.allocate(256 * sizeof(float)))) return;
    if (!CUDA_OK(cudaMemcpy(d_tileGrid.get(), tileGrid, 192 * sizeof(float), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_tileTiny.get(), tileTiny, 256, cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_tileLBP.get(), tileLBP, 256 * sizeof(float), cudaMemcpyHostToDevice))) return;

    // 上传候选索引
    DeviceBuffer<int> d_indices;
    if (!CUDA_OK(d_indices.allocate(static_cast<std::size_t>(N) * sizeof(int)))) return;
    if (!CUDA_OK(cudaMemcpy(d_indices.get(), indices, static_cast<std::size_t>(N) * sizeof(int), cudaMemcpyHostToDevice))) return;

    // 仅分配 N 个评分的空间（而非 lib.count）
    DeviceBuffer<double> d_scores;
    if (!CUDA_OK(d_scores.allocate(static_cast<std::size_t>(N) * sizeof(double)))) return;

    int blockSize = 256;
    int gridSize = (N + blockSize - 1) / blockSize;
    scoreIndexedKernel<<<gridSize, blockSize>>>(
        tL, tA, tB, d_tileGrid.get(), d_tileTiny.get(), tileEdge, d_tileLBP.get(),
        lib.d_lab, lib.d_grid, lib.d_tiny, lib.d_edge, lib.d_lbp, lib.d_use,
        d_indices.get(), N,
        lib.count,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty, d_scores.get());

    if (!CUDA_OK(cudaGetLastError())) return;
    if (!CUDA_OK(cudaDeviceSynchronize())) return;
    CUDA_OK(cudaMemcpy(outScores, d_scores.get(), static_cast<std::size_t>(N) * sizeof(double), cudaMemcpyDeviceToHost));
}

int matchWithIndices(
    double tL, double tA, double tB,
    const float* tileGrid, const std::uint8_t* tileTiny,
    double tileEdge, const float* tileLBP,
    const GpuLibrary& lib,
    const int* indices, int N,
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty)
{
    // 仅对 N 个候选评分
    if (N <= 0 || !indices || !hasValidLibrary(lib)) { return -1; }

    DeviceBuffer<float> d_tileGrid;
    DeviceBuffer<std::uint8_t> d_tileTiny;
    DeviceBuffer<float> d_tileLBP;
    DeviceBuffer<int> d_indices;
    if (!CUDA_OK(d_tileGrid.allocate(192 * sizeof(float)))) return -1;
    if (!CUDA_OK(d_tileTiny.allocate(256))) return -1;
    if (!CUDA_OK(d_tileLBP.allocate(256 * sizeof(float)))) return -1;
    if (!CUDA_OK(d_indices.allocate(static_cast<std::size_t>(N) * sizeof(int)))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_tileGrid.get(), tileGrid, 192 * sizeof(float), cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_tileTiny.get(), tileTiny, 256, cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_tileLBP.get(), tileLBP, 256 * sizeof(float), cudaMemcpyHostToDevice))) return -1;
    if (!CUDA_OK(cudaMemcpy(d_indices.get(), indices, static_cast<std::size_t>(N) * sizeof(int), cudaMemcpyHostToDevice))) return -1;

    DeviceBuffer<double> d_scores;
    if (!CUDA_OK(d_scores.allocate(static_cast<std::size_t>(N) * sizeof(double)))) return -1;

    int blockSize = 256;
    int gridSize = (N + blockSize - 1) / blockSize;
    // 使用索引化 kernel，仅对候选集评分，不遍历全库
    scoreIndexedKernel<<<gridSize, blockSize>>>(
        tL, tA, tB, d_tileGrid.get(), d_tileTiny.get(), tileEdge, d_tileLBP.get(),
        lib.d_lab, lib.d_grid, lib.d_tiny, lib.d_edge, lib.d_lbp, lib.d_use,
        d_indices.get(), N,
        lib.count,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty, d_scores.get());

    if (!CUDA_OK(cudaGetLastError())) return -1;
    if (!CUDA_OK(cudaDeviceSynchronize())) return -1;
    std::vector<double> scores(N);
    if (!CUDA_OK(cudaMemcpy(scores.data(), d_scores.get(), static_cast<std::size_t>(N) * sizeof(double), cudaMemcpyDeviceToHost))) return -1;

    // 在候选子集中找最优
    int bestSub = 0;
    for (int i = 1; i < N; ++i)
    {
        if (scores[i] < scores[bestSub]) { bestSub = i; }
    }
    return indices[bestSub];
}

// ============================================================
// 批量评分：一次 kernel 处理全部 tile，消除逐 tile 启动开销
// ============================================================
void scoreBatch(
    int totalTiles,
    const double* h_tileL, const double* h_tileA, const double* h_tileB,
    const float* h_tileGrid,     // [totalTiles * 192]，已扁平化
    const std::uint8_t* h_tileTiny, // [totalTiles * 256]
    const double* h_tileEdge,     // [totalTiles]
    const float* h_tileLBP,       // [totalTiles * 256]
    const int* h_indices,         // [totalTiles * N]，-1 表示无效候选
    int N,
    const GpuLibrary& lib,
    const double* h_labW, const double* h_gridW,
    const double* h_tinyW, const double* h_edgeW, const double* h_lbpW,
    double usePenalty,
    double* outScores)            // [totalTiles * N]
{
    if (totalTiles <= 0 || N <= 0) { return; }
    if (totalTiles > std::numeric_limits<int>::max() / N) { return; }
    if (!outScores || !h_indices) { return; }
    int totalWork = totalTiles * N;
    fillFailedScores(outScores, totalWork);
    if (!hasValidLibrary(lib)) { return; }

    // ——— 上传 tile 特征（仅一次） ———
    DeviceBuffer<double> d_tileL;
    DeviceBuffer<double> d_tileA;
    DeviceBuffer<double> d_tileB;
    DeviceBuffer<float> d_tileGrid;
    DeviceBuffer<std::uint8_t> d_tileTiny;
    DeviceBuffer<double> d_tileEdge;
    DeviceBuffer<float> d_tileLBP;

    if (!CUDA_OK(d_tileL.allocate(static_cast<std::size_t>(totalTiles) * sizeof(double)))) return;
    if (!CUDA_OK(d_tileA.allocate(static_cast<std::size_t>(totalTiles) * sizeof(double)))) return;
    if (!CUDA_OK(d_tileB.allocate(static_cast<std::size_t>(totalTiles) * sizeof(double)))) return;
    if (!CUDA_OK(d_tileGrid.allocate(static_cast<std::size_t>(totalTiles) * 192 * sizeof(float)))) return;
    if (!CUDA_OK(d_tileTiny.allocate(static_cast<std::size_t>(totalTiles) * 256))) return;
    if (!CUDA_OK(d_tileEdge.allocate(static_cast<std::size_t>(totalTiles) * sizeof(double)))) return;
    if (!CUDA_OK(d_tileLBP.allocate(static_cast<std::size_t>(totalTiles) * 256 * sizeof(float)))) return;

    if (!CUDA_OK(cudaMemcpy(d_tileL.get(), h_tileL, static_cast<std::size_t>(totalTiles) * sizeof(double), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_tileA.get(), h_tileA, static_cast<std::size_t>(totalTiles) * sizeof(double), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_tileB.get(), h_tileB, static_cast<std::size_t>(totalTiles) * sizeof(double), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_tileGrid.get(), h_tileGrid, static_cast<std::size_t>(totalTiles) * 192 * sizeof(float), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_tileTiny.get(), h_tileTiny, static_cast<std::size_t>(totalTiles) * 256, cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_tileEdge.get(), h_tileEdge, static_cast<std::size_t>(totalTiles) * sizeof(double), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_tileLBP.get(), h_tileLBP, static_cast<std::size_t>(totalTiles) * 256 * sizeof(float), cudaMemcpyHostToDevice))) return;

    // ——— 上传自适应权重（每 tile 一套） ———
    DeviceBuffer<double> d_labW;
    DeviceBuffer<double> d_gridW;
    DeviceBuffer<double> d_tinyW;
    DeviceBuffer<double> d_edgeW;
    DeviceBuffer<double> d_lbpW;
    if (!CUDA_OK(d_labW.allocate(static_cast<std::size_t>(totalTiles) * sizeof(double)))) return;
    if (!CUDA_OK(d_gridW.allocate(static_cast<std::size_t>(totalTiles) * sizeof(double)))) return;
    if (!CUDA_OK(d_tinyW.allocate(static_cast<std::size_t>(totalTiles) * sizeof(double)))) return;
    if (!CUDA_OK(d_edgeW.allocate(static_cast<std::size_t>(totalTiles) * sizeof(double)))) return;
    if (!CUDA_OK(d_lbpW.allocate(static_cast<std::size_t>(totalTiles) * sizeof(double)))) return;
    if (!CUDA_OK(cudaMemcpy(d_labW.get(), h_labW, static_cast<std::size_t>(totalTiles) * sizeof(double), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_gridW.get(), h_gridW, static_cast<std::size_t>(totalTiles) * sizeof(double), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_tinyW.get(), h_tinyW, static_cast<std::size_t>(totalTiles) * sizeof(double), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_edgeW.get(), h_edgeW, static_cast<std::size_t>(totalTiles) * sizeof(double), cudaMemcpyHostToDevice))) return;
    if (!CUDA_OK(cudaMemcpy(d_lbpW.get(), h_lbpW, static_cast<std::size_t>(totalTiles) * sizeof(double), cudaMemcpyHostToDevice))) return;

    // ——— 上传候选索引 ———
    DeviceBuffer<int> d_indices;
    if (!CUDA_OK(d_indices.allocate(static_cast<std::size_t>(totalWork) * sizeof(int)))) return;
    if (!CUDA_OK(cudaMemcpy(d_indices.get(), h_indices, static_cast<std::size_t>(totalWork) * sizeof(int), cudaMemcpyHostToDevice))) return;

    // ——— 评分输出 ———
    DeviceBuffer<double> d_scores;
    if (!CUDA_OK(d_scores.allocate(static_cast<std::size_t>(totalWork) * sizeof(double)))) return;

    // ——— 启动 kernel（一次处理全部 tile） ———
    int blockSize = 256;
    int gridSize = (totalWork + blockSize - 1) / blockSize;
    scoreBatchKernel<<<gridSize, blockSize>>>(
        totalTiles,
        d_tileL.get(), d_tileA.get(), d_tileB.get(),
        d_tileGrid.get(), d_tileTiny.get(), d_tileEdge.get(), d_tileLBP.get(),
        d_indices.get(), N,
        lib.d_lab, lib.d_grid, lib.d_tiny, lib.d_edge, lib.d_lbp, lib.d_use,
        d_labW.get(), d_gridW.get(), d_tinyW.get(), d_edgeW.get(), d_lbpW.get(), usePenalty,
        d_scores.get());

    if (!CUDA_OK(cudaGetLastError())) return;
    if (!CUDA_OK(cudaDeviceSynchronize())) return;
    CUDA_OK(cudaMemcpy(outScores, d_scores.get(), static_cast<std::size_t>(totalWork) * sizeof(double), cudaMemcpyDeviceToHost));
}

// ============================================================
// 运行时检测 CUDA 可用性
// ============================================================
bool isCudaAvailable()
{
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    return (err == cudaSuccess && count > 0);
}

// ============================================================
// 从 tile ROI 批量提取特征（CPU resize → GPU kernel）
// 委托给 FeatureExtractorCuda::extractFeaturesRaw
// ============================================================
extern int extractTileFeatures(
    const uint8_t* h_tiles, int N, int imgW, int imgH,
    double* h_avgLAB, float* h_grid, uint8_t* h_tiny,
    double* h_edge, float* h_lbp)
{
    return mosaicraft::cuda::extractFeaturesRaw(
        h_tiles, N, imgW, imgH, h_avgLAB, h_grid, h_tiny, h_edge, h_lbp);
}

extern int extractTileFeatures(
    const uint8_t* h_tiles, int N,
    double* h_avgLAB, float* h_grid, uint8_t* h_tiny,
    double* h_edge, float* h_lbp)
{
    return mosaicraft::cuda::extractFeaturesRaw(
        h_tiles, N, h_avgLAB, h_grid, h_tiny, h_edge, h_lbp);
}

} // namespace cuda
} // namespace mosaicraft
