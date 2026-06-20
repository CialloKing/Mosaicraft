#include "FeatureExtractorCuda.h"

#include <cuda_runtime.h>
#include <opencv2/imgproc.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace mosaicraft {
namespace cuda {

// ============================================================
// GPU 常量：归一化图尺寸
// ============================================================
static constexpr int IMG_W = 320;
static constexpr int IMG_H = 180;
static constexpr int IMG_PIX = IMG_W * IMG_H;        // 57600
static constexpr int GRID_CW = IMG_W / 8;             // 40
static constexpr int GRID_CH = IMG_H / 8;             // 22.5→22
static constexpr int GRID_CELLS = 64;
static constexpr int TINY_W = 16;
static constexpr int TINY_H = 16;
static constexpr int LBP_BINS = 256;

// ============================================================
// 工具：RGB→LAB（GPU device function）
// ============================================================
__device__ void rgb2lab(float r, float g, float b, float& l, float& a, float& bb)
{
    // sRGB → XYZ
    float varR = r / 255.0f, varG = g / 255.0f, varB = b / 255.0f;
    varR = (varR > 0.04045f) ? powf((varR + 0.055f) / 1.055f, 2.4f) : varR / 12.92f;
    varG = (varG > 0.04045f) ? powf((varG + 0.055f) / 1.055f, 2.4f) : varG / 12.92f;
    varB = (varB > 0.04045f) ? powf((varB + 0.055f) / 1.055f, 2.4f) : varB / 12.92f;
    varR *= 100.0f; varG *= 100.0f; varB *= 100.0f;

    float x = varR * 0.4124f + varG * 0.3576f + varB * 0.1805f;
    float y = varR * 0.2126f + varG * 0.7152f + varB * 0.0722f;
    float z = varR * 0.0193f + varG * 0.1192f + varB * 0.9505f;

    float refX = 95.047f, refY = 100.0f, refZ = 108.883f;
    float vx = x / refX, vy = y / refY, vz = z / refZ;
    vx = (vx > 0.008856f) ? powf(vx, 1.0f/3.0f) : (7.787f * vx + 16.0f/116.0f);
    vy = (vy > 0.008856f) ? powf(vy, 1.0f/3.0f) : (7.787f * vy + 16.0f/116.0f);
    vz = (vz > 0.008856f) ? powf(vz, 1.0f/3.0f) : (7.787f * vz + 16.0f/116.0f);

    l = (116.0f * vy - 16.0f) * 2.55f;    // OpenCV scale
    a = 500.0f * (vx - vy) + 128.0f;
    bb = 200.0f * (vy - vz) + 128.0f;
}

__device__ float rgb2gray(float r, float g, float b)
{
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

// ============================================================
// 主 kernel：每个 block 处理一张图片，计算全部特征
// blockDim = (IMG_W, 1, 1) = 320 threads
// 用网格跨行覆盖 IMG_H=180 行
// ============================================================
extern "C" __global__ void featureKernel(
    const uint8_t* __restrict__ d_images,   // [batchSize * IMG_H * IMG_W * 3]
    float* __restrict__ d_grid,             // [batchSize * 192]
    uint8_t* __restrict__ d_tiny,           // [batchSize * 256]
    float* __restrict__ d_lbp,              // [batchSize * 256]
    double* __restrict__ d_avgLAB,          // [batchSize * 3]
    double* __restrict__ d_brightness,      // [batchSize]
    double* __restrict__ d_contrast,        // [batchSize]
    double* __restrict__ d_edgeDensity,     // [batchSize]
    int batchSize)
{
    int imgIdx = blockIdx.x;
    if (imgIdx >= batchSize) return;

    int tx = threadIdx.x;
    int stride = blockDim.x; // 320

    const uint8_t* img = d_images + imgIdx * IMG_PIX * 3;

    // 共享内存：每行 LAB 累积 (shared across threads in block)
    __shared__ float s_gridLab[GRID_CELLS * 3];   // 16 cells × L,A,B
    __shared__ float s_grayAcc;                    // 灰度总和
    __shared__ float s_graySqAcc;                  // 灰度平方和
    __shared__ int   s_edgeCount;                  // 边缘像素计数
    __shared__ uint32_t s_lbpHist[LBP_BINS];       // LBP 直方图

    // 初始化共享内存（仅前几个线程）
    if (tx < GRID_CELLS * 3) s_gridLab[tx] = 0.0f;
    if (tx == 0) { s_grayAcc = 0; s_graySqAcc = 0; s_edgeCount = 0; }
    if (tx < LBP_BINS) s_lbpHist[tx] = 0;
    __syncthreads();

    // 每个线程处理一列，跨行遍历
    float localGrayAcc = 0, localGraySq = 0;
    int localEdge = 0;

    for (int y = 0; y < IMG_H; ++y)
    {
        int x = tx;
        if (x >= IMG_W) continue;

        int idx = (y * IMG_W + x) * 3;
        float b = img[idx];
        float g = img[idx + 1];
        float r = img[idx + 2];

        // LAB
        float lv, av, bv;
        rgb2lab(r, g, b, lv, av, bv);

        // Grid4x4: 确定所属 cell
        int cellX = x / GRID_CW;
        int cellY = y / GRID_CH;
        int cellIdx = cellY * 4 + cellX;
        atomicAdd(&s_gridLab[cellIdx * 3 + 0], lv);
        atomicAdd(&s_gridLab[cellIdx * 3 + 1], av);
        atomicAdd(&s_gridLab[cellIdx * 3 + 2], bv);

        // Gray
        float gray = rgb2gray(r, g, b);
        localGrayAcc += gray;
        localGraySq += gray * gray;

        // Edge (simple Sobel-like: abs diff with right/bottom neighbor)
        if (x > 0 && y > 0)
        {
            float center = gray;
            float right = rgb2gray(img[(y * IMG_W + x - 1) * 3], img[(y * IMG_W + x - 1) * 3 + 1], img[(y * IMG_W + x - 1) * 3 + 2]);
            float down  = rgb2gray(img[((y-1) * IMG_W + x) * 3], img[((y-1) * IMG_W + x) * 3 + 1], img[((y-1) * IMG_W + x) * 3 + 2]);
            float grad = fabsf(center - right) + fabsf(center - down);
            if (grad > 30.0f) localEdge++;
        }

        // LBP: if not border
        if (x > 0 && x < IMG_W - 1 && y > 0 && y < IMG_H - 1)
        {
            uint8_t code = 0;
            float c = rgb2gray(img[((y) * IMG_W + x) * 3], img[((y) * IMG_W + x) * 3 + 1], img[((y) * IMG_W + x) * 3 + 2]);
            auto gv = [&](int dy, int dx) {
                float v = rgb2gray(img[((y+dy) * IMG_W + (x+dx)) * 3],
                                   img[((y+dy) * IMG_W + (x+dx)) * 3 + 1],
                                   img[((y+dy) * IMG_W + (x+dx)) * 3 + 2]);
                return v >= c ? 1.0f : 0.0f;
            };
            if (gv(-1,-1)) code |= 1;    if (gv(-1,0)) code |= 2;    if (gv(-1,1)) code |= 4;
            if (gv(0,1))  code |= 8;     if (gv(1,1))  code |= 16;   if (gv(1,0))  code |= 32;
            if (gv(1,-1)) code |= 64;    if (gv(0,-1)) code |= 128;
            atomicAdd(&s_lbpHist[code], 1u);
        }
    }

    // 归约 local → shared
    atomicAdd(&s_grayAcc, localGrayAcc);
    atomicAdd(&s_graySqAcc, localGraySq);
    atomicAdd(&s_edgeCount, localEdge);
    __syncthreads();

    // 前几个线程写结果
    if (tx == 0)
    {
        float nPix = IMG_PIX;
        float grayMean = s_grayAcc / nPix;
        float grayVar = s_graySqAcc / nPix - grayMean * grayMean;

        d_brightness[imgIdx] = grayMean;
        d_contrast[imgIdx]   = (grayVar > 0 ? sqrtf(grayVar) : 0.0f) / 255.0f;
        d_edgeDensity[imgIdx] = static_cast<double>(s_edgeCount) / nPix;

        int outOff = imgIdx * 192;
        for (int c = 0; c < GRID_CELLS; ++c)
        {
            float cnt = GRID_CW * GRID_CH;
            d_grid[outOff + c * 3 + 0] = s_gridLab[c * 3 + 0] / cnt;
            d_grid[outOff + c * 3 + 1] = s_gridLab[c * 3 + 1] / cnt;
            d_grid[outOff + c * 3 + 2] = s_gridLab[c * 3 + 2] / cnt;
        }

        // AvgLAB
        float totalL = 0, totalA = 0, totalB = 0;
        for (int c = 0; c < GRID_CELLS; ++c)
        {
            totalL += s_gridLab[c * 3 + 0];
            totalA += s_gridLab[c * 3 + 1];
            totalB += s_gridLab[c * 3 + 2];
        }
        d_avgLAB[imgIdx * 3 + 0] = totalL / nPix;
        d_avgLAB[imgIdx * 3 + 1] = totalA / nPix;
        d_avgLAB[imgIdx * 3 + 2] = totalB / nPix;

        // LBP (L1 normalize)
        int lbpOff = imgIdx * LBP_BINS;
        float lbpSum = 0;
        for (int i = 0; i < LBP_BINS; ++i) lbpSum += s_lbpHist[i];
        if (lbpSum > 0)
        {
            for (int i = 0; i < LBP_BINS; ++i)
                d_lbp[lbpOff + i] = s_lbpHist[i] / lbpSum;
        }
        else
        {
            for (int i = 0; i < LBP_BINS; ++i) d_lbp[lbpOff + i] = 0;
        }
    }

    // TinyImage: 线程 0-255 各计算一个 16×16 像素
    if (tx < 256)
    {
        int ty = tx / 16;
        int ttx = tx % 16;
        // 平均 pooling: 每个 tiny 像素覆盖 (320/16)×(180/16) = 20×11.25 ≈ 20×11 源像素
        int srcX0 = ttx * 20;
        int srcY0 = ty * 11;
        float sum = 0;
        int cnt = 0;
        for (int dy = 0; dy < 11 && (srcY0 + dy) < IMG_H; ++dy)
        {
            for (int dx = 0; dx < 20 && (srcX0 + dx) < IMG_W; ++dx)
            {
                int idx2 = ((srcY0 + dy) * IMG_W + (srcX0 + dx)) * 3;
                sum += rgb2gray(img[idx2 + 2], img[idx2 + 1], img[idx2]);
                cnt++;
            }
        }
        d_tiny[imgIdx * 256 + tx] = static_cast<uint8_t>(sum / cnt);
    }
}

// ============================================================
// 主机接口
// ============================================================
int extractBatch(
    const std::vector<cv::Mat>& images,
    std::vector<ImageRecord>& records,
    const std::string& featDir,
    const std::vector<std::string>& stems)
{
    int N = static_cast<int>(images.size());
    if (N <= 0) return 0;

    // 上传图像到 GPU
    size_t imgBytes = IMG_PIX * 3;
    std::vector<uint8_t> h_images(N * imgBytes);
    for (int i = 0; i < N; ++i)
    {
        cv::Mat bgr;
        if (images[i].channels() == 4)
        {
            cv::cvtColor(images[i], bgr, cv::COLOR_BGRA2BGR);
        }
        else
        {
            bgr = images[i];
        }
        std::memcpy(&h_images[i * imgBytes], bgr.data, imgBytes);
    }

    uint8_t* d_images = nullptr;
    float*   d_grid = nullptr;
    uint8_t* d_tiny = nullptr;
    float*   d_lbp = nullptr;
    double*  d_avgLAB = nullptr;
    double*  d_bright = nullptr;
    double*  d_contrast = nullptr;
    double*  d_edge = nullptr;

    cudaMalloc(&d_images, N * imgBytes);
    cudaMalloc(&d_grid, N * 192 * sizeof(float));
    cudaMalloc(&d_tiny, N * 256);
    cudaMalloc(&d_lbp, N * 256 * sizeof(float));
    cudaMalloc(&d_avgLAB, N * 3 * sizeof(double));
    cudaMalloc(&d_bright, N * sizeof(double));
    cudaMalloc(&d_contrast, N * sizeof(double));
    cudaMalloc(&d_edge, N * sizeof(double));

    cudaMemcpy(d_images, h_images.data(), N * imgBytes, cudaMemcpyHostToDevice);

    // 启动 kernel: N 个 block, 每个 320 线程
    featureKernel<<<N, 320>>>(
        d_images, d_grid, d_tiny, d_lbp,
        d_avgLAB, d_bright, d_contrast, d_edge, N);

    cudaDeviceSynchronize();
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess)
    {
        fprintf(stderr, "GPU feature error: %s\n", cudaGetErrorString(err));
        cudaFree(d_images); cudaFree(d_grid); cudaFree(d_tiny);
        cudaFree(d_lbp); cudaFree(d_avgLAB); cudaFree(d_bright);
        cudaFree(d_contrast); cudaFree(d_edge);
        return 0;
    }

    // 下载结果
    std::vector<float> h_grid(N * 192);
    std::vector<uint8_t> h_tiny(N * 256);
    std::vector<float> h_lbp(N * 256);
    std::vector<double> h_avgLAB(N * 3);
    std::vector<double> h_bright(N);
    std::vector<double> h_contrast(N);
    std::vector<double> h_edge(N);

    cudaMemcpy(h_grid.data(), d_grid, N * 192 * sizeof(float), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_tiny.data(), d_tiny, N * 256, cudaMemcpyDeviceToHost);
    cudaMemcpy(h_lbp.data(), d_lbp, N * 256 * sizeof(float), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_avgLAB.data(), d_avgLAB, N * 3 * sizeof(double), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_bright.data(), d_bright, N * sizeof(double), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_contrast.data(), d_contrast, N * sizeof(double), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_edge.data(), d_edge, N * sizeof(double), cudaMemcpyDeviceToHost);

    cudaFree(d_images); cudaFree(d_grid); cudaFree(d_tiny);
    cudaFree(d_lbp); cudaFree(d_avgLAB); cudaFree(d_bright);
    cudaFree(d_contrast); cudaFree(d_edge);

    // 填充 ImageRecord
    for (int i = 0; i < N; ++i)
    {
        auto& rec = records[i];
        rec.grid4x4.assign(&h_grid[i * 192], &h_grid[i * 192] + 192);
        rec.avgL = h_avgLAB[i * 3 + 0];
        rec.avgA = h_avgLAB[i * 3 + 1];
        rec.avgB = h_avgLAB[i * 3 + 2];
        rec.meanBrightness = h_bright[i];
        rec.contrast = h_contrast[i];
        rec.edgeDensity = h_edge[i];

        // stdBrightness placeholder
        rec.stdBrightness = 0;

        // Save TinyImage
        if (!featDir.empty() && i < static_cast<int>(stems.size()))
        {
            std::string tinyPath = featDir + "/" + stems[i] + ".tiny";
            std::ofstream ofs(tinyPath, std::ios::binary);
            if (ofs.is_open())
            {
                ofs.write(reinterpret_cast<const char*>(&h_tiny[i * 256]), 256);
                rec.tinyPath = tinyPath;
            }

            // Save LBP histogram
            std::string lbpPath = featDir + "/" + stems[i] + ".hist";
            std::ofstream ofs2(lbpPath, std::ios::binary);
            if (ofs2.is_open())
            {
                ofs2.write(reinterpret_cast<const char*>(&h_lbp[i * 256]), 256 * sizeof(float));
                rec.histPath = lbpPath;
            }
        }

        rec.featureVersion = 4;
    }

    return N;
}

} // namespace cuda
} // namespace mosaicraft
