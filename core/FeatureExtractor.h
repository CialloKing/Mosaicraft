#pragma once

#include "Database.h"

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

    // 从归一化 BGR 图提取特征，填充 rec 各字段，不限于模板尺寸。
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
