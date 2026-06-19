#pragma once

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mosaicraft
{

// ============================================================
// 共享特征计算工具（MosaicEngine + inspect 共用）
// ============================================================

inline double labDistance(double l1, double a1, double b1,
                           double l2, double a2, double b2)
{
    double dl = l1 - l2;
    double da = a1 - a2;
    double db = b1 - b2;
    return std::sqrt(dl * dl + da * da + db * db) / 100.0;
}

inline std::vector<float> computeGrid4x4(const cv::Mat& bgr)
{
    const int gridRows = 4, gridCols = 4;
    const int cellH = bgr.rows / gridRows, cellW = bgr.cols / gridCols;
    std::vector<float> grid;
    grid.reserve(48);
    for (int r = 0; r < gridRows; ++r)
    {
        for (int c = 0; c < gridCols; ++c)
        {
            cv::Mat cell = bgr(cv::Rect(c * cellW, r * cellH, cellW, cellH));
            cv::Mat cellLab;
            cv::cvtColor(cell, cellLab, cv::COLOR_BGR2Lab);
            cv::Scalar m = cv::mean(cellLab);
            grid.push_back(static_cast<float>(m[0]));
            grid.push_back(static_cast<float>(m[1]));
            grid.push_back(static_cast<float>(m[2]));
        }
    }
    return grid;
}

inline double gridDistance(const std::vector<float>& a,
                            const std::vector<float>& b)
{
    if (a.size() != 48 || b.size() != 48) { return 1e6; }
    double sum = 0.0;
    for (std::size_t i = 0; i < 16; ++i)
    {
        std::size_t idx = i * 3;
        double dl = a[idx] - b[idx];
        double da = a[idx + 1] - b[idx + 1];
        double db = a[idx + 2] - b[idx + 2];
        sum += std::sqrt(dl * dl + da * da + db * db);
    }
    return sum / 16.0 / 100.0;
}

inline std::vector<uint8_t> computeTinyImage(const cv::Mat& bgr)
{
    cv::Mat gray, tiny;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::resize(gray, tiny, cv::Size(16, 16), 0, 0, cv::INTER_AREA);
    std::vector<uint8_t> result(256);
    std::memcpy(result.data(), tiny.data, 256);
    return result;
}

inline double tinyMSE(const std::vector<uint8_t>& a,
                       const std::vector<uint8_t>& b)
{
    if (a.size() != 256 || b.size() != 256) { return 1.0; }
    double sum = 0.0;
    for (std::size_t i = 0; i < 256; ++i)
    {
        double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sum += diff * diff;
    }
    return sum / (256.0 * 255.0 * 255.0);
}

inline double computeEdgeDensity(const cv::Mat& bgr)
{
    cv::Mat gray, edges;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::Canny(gray, edges, 60, 120);
    return static_cast<double>(cv::countNonZero(edges)) / gray.total();
}

inline std::vector<float> computeLBPHistogram(const cv::Mat& bgr)
{
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    std::vector<float> hist(256, 0.0f);
    for (int y = 1; y < gray.rows - 1; ++y)
    {
        const uint8_t* rowUp   = gray.ptr<uint8_t>(y - 1);
        const uint8_t* rowCur  = gray.ptr<uint8_t>(y);
        const uint8_t* rowDown = gray.ptr<uint8_t>(y + 1);
        for (int x = 1; x < gray.cols - 1; ++x)
        {
            uint8_t center = rowCur[x];
            uint8_t code = 0;
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
    float sum = 0.0f;
    for (float v : hist) { sum += v; }
    if (sum > 0.0f)
    {
        for (float& v : hist) { v /= sum; }
    }
    return hist;
}

inline double lbpDistance(const std::vector<float>& a,
                           const std::vector<float>& b)
{
    if (a.size() != 256 || b.size() != 256) { return 1.0; }
    double sum = 0.0;
    for (std::size_t i = 0; i < 256; ++i)
    {
        sum += std::abs(a[i] - b[i]);
    }
    return sum / 2.0;
}

} // namespace mosaicraft
