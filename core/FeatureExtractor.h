#pragma once

#include "Database.h"
#include "FeaturePayload.h"

#include <opencv2/core.hpp>
#include <string>

namespace mosaicraft
{

// FeatureExtractor — 从归一化图像中提取全部特征。
//
// 当前特征包含 8×8 LAB Grid、16×16 TinyImage、256 桶 LBP、颜色和边缘统计。
// 版本从 contracts.json 生成，所有后端复用相同定义与文件编码。
class FeatureExtractor
{
public:
    FeatureExtractor() = default;

    // 只提取内存特征；不需要纹理的分析调用可以省略该输出。
    void compute(const cv::Mat &bgr, ImageRecord &record, TextureFeatures *texture = nullptr);

    // 当前特征版本号
    static int featureVersion();
    // OpenCV 的颜色统计和 Canny 定义由两种后端共享，避免同一数据库列有两种语义。
    static void computeStatistics(const cv::Mat& bgr, ImageRecord& record,
                                  const cv::Mat& lab = {}, const cv::Mat& gray = {});
};

} // namespace mosaicraft
