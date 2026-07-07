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
};

} // namespace mosaicraft
