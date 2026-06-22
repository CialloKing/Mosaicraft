#pragma once

#include <cstdint>

namespace mosaicraft {
namespace cuda {

struct CandidateData {
    const double*  lab;
    const float*   grid;
    const std::uint8_t* tiny;
    const double*  edge;
    const float*   lbp;
    const int*     use;
    int count;
};

struct GpuLibrary {
    double*  d_lab = nullptr;
    float*   d_grid = nullptr;
    std::uint8_t* d_tiny = nullptr;
    double*  d_edge = nullptr;
    float*   d_lbp = nullptr;
    int*     d_use = nullptr;
    int count = 0;
};

bool uploadLibrary(GpuLibrary& lib,
                   const double* h_lab, const float* h_grid,
                   const std::uint8_t* h_tiny, const double* h_edge,
                   const float* h_lbp, const int* h_use, int N);
void freeLibrary(GpuLibrary& lib);

int matchAgainstLibrary(
    double tL, double tA, double tB,
    const float* tileGrid, const std::uint8_t* tileTiny,
    double tileEdge, const float* tileLBP,
    const GpuLibrary& lib,
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty);

// 返回候选索引子集的全部评分（通过 outScores 输出参数）
void scoreIndices(
    double tL, double tA, double tB,
    const float* tileGrid, const std::uint8_t* tileTiny,
    double tileEdge, const float* tileLBP,
    const GpuLibrary& lib,
    const int* indices, int N,
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty,
    double* outScores);

int matchWithIndices(
    double tL, double tA, double tB,
    const float* tileGrid, const std::uint8_t* tileTiny,
    double tileEdge, const float* tileLBP,
    const GpuLibrary& lib,
    const int* indices, int numIndices,
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty);

// 批量评分：totalTiles 个 tile 的全部候选一次性 GPU 评分
void scoreBatch(
    int totalTiles,
    const double* h_tileL, const double* h_tileA, const double* h_tileB,
    const float* h_tileGrid, const std::uint8_t* h_tileTiny,
    const double* h_tileEdge, const float* h_tileLBP,
    const int* h_indices, int N,
    const GpuLibrary& lib,
    const double* h_labW, const double* h_gridW,
    const double* h_tinyW, const double* h_edgeW, const double* h_lbpW,
    double usePenalty,
    double* outScores);

int matchOnGpu(
    double tL, double tA, double tB,
    const float* tileGrid, const std::uint8_t* tileTiny,
    double tileEdge, const float* tileLBP,
    const CandidateData& candidates,
    double labW, double gridW, double tinyW, double edgeW, double lbpW,
    double usePenalty);

bool isCudaAvailable();

int extractTileFeatures(
    const std::uint8_t* h_tiles180, int N,
    double* h_avgLAB, float* h_grid, std::uint8_t* h_tiny,
    double* h_edge, float* h_lbp);

// 原始特征提取（从 FeatureExtractorCuda，不写文件）
int extractFeaturesRaw(
    const std::uint8_t* h_images, int N,
    double* h_avgLAB, float* h_grid, std::uint8_t* h_tiny,
    double* h_edge, float* h_lbp);

} // namespace cuda
} // namespace mosaicraft
