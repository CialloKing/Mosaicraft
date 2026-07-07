#include "FeatureExtractor.h"

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
    return 5;  // v5: Grid 8×8 (192-dim)
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
    const int gridRows = 8;
    const int gridCols = 8;
    rec.grid4x4.clear();
    rec.grid4x4.reserve(gridRows * gridCols * 3);
    if (bgr.rows < gridRows || bgr.cols < gridCols)
    {
        rec.grid4x4.assign(gridRows * gridCols * 3, 0.0f);
    }
    else
    {
        const int cellH = bgr.rows / gridRows;
        const int cellW = bgr.cols / gridCols;
        for (int r = 0; r < gridRows; ++r)
        {
            for (int c = 0; c < gridCols; ++c)
            {
                cv::Rect roi(c * cellW, r * cellH, cellW, cellH);
                cv::Mat cellBGR = bgr(roi);
                cv::Mat cellLab;
                cv::cvtColor(cellBGR, cellLab, cv::COLOR_BGR2Lab);
                cv::Scalar cellMean = cv::mean(cellLab);

                rec.grid4x4.push_back(static_cast<float>(cellMean[0]));
                rec.grid4x4.push_back(static_cast<float>(cellMean[1]));
                rec.grid4x4.push_back(static_cast<float>(cellMean[2]));
            }
        }
    }

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

    // ——— V3：TinyImage 16×16 ———
    if (!featDir.empty() && !baseName.empty())
    {
        cv::Mat tiny;
        cv::resize(gray, tiny, cv::Size(16, 16), 0, 0, cv::INTER_AREA);

        std::string tinyPath = featDir + "/" + baseName + ".tiny";
        std::ofstream ofs(tinyPath, std::ios::binary);
        if (ofs.is_open())
        {
            ofs.write(reinterpret_cast<const char*>(tiny.data),
                      static_cast<std::streamsize>(tiny.total() * tiny.elemSize()));
            ofs.close();
            rec.tinyPath = tinyPath;
        }
    }

    // ——— V4：边缘密度 ———
    // Canny 边缘检测：低阈值取高阈值的 1/2（OpenCV 推荐）
    // 双阈值 60/120 对 180×320 中等尺寸图效果稳健
    cv::Mat edges;
    cv::Canny(gray, edges, 60, 120);
    // 边缘密度 = 边缘像素数 / 总像素数，范围 0~1
    double totalPixels = static_cast<double>(gray.total());
    double edgePixels = static_cast<double>(cv::countNonZero(edges));
    rec.edgeDensity = edgePixels / totalPixels;

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
        std::ofstream ofs(histPath, std::ios::binary);
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
