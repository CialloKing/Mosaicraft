#include "FeatureExtractor.h"
#include "FeatureUtils.h"
#include "UnicodeIO.h"

#include <opencv2/imgproc.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <stdexcept>
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

    if (!featDir.empty() && !baseName.empty())
    {
        const auto tiny = computeTinyImageFromGray(gray);
        const auto hist = computeLBPHistogramFromGray(gray);
        saveFeatures(rec, featDir, baseName, tiny.data(), hist.data());
    }

    rec.featureVersion = featureVersion();
}

void FeatureExtractor::saveFeatures(ImageRecord &record, const std::string &directory,
                                     const std::string &stem, const uint8_t *tiny, const float *lbp)
{
    if (directory.empty() || stem.empty())
    {
        return;
    }
    auto write = [&](const std::string &path, const void *data, size_t bytes)
    {
        std::ofstream output(u8path(path), std::ios::binary);
        if (!output)
        {
            throw std::runtime_error("cannot write feature: " + path);
        }
        output.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
        output.close();
        if (!output)
        {
            throw std::runtime_error("feature write failed: " + path);
        }
    };
    const std::string tinyPath = directory + "/" + stem + ".tiny";
    const std::string histogramPath = directory + "/" + stem + ".hist";
    write(tinyPath, tiny, 256);
    write(histogramPath, lbp, 256 * sizeof(float));
    record.tinyPath = tinyPath;
    record.histPath = histogramPath;
}

} // namespace mosaicraft
