#pragma once

#include "CudaBackend.h"
#include "core/Database.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mosaicraft
{
namespace cuda
{

// GPU 批量特征提取。
// 一次处理多张归一化图，填充对应的 ImageRecord。
//
// images:           [batchSize] 张 BGR 归一化图，尺寸必须一致且在模板尺寸名单中
// records:          [batchSize] 输出，各字段被填充（grid_lab, avgL/A/B,
//                   meanBrightness, stdBrightness, contrast, saturation,
//                   colorVariance, edgeDensity, featureVersion）
// textures:        [batchSize] 主机端纹理输出，存储由建库事务负责。
//
// 返回成功处理的数量。
int extractBatch(const std::vector<cv::Mat> &images, std::vector<ImageRecord> &records, std::vector<TextureFeatures> &textures, FeatureWorkspace *workspace = nullptr);

} // namespace cuda
} // namespace mosaicraft
