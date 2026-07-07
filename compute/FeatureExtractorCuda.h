#pragma once

#include "core/Database.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mosaicraft {
namespace cuda {

// GPU 批量特征提取。
// 一次处理多张归一化图（180×320 BGR），填充对应的 ImageRecord。
//
// images:           [batchSize] 张 BGR 归一化图，尺寸必须一致（180×320）
// records:          [batchSize] 输出，各字段被填充（grid4x4, avgL/A/B,
//                   meanBrightness, stdBrightness, contrast, saturation,
//                   colorVariance, edgeDensity, LBP, tinyPath, featureVersion）
// featDir, stems:   [batchSize] 特征文件目录和文件名前缀
//
// 返回成功处理的数量。
int extractBatch(
    const std::vector<cv::Mat>& images,
    std::vector<ImageRecord>& records,
    const std::string& featDir,
    const std::vector<std::string>& stems
);

} // namespace cuda
} // namespace mosaicraft
