#include "CudaBackend.h"

#include <cuda_runtime.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mosaicraft {
namespace cuda {

// ============================================================
// GPU kernel: 每个线程计算一个候选的加权距离
// ============================================================
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

    // --- Grid distance (mean of 16 cell LAB distances) ---
    double gridDist = 0.0;
    {
        const float* candGrid = d_candGrid + idx * 192;
        const float* tGrid = tileGrid;

        // 检查 grid 是否可用（至少第一个值非零或全零都算合法；
        // 非法数据由 host 端保证不传入，这里直接计算）
        double sum = 0.0;
        for (int i = 0; i < 16; ++i)
        {
            int off = i * 3;
            double dl = static_cast<double>(tGrid[off])     - static_cast<double>(candGrid[off]);
            double da = static_cast<double>(tGrid[off + 1]) - static_cast<double>(candGrid[off + 1]);
            double db = static_cast<double>(tGrid[off + 2]) - static_cast<double>(candGrid[off + 2]);
            sum += sqrt(dl * dl + da * da + db * db);
        }
        gridDist = sum / 16.0 / 100.0;
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
        for (int i = 0; i < 16; ++i)
        {
            int off = i * 3;
            double dl2 = static_cast<double>(tileGrid[off])     - static_cast<double>(candGrid[off]);
            double da2 = static_cast<double>(tileGrid[off + 1]) - static_cast<double>(candGrid[off + 1]);
            double db2 = static_cast<double>(tileGrid[off + 2]) - static_cast<double>(candGrid[off + 2]);
            sum += sqrt(dl2 * dl2 + da2 * da2 + db2 * db2);
        }
        gridDist = sum / 16.0 / 100.0;
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
    // 权重
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
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
        for (int i = 0; i < 16; ++i)
        {
            int off = i * 3;
            double dl2 = static_cast<double>(tGrid[off])     - static_cast<double>(cGrid[off]);
            double da2 = static_cast<double>(tGrid[off + 1]) - static_cast<double>(cGrid[off + 1]);
            double db2 = static_cast<double>(tGrid[off + 2]) - static_cast<double>(cGrid[off + 2]);
            sum += sqrt(dl2 * dl2 + da2 * da2 + db2 * db2);
        }
        gridDist = sum / 16.0 / 100.0;
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
    // alloc scores
    double* d_scores = nullptr;
    std::vector<double> scores;

    if (numCandidates <= 0)
    {
        return -1;
    }

#define CUDA_CHECK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__, \
                cudaGetErrorString(_e)); \
        goto cleanup2; \
    } \
} while(0)

    CUDA_CHECK(cudaMalloc(&d_scores, static_cast<std::size_t>(numCandidates) * sizeof(double)));

    // 启动 kernel：256 线程/块
    int blockSize = 256;
    int gridSize = (numCandidates + blockSize - 1) / blockSize;

    scoreKernel<<<gridSize, blockSize>>>(
        tL, tA, tB,
        tileGrid, tileTiny, tileEdge, tileLBP,
        d_candLAB, d_candGrid, d_candTiny, d_candEdge, d_candLBP,
        d_candUseCount, numCandidates,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty,
        d_scores);

    CUDA_CHECK(cudaDeviceSynchronize());
    CUDA_CHECK(cudaGetLastError());

    // CPU 侧找 argmin
    scores.resize(static_cast<std::size_t>(numCandidates));
    CUDA_CHECK(cudaMemcpy(scores.data(), d_scores,
               static_cast<std::size_t>(numCandidates) * sizeof(double),
               cudaMemcpyDeviceToHost));
    cudaFree(d_scores);

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

cleanup2:
    if (d_scores) cudaFree(d_scores);
#undef CUDA_CHECK
    return -1;
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

    double*  d_lab = nullptr;
    float*   d_grid = nullptr;
    std::uint8_t* d_tiny = nullptr;
    double*  d_edge = nullptr;
    float*   d_lbp = nullptr;
    int*     d_use = nullptr;

#define CUDA_CHECK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__, \
                cudaGetErrorString(_e)); \
        goto cleanup; \
    } \
} while(0)

    CUDA_CHECK(cudaMalloc(&d_lab,  N * 3 * sizeof(double)));
    CUDA_CHECK(cudaMalloc(&d_grid, N * 192 * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&d_tiny, N * 256));
    CUDA_CHECK(cudaMalloc(&d_edge, N * sizeof(double)));
    CUDA_CHECK(cudaMalloc(&d_lbp,  N * 256 * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&d_use,  N * sizeof(int)));

    CUDA_CHECK(cudaMemcpy(d_lab,  cd.lab,  N * 3 * sizeof(double), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_grid, cd.grid, N * 192 * sizeof(float), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_tiny, cd.tiny, N * 256, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_edge, cd.edge, N * sizeof(double), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_lbp,  cd.lbp,  N * 256 * sizeof(float), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_use,  cd.use,  N * sizeof(int), cudaMemcpyHostToDevice));

    // 上传 tile 特征到 GPU（kernel 只能访问 device memory）
    float*   d_tileGrid = nullptr;
    std::uint8_t* d_tileTiny = nullptr;
    float*   d_tileLBP = nullptr;

    CUDA_CHECK(cudaMalloc(&d_tileGrid, 192 * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&d_tileTiny, 256));
    CUDA_CHECK(cudaMalloc(&d_tileLBP,  256 * sizeof(float)));

    CUDA_CHECK(cudaMemcpy(d_tileGrid, tileGrid, 192 * sizeof(float), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_tileTiny, tileTiny, 256, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_tileLBP,  tileLBP,  256 * sizeof(float), cudaMemcpyHostToDevice));

    int bestIdx = findBestMatch(
        tL, tA, tB,
        d_tileGrid, d_tileTiny, tileEdge, d_tileLBP,
        d_lab, d_grid, d_tiny, d_edge, d_lbp, d_use, N,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty);

cleanup:
    if (d_tileGrid) cudaFree(d_tileGrid);
    if (d_tileTiny) cudaFree(d_tileTiny);
    if (d_tileLBP)  cudaFree(d_tileLBP);
    if (d_lab)  cudaFree(d_lab);
    if (d_grid) cudaFree(d_grid);
    if (d_tiny) cudaFree(d_tiny);
    if (d_edge) cudaFree(d_edge);
    if (d_lbp)  cudaFree(d_lbp);
    if (d_use)  cudaFree(d_use);
#undef CUDA_CHECK

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
    lib.count = N;
    cudaMalloc(&lib.d_lab,  N * 3 * sizeof(double));
    cudaMalloc(&lib.d_grid, N * 192 * sizeof(float));
    cudaMalloc(&lib.d_tiny, N * 256);
    cudaMalloc(&lib.d_edge, N * sizeof(double));
    cudaMalloc(&lib.d_lbp,  N * 256 * sizeof(float));
    cudaMalloc(&lib.d_use,  N * sizeof(int));
    cudaMemcpy(lib.d_lab,  h_lab,  N * 3 * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(lib.d_grid, h_grid, N * 192 * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(lib.d_tiny, h_tiny, N * 256, cudaMemcpyHostToDevice);
    cudaMemcpy(lib.d_edge, h_edge, N * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(lib.d_lbp,  h_lbp,  N * 256 * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(lib.d_use,  h_use,  N * sizeof(int), cudaMemcpyHostToDevice);
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
    if (N <= 0) return -1;

    // 上传 tile 特征
    float*   d_tileGrid = nullptr;
    std::uint8_t* d_tileTiny = nullptr;
    float*   d_tileLBP = nullptr;
    cudaMalloc(&d_tileGrid, 192 * sizeof(float));
    cudaMalloc(&d_tileTiny, 256);
    cudaMalloc(&d_tileLBP,  256 * sizeof(float));
    cudaMemcpy(d_tileGrid, tileGrid, 48 * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileTiny, tileTiny, 256, cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileLBP,  tileLBP,  256 * sizeof(float), cudaMemcpyHostToDevice);

    double* d_scores = nullptr;
    cudaMalloc(&d_scores, N * sizeof(double));

    int blockSize = 256;
    int gridSize = (N + blockSize - 1) / blockSize;
    scoreKernel<<<gridSize, blockSize>>>(
        tL, tA, tB,
        d_tileGrid, d_tileTiny, tileEdge, d_tileLBP,
        lib.d_lab, lib.d_grid, lib.d_tiny, lib.d_edge, lib.d_lbp, lib.d_use, N,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty,
        d_scores);

    cudaDeviceSynchronize();

    std::vector<double> scores(N);
    cudaMemcpy(scores.data(), d_scores, N * sizeof(double), cudaMemcpyDeviceToHost);

    cudaFree(d_scores);
    cudaFree(d_tileGrid); cudaFree(d_tileTiny); cudaFree(d_tileLBP);

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
    if (N <= 0 || lib.count <= 0) { return; }

    // 上传 tile 特征
    float *d_tileGrid=nullptr, *d_tileLBP=nullptr;
    uint8_t* d_tileTiny=nullptr;
    cudaMalloc(&d_tileGrid, 192*sizeof(float)); cudaMalloc(&d_tileTiny, 256);
    cudaMalloc(&d_tileLBP, 256*sizeof(float));
    cudaMemcpy(d_tileGrid, tileGrid, 192*sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileTiny, tileTiny, 256, cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileLBP, tileLBP, 256*sizeof(float), cudaMemcpyHostToDevice);

    // 上传候选索引
    int* d_indices = nullptr;
    cudaMalloc(&d_indices, N * sizeof(int));
    cudaMemcpy(d_indices, indices, N * sizeof(int), cudaMemcpyHostToDevice);

    // 仅分配 N 个评分的空间（而非 lib.count）
    double* d_scores = nullptr;
    cudaMalloc(&d_scores, N * sizeof(double));

    int blockSize = 256;
    int gridSize = (N + blockSize - 1) / blockSize;
    scoreIndexedKernel<<<gridSize, blockSize>>>(
        tL, tA, tB, d_tileGrid, d_tileTiny, tileEdge, d_tileLBP,
        lib.d_lab, lib.d_grid, lib.d_tiny, lib.d_edge, lib.d_lbp, lib.d_use,
        d_indices, N,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty, d_scores);

    cudaDeviceSynchronize();
    cudaMemcpy(outScores, d_scores, N * sizeof(double), cudaMemcpyDeviceToHost);

    cudaFree(d_scores); cudaFree(d_indices);
    cudaFree(d_tileGrid); cudaFree(d_tileTiny); cudaFree(d_tileLBP);
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
    if (N <= 0 || lib.count <= 0) { return -1; }

    float* d_tileGrid = nullptr; uint8_t* d_tileTiny = nullptr; float* d_tileLBP = nullptr;
    int* d_indices = nullptr;
    cudaMalloc(&d_tileGrid, 192*sizeof(float)); cudaMalloc(&d_tileTiny, 256);
    cudaMalloc(&d_tileLBP, 256*sizeof(float)); cudaMalloc(&d_indices, N*sizeof(int));
    cudaMemcpy(d_tileGrid, tileGrid, 192*sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileTiny, tileTiny, 256, cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileLBP, tileLBP, 256*sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_indices, indices, N*sizeof(int), cudaMemcpyHostToDevice);

    double* d_scores = nullptr;
    cudaMalloc(&d_scores, N*sizeof(double));

    int blockSize = 256;
    int gridSize = (N + blockSize - 1) / blockSize;
    // 使用索引化 kernel，仅对候选集评分，不遍历全库
    scoreIndexedKernel<<<gridSize, blockSize>>>(
        tL, tA, tB, d_tileGrid, d_tileTiny, tileEdge, d_tileLBP,
        lib.d_lab, lib.d_grid, lib.d_tiny, lib.d_edge, lib.d_lbp, lib.d_use,
        d_indices, N,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty, d_scores);

    cudaDeviceSynchronize();
    std::vector<double> scores(N);
    cudaMemcpy(scores.data(), d_scores, N*sizeof(double), cudaMemcpyDeviceToHost);

    cudaFree(d_scores); cudaFree(d_tileGrid); cudaFree(d_tileTiny);
    cudaFree(d_tileLBP); cudaFree(d_indices);

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
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty,
    double* outScores)            // [totalTiles * N]
{
    if (totalTiles <= 0 || N <= 0 || lib.count <= 0) { return; }
    int totalWork = totalTiles * N;

    // ——— 上传 tile 特征（仅一次） ———
    double*  d_tileL = nullptr; double* d_tileA = nullptr; double* d_tileB = nullptr;
    float*   d_tileGrid = nullptr; uint8_t* d_tileTiny = nullptr;
    double*  d_tileEdge = nullptr; float* d_tileLBP = nullptr;

    cudaMalloc(&d_tileL,    totalTiles * sizeof(double));
    cudaMalloc(&d_tileA,    totalTiles * sizeof(double));
    cudaMalloc(&d_tileB,    totalTiles * sizeof(double));
    cudaMalloc(&d_tileGrid, totalTiles * 192 * sizeof(float));
    cudaMalloc(&d_tileTiny, totalTiles * 256);
    cudaMalloc(&d_tileEdge, totalTiles * sizeof(double));
    cudaMalloc(&d_tileLBP,  totalTiles * 256 * sizeof(float));

    cudaMemcpy(d_tileL,    h_tileL,    totalTiles * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileA,    h_tileA,    totalTiles * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileB,    h_tileB,    totalTiles * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileGrid, h_tileGrid, totalTiles * 192 * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileTiny, h_tileTiny, totalTiles * 256, cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileEdge, h_tileEdge, totalTiles * sizeof(double), cudaMemcpyHostToDevice);
    cudaMemcpy(d_tileLBP,  h_tileLBP,  totalTiles * 256 * sizeof(float), cudaMemcpyHostToDevice);

    // ——— 上传候选索引 ———
    int* d_indices = nullptr;
    cudaMalloc(&d_indices, totalWork * sizeof(int));
    cudaMemcpy(d_indices, h_indices, totalWork * sizeof(int), cudaMemcpyHostToDevice);

    // ——— 评分输出 ———
    double* d_scores = nullptr;
    cudaMalloc(&d_scores, totalWork * sizeof(double));

    // ——— 启动 kernel（一次处理全部 tile） ———
    int blockSize = 256;
    int gridSize = (totalWork + blockSize - 1) / blockSize;
    scoreBatchKernel<<<gridSize, blockSize>>>(
        totalTiles,
        d_tileL, d_tileA, d_tileB,
        d_tileGrid, d_tileTiny, d_tileEdge, d_tileLBP,
        d_indices, N,
        lib.d_lab, lib.d_grid, lib.d_tiny, lib.d_edge, lib.d_lbp, lib.d_use,
        labW, gridW, tinyW, edgeW, lbpW, usePenalty,
        d_scores);

    cudaDeviceSynchronize();
    cudaMemcpy(outScores, d_scores, totalWork * sizeof(double), cudaMemcpyDeviceToHost);

    // ——— 清理 ———
    cudaFree(d_scores); cudaFree(d_indices);
    cudaFree(d_tileL); cudaFree(d_tileA); cudaFree(d_tileB);
    cudaFree(d_tileGrid); cudaFree(d_tileTiny);
    cudaFree(d_tileEdge); cudaFree(d_tileLBP);
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

} // namespace cuda
} // namespace mosaicraft
