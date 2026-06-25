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
// GPU 常量：归一化图尺寸 — 模板化以支持常见分辨率
// ============================================================
template<int W, int H> struct GpuParams {
    static constexpr int IMG_W = W;
    static constexpr int IMG_H = H;
    static constexpr int IMG_PIX = W * H;
    static constexpr int GRID_CW = (W + 7) / 8;
    static constexpr int GRID_CH = (H + 7) / 8;
    static constexpr int GRID_CELLS = 64;
    static constexpr int TINY_SX = W / 16;   // TinyImage 采样步长 X
    static constexpr int TINY_SY = H / 16;   // TinyImage 采样步长 Y
};
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
// 主 kernel：模板化，每个 block 处理一张图片
// 模板参数 W, H = 图像宽高
// ============================================================
template<int W, int H>
__global__ void featureKernel(
    const uint8_t* __restrict__ d_images,
    float* __restrict__ d_grid,
    uint8_t* __restrict__ d_tiny,
    float* __restrict__ d_lbp,
    double* __restrict__ d_avgLAB,
    double* __restrict__ d_brightness,
    double* __restrict__ d_contrast,
    double* __restrict__ d_edgeDensity,
    int batchSize)
{
    constexpr int IMG_W = W;
    constexpr int IMG_H = H;
    constexpr int IMG_PIX = W * H;
    constexpr int GRID_CW = (W + 7) / 8;
    constexpr int GRID_CH = (H + 7) / 8;
    constexpr int GRID_CELLS = 64;
    constexpr int TINY_SX = W / 16;
    constexpr int TINY_SY = H / 16;

    int imgIdx = blockIdx.x;
    if (imgIdx >= batchSize) return;

    int tx = threadIdx.x;
    int stride = blockDim.x;

    const uint8_t* img = d_images + imgIdx * IMG_PIX * 3;

    __shared__ float s_gridLab[GRID_CELLS * 3];
    __shared__ float s_grayAcc;
    __shared__ float s_graySqAcc;
    __shared__ int   s_edgeCount;
    __shared__ uint32_t s_lbpHist[LBP_BINS];

    if (tx < GRID_CELLS * 3) s_gridLab[tx] = 0.0f;
    if (tx == 0) { s_grayAcc = 0; s_graySqAcc = 0; s_edgeCount = 0; }
    if (tx < LBP_BINS) s_lbpHist[tx] = 0;
    __syncthreads();

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

        float lv, av, bv;
        rgb2lab(r, g, b, lv, av, bv);

        int cellX = x / GRID_CW;
        int cellY = y / GRID_CH;
        if (cellX >= 8) cellX = 7;
        if (cellY >= 8) cellY = 7;
        int cellIdx = cellY * 8 + cellX;
        atomicAdd(&s_gridLab[cellIdx * 3 + 0], lv);
        atomicAdd(&s_gridLab[cellIdx * 3 + 1], av);
        atomicAdd(&s_gridLab[cellIdx * 3 + 2], bv);

        float gray = rgb2gray(r, g, b);
        localGrayAcc += gray;
        localGraySq += gray * gray;

        if (x > 0 && y > 0)
        {
            float center = gray;
            float right = rgb2gray(img[(y * IMG_W + x - 1) * 3 + 2], img[(y * IMG_W + x - 1) * 3 + 1], img[(y * IMG_W + x - 1) * 3]);
            float down  = rgb2gray(img[((y-1) * IMG_W + x) * 3 + 2], img[((y-1) * IMG_W + x) * 3 + 1], img[((y-1) * IMG_W + x) * 3]);
            float grad = fabsf(center - right) + fabsf(center - down);
            if (grad > 30.0f) localEdge++;
        }

        if (x > 0 && x < IMG_W - 1 && y > 0 && y < IMG_H - 1)
        {
            uint8_t code = 0;
            float c = rgb2gray(img[((y) * IMG_W + x) * 3 + 2], img[((y) * IMG_W + x) * 3 + 1], img[((y) * IMG_W + x) * 3]);
            auto gv = [&](int dy, int dx) {
                float v = rgb2gray(img[((y+dy) * IMG_W + (x+dx)) * 3 + 2],
                                   img[((y+dy) * IMG_W + (x+dx)) * 3 + 1],
                                   img[((y+dy) * IMG_W + (x+dx)) * 3]);
                return v >= c ? 1.0f : 0.0f;
            };
            if (gv(-1,-1)) code |= 1;    if (gv(-1,0)) code |= 2;    if (gv(-1,1)) code |= 4;
            if (gv(0,1))  code |= 8;     if (gv(1,1))  code |= 16;   if (gv(1,0))  code |= 32;
            if (gv(1,-1)) code |= 64;    if (gv(0,-1)) code |= 128;
            atomicAdd(&s_lbpHist[code], 1u);
        }
    }

    atomicAdd(&s_grayAcc, localGrayAcc);
    atomicAdd(&s_graySqAcc, localGraySq);
    atomicAdd(&s_edgeCount, localEdge);
    __syncthreads();

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

    // TinyImage: loop over all 256 pixels, works with any blockDim
    for (int ti = tx; ti < 256; ti += blockDim.x)
    {
        int ty = ti / 16;
        int ttx = ti % 16;
        int srcX0 = ttx * TINY_SX;
        int srcY0 = ty * TINY_SY;
        float sum = 0;
        int cnt = 0;
        for (int dy = 0; dy < TINY_SY && (srcY0 + dy) < IMG_H; ++dy)
        {
            for (int dx = 0; dx < TINY_SX && (srcX0 + dx) < IMG_W; ++dx)
            {
                int idx2 = ((srcY0 + dy) * IMG_W + (srcX0 + dx)) * 3;
                sum += rgb2gray(img[idx2 + 2], img[idx2 + 1], img[idx2]);
                cnt++;
            }
        }
        d_tiny[imgIdx * 256 + ti] = static_cast<uint8_t>(sum / cnt);
    }
}

// 显式实例化：支持的四种分辨率

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
    if (images[0].empty()) return 0;

    // 从实际图像读取尺寸
    int imgW = images[0].cols;
    int imgH = images[0].rows;
    size_t imgBytes = static_cast<size_t>(imgW) * imgH * 3;

    // 上传图像到 GPU
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
    if (imgW == 180 && imgH == 320)
        featureKernel<180, 320><<<N, 180>>>(
            d_images, d_grid, d_tiny, d_lbp,
            d_avgLAB, d_bright, d_contrast, d_edge, N);
    else if (imgW == 320 && imgH == 180)
        featureKernel<320, 180><<<N, 320>>>(
            d_images, d_grid, d_tiny, d_lbp,
            d_avgLAB, d_bright, d_contrast, d_edge, N);
    else if (imgW == 360 && imgH == 640)
        featureKernel<360, 640><<<N, 360>>>(
            d_images, d_grid, d_tiny, d_lbp,
            d_avgLAB, d_bright, d_contrast, d_edge, N);
    else if (imgW == 640 && imgH == 360)
        featureKernel<640, 360><<<N, 640>>>(
            d_images, d_grid, d_tiny, d_lbp,
            d_avgLAB, d_bright, d_contrast, d_edge, N);
    else {
        fprintf(stderr, "GPU build: unsupported size %dx%d\n", imgW, imgH);
        cudaFree(d_images); cudaFree(d_grid); cudaFree(d_tiny); cudaFree(d_lbp);
        cudaFree(d_avgLAB); cudaFree(d_bright); cudaFree(d_contrast); cudaFree(d_edge);
        return 0;
    }
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

        rec.featureVersion = 5;
    }

    return N;
}

// ============================================================
// 调度器宏：根据尺寸选择模板实例
// ============================================================
#define LAUNCH_FEATURE(W, H) \
    if (imgW == W && imgH == H) return launchKernel<W, H>(h_images, N, h_avgLAB, h_grid, h_tiny, h_edge, h_lbp)

namespace {
    template<int W, int H>
    int launchKernel(const uint8_t* h_images, int N,
                     double* h_avgLAB, float* h_grid, uint8_t* h_tiny,
                     double* h_edge, float* h_lbp)
    {
        constexpr int PIX = W * H;
        size_t imgBytes = PIX * 3;
        uint8_t *d_img=nullptr; float *d_grid=nullptr; uint8_t *d_tiny=nullptr;
        float *d_lbp=nullptr; double *d_lab=nullptr, *d_bright=nullptr, *d_contrast=nullptr, *d_edge=nullptr;
        cudaMalloc(&d_img, N * imgBytes);
        cudaMalloc(&d_grid, N * 192 * sizeof(float));
        cudaMalloc(&d_tiny, N * 256);
        cudaMalloc(&d_lbp, N * 256 * sizeof(float));
        cudaMalloc(&d_lab, N * 3 * sizeof(double));
        cudaMalloc(&d_bright, N * sizeof(double));
        cudaMalloc(&d_contrast, N * sizeof(double));
        cudaMalloc(&d_edge, N * sizeof(double));
        cudaMemcpy(d_img, h_images, N * imgBytes, cudaMemcpyHostToDevice);
        featureKernel<W, H><<<N, W>>>(d_img, d_grid, d_tiny, d_lbp, d_lab, d_bright, d_contrast, d_edge, N);
        cudaDeviceSynchronize();
        cudaError_t err = cudaGetLastError();
        if (err != cudaSuccess) {
            fprintf(stderr, "GPU feature error (%dx%d): %s\n", W, H, cudaGetErrorString(err));
            cudaFree(d_img); cudaFree(d_grid); cudaFree(d_tiny); cudaFree(d_lbp);
            cudaFree(d_lab); cudaFree(d_bright); cudaFree(d_contrast); cudaFree(d_edge);
            return -1;
        }
        cudaMemcpy(h_avgLAB, d_lab, N*3*sizeof(double), cudaMemcpyDeviceToHost);
        cudaMemcpy(h_grid, d_grid, N*192*sizeof(float), cudaMemcpyDeviceToHost);
        cudaMemcpy(h_tiny, d_tiny, N*256, cudaMemcpyDeviceToHost);
        cudaMemcpy(h_lbp, d_lbp, N*256*sizeof(float), cudaMemcpyDeviceToHost);
        double hostEdge; cudaMemcpy(&hostEdge, d_edge, sizeof(double), cudaMemcpyDeviceToHost);
        *h_edge = hostEdge;
        cudaFree(d_img); cudaFree(d_grid); cudaFree(d_tiny); cudaFree(d_lbp);
        cudaFree(d_lab); cudaFree(d_bright); cudaFree(d_contrast); cudaFree(d_edge);
        return 0;
    }
} // anonymous namespace

// 带尺寸参数的版本
int extractFeaturesRaw(
    const uint8_t* h_images, int N, int imgW, int imgH,
    double* h_avgLAB, float* h_grid, uint8_t* h_tiny,
    double* h_edge, float* h_lbp)
{
    if (N <= 0) return 0;
    LAUNCH_FEATURE(180, 320);
    LAUNCH_FEATURE(320, 180);
    LAUNCH_FEATURE(360, 640);
    LAUNCH_FEATURE(640, 360);
    fprintf(stderr, "GPU: unsupported feature size %dx%d, use CPU fallback\n", imgW, imgH);
    return -1;
}

// 向后兼容：默认 180×320
int extractFeaturesRaw(
    const uint8_t* h_images, int N,
    double* h_avgLAB, float* h_grid, uint8_t* h_tiny,
    double* h_edge, float* h_lbp)
{
    return extractFeaturesRaw(h_images, N, 180, 320, h_avgLAB, h_grid, h_tiny, h_edge, h_lbp);
}


} // namespace cuda
} // namespace mosaicraft
