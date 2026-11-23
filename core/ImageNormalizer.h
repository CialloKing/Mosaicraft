#pragma once

#include "BenchmarkTimer.h"
#include "LibraryEncodingOptions.h"
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace mosaicraft
{
class SmartCropInference;
struct SmartCropStats;

// ImageNormalizer — 将任意尺寸/格式的图片归一化为统一规格（默认 180×320）。
// 算法：等比缩放至覆盖目标尺寸 → 中心裁剪，保证无拉伸、无黑边。
class ImageNormalizer
{
public:
    // 默认目标尺寸与 Mosaicraft 图库规格一致
    explicit ImageNormalizer(int targetWidth = 180, int targetHeight = 320,
                             SmartCropInference *smartCrop = nullptr, SmartCropStats *stats = nullptr);

    // 同尺寸、同格式时原字节保存；否则缩放、裁剪后编码。
    // storedPixels 按需返回实际保存内容的解码像素，供特征流水线使用。
    // 成功返回 true，失败返回 false（并输出错误信息到 stderr）
    bool process(const std::string& inputPath, const std::string& outputPath,
                 const std::vector<int>& writeParams = {}, cv::Mat* storedPixels = nullptr);

    // 建库和追加共用输出格式与签名校验。
    struct LibraryTimings
    {
        BenchmarkSample normalize, encode, write;

        void merge(const LibraryTimings &other)
        {
            normalize.merge(other.normalize);
            encode.merge(other.encode);
            write.merge(other.write);
        }
    };
    static std::string libraryOutputExtension(const std::string& inputPath);
    // 原地入库预检可检测是否必须写出修复后的 PNG，常规读取仍要求文件本身可解码。
    static cv::Mat readLibrarySource(const std::string& inputPath, bool* requiresPngRewrite = nullptr);
    bool processLibraryImage(const std::string& inputPath, const std::string& outputPath,
                             const LibraryEncodingOptions& encoding, cv::Mat* storedPixels, LibraryTimings* timings);
    bool processStandaloneImage(const std::string& inputPath, const std::string& outputPath,
                                cv::Mat* storedPixels = nullptr);

    // 返回归一化后的 cv::Mat（不写磁盘），供流水线使用
    cv::Mat processToMat(const std::string& inputPath);

    // 等比缩放：使图像刚好覆盖目标尺寸
    cv::Mat resizeToCover(const cv::Mat& src);
    // 中心裁剪到精确的目标尺寸
    cv::Mat centerCrop(const cv::Mat& src);

private:
    bool processImpl(const std::string& inputPath, const std::string& outputPath,
                     const std::vector<int>& writeParams, cv::Mat* storedPixels, bool compactPng,
                     const LibraryEncodingOptions* encoding = nullptr, LibraryTimings* timings = nullptr);
    cv::Mat normalize(const cv::Mat& src);
    int m_targetWidth;
    int m_targetHeight;
    SmartCropInference *m_smartCrop;
    SmartCropStats *m_smartStats;
};

} // namespace mosaicraft
