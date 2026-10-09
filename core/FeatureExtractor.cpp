#include "FeatureExtractor.h"
#include "FeatureUtils.h"
#include "UnicodeIO.h"

#include <opencv2/imgproc.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace mosaicraft
{

int FeatureExtractor::featureVersion()
{
    // V1 = 1: AvgLAB + mean brightness
    // V2 = 2: + Grid4x4
    // V3 = 3: + TinyImage + 颜色统计
    // V4 = 4: + EdgeDensity + LBP 纹理直方图
    return 6;  // v6: 完整 8×8 分区及统一后端统计/Canny 定义
}

void FeatureExtractor::computeStatistics(const cv::Mat& bgr, ImageRecord& rec,
                                         const cv::Mat& inputLab, const cv::Mat& inputGray)
{
    cv::Mat lab = inputLab, gray = inputGray;
    if (lab.empty())
    {
        cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);
    }
    if (gray.empty())
    {
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    }
    rec.meanBrightness = cv::mean(gray)[0];
    // ——— V3：颜色统计 ———
    cv::Scalar meanGray, stdGray;
    cv::meanStdDev(gray, meanGray, stdGray);
    rec.stdBrightness = stdGray[0];
    rec.contrast = stdGray[0] / 255.0;

    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    std::vector<cv::Mat> hsvChannels;
    cv::split(hsv, hsvChannels);
    rec.saturation = cv::mean(hsvChannels[1])[0];

    std::vector<cv::Mat> labChannels;
    cv::split(lab, labChannels);
    cv::Scalar meanA, stdA, meanB, stdB;
    cv::meanStdDev(labChannels[1], meanA, stdA);
    cv::meanStdDev(labChannels[2], meanB, stdB);
    rec.colorVariance = stdA[0] * stdA[0] + stdB[0] * stdB[0];

    rec.edgeDensity = computeEdgeDensityFromGray(gray);
}

void FeatureExtractor::compute(const cv::Mat& bgr,
                                ImageRecord& rec,
                                const std::string& featDir,
                                const std::string& baseName)
{
    // ——— V1：全局 AvgLAB + 亮度 ———
    cv::Mat lab;
    cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);

    cv::Scalar meanLab = cv::mean(lab);
    rec.avgL = meanLab[0];
    rec.avgA = meanLab[1];
    rec.avgB = meanLab[2];

    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    rec.meanBrightness = cv::mean(gray)[0];

    // ——— V2：8×8 LAB Grid ———
    rec.grid4x4 = computeGrid8x8FromLab(lab);

    computeStatistics(bgr, rec, lab, gray);

    // ——— V3：TinyImage 16×16 ———
    if (!featDir.empty() && !baseName.empty())
    {
        cv::Mat tiny;
        cv::resize(gray, tiny, cv::Size(16, 16), 0, 0, cv::INTER_AREA);

        std::string tinyPath = featDir + "/" + baseName + ".tiny";
        std::ofstream ofs(u8path(tinyPath), std::ios::binary);
        if (ofs.is_open())
        {
            ofs.write(reinterpret_cast<const char*>(tiny.data),
                      static_cast<std::streamsize>(tiny.total() * tiny.elemSize()));
            ofs.close();
            rec.tinyPath = tinyPath;
        }
    }

    // ——— V4：LBP 纹理直方图 ———
    // 基本 LBP (3×3 邻域，8 个邻居) → 256 维直方图
    // 跳过 1 像素宽的边界，逐像素计算 LBP 8-bit 码
    if (!featDir.empty() && !baseName.empty())
    {
        // 256 个 bin 的直方图，初始化为 0
        std::vector<float> hist(256, 0.0f);

        // LBP 邻居相对坐标（顺时针，左上起）
        const int dx[8] = {-1,  0,  1, 1, 1, 0, -1, -1};
        const int dy[8] = {-1, -1, -1, 0, 1, 1,  1,  0};

        for (int y = 1; y < gray.rows - 1; ++y)
        {
            // 行指针加速：当前行及上下相邻行
            const uint8_t* rowUp   = gray.ptr<uint8_t>(y - 1);
            const uint8_t* rowCur  = gray.ptr<uint8_t>(y);
            const uint8_t* rowDown = gray.ptr<uint8_t>(y + 1);

            for (int x = 1; x < gray.cols - 1; ++x)
            {
                uint8_t center = rowCur[x];
                uint8_t code = 0;

                // 8 个邻居，按 dx/dy 顺序：生成 8 位 LBP 码
                // 手动展开以利用行指针（避免逐像素边界检查）
                code |= ((rowUp[x - 1]   >= center) ? 1 : 0) << 0;
                code |= ((rowUp[x]       >= center) ? 1 : 0) << 1;
                code |= ((rowUp[x + 1]   >= center) ? 1 : 0) << 2;
                code |= ((rowCur[x + 1]  >= center) ? 1 : 0) << 3;
                code |= ((rowDown[x + 1] >= center) ? 1 : 0) << 4;
                code |= ((rowDown[x]     >= center) ? 1 : 0) << 5;
                code |= ((rowDown[x - 1] >= center) ? 1 : 0) << 6;
                code |= ((rowCur[x - 1]  >= center) ? 1 : 0) << 7;

                hist[code] += 1.0f;
            }
        }

        // L1 归一化：使直方图不受图像尺寸影响
        float sum = 0.0f;
        for (float v : hist)
        {
            sum += v;
        }
        if (sum > 0.0f)
        {
            for (float& v : hist)
            {
                v /= sum;
            }
        }

        // 保存为 .hist 文件（256 个 float = 1024 字节）
        std::string histPath = featDir + "/" + baseName + ".hist";
        std::ofstream ofs(u8path(histPath), std::ios::binary);
        if (ofs.is_open())
        {
            ofs.write(reinterpret_cast<const char*>(hist.data()),
                      static_cast<std::streamsize>(hist.size() * sizeof(float)));
            ofs.close();
            rec.histPath = histPath;
        }
    }

    rec.featureVersion = featureVersion();
}

} // namespace mosaicraft
