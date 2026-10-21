#pragma once

#include "Database.h"

#include <opencv2/core.hpp>
#include <string>

namespace mosaicraft
{

// FeatureExtractor — 从归一化图像中提取全部特征。
//
// V1: AvgLAB + 亮度均值
// V2: 4×4 LAB Grid (48 float)
// V3: 16×16 TinyImage + 颜色统计 (contrast, std brightness, saturation,
//     color variance)
// V4: 边缘密度 + 纹理（预留）
//
// 接口固定，内部实现随版本递增；featureVersion() 标记当前版本号。
class FeatureExtractor
{
public:
    FeatureExtractor() = default;

    // 从归一化图（180×320 BGR）提取特征，填充 rec 各字段。
    // featDir  + baseName 非空时，TinyImage 写入 featDir/baseName.tiny
    void compute(const cv::Mat& normalizedBGR,
                 ImageRecord& rec,
                 const std::string& featDir = "",
                 const std::string& baseName = "");

    // 当前特征版本号
    static int featureVersion();
    // OpenCV 的颜色统计和 Canny 定义由两种后端共享，避免同一数据库列有两种语义。
    static void computeStatistics(const cv::Mat& bgr, ImageRecord& record,
                                  const cv::Mat& lab = {}, const cv::Mat& gray = {});
    // 两种 CPU 兼容提取路径共用落盘检查，不改变特征文件格式。
    static void saveFeatures(ImageRecord &record, const std::string &directory, const std::string &stem,
                             const uint8_t *tiny, const float *lbp);
};

} // namespace mosaicraft
