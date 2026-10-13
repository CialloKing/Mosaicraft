#pragma once

#include "Database.h"
#include "MosaicEngine.h"
#include <functional>
#include <cstdint>
#include <opencv2/core.hpp>

namespace mosaicraft
{

struct OutputStats
{
    int matched = 0;
    int failed = 0;
    double placementMs = 0;
    double encodingMs = 0;
    double loadingMs = 0; // 工作线程耗时之和，不计入墙钟总和。
    double colorMs = 0, codecMs = 0;
    double overlaySamplingMs = 0, overlayBlendMs = 0; // 工作线程耗时之和。
    uint64_t overlayReferenceBytes = 0, overlayWorkingBytes = 0; // 后者为工作线程临时缓冲预算。
    double producerWaitMs = 0, consumerWaitMs = 0;
    size_t cacheHits = 0, cacheDecodes = 0, cachePeakBytes = 0, cacheSharedLoads = 0;
    std::string path;
};

bool writeMosaicOutput(const MosaicEngine::Config &config, const std::string &path, int tilesX, int tilesY, int tileW,
                       int tileH, const std::vector<ImageRecord> &records, const std::vector<int> &selected,
                       const std::function<void(cv::Mat &, double)> &adjustColor, OutputStats &stats,
                       const cv::Mat &target = cv::Mat());

// 选择结果保存图库下标；输出引用图库记录，不再为每个 tile 复制路径和 Grid。
bool writeIndexedMosaicOutput(const MosaicEngine::Config &config, const std::string &path, int tilesX, int tilesY,
                              int tileW, int tileH, const std::vector<ImageRecord> &library,
                              const std::vector<int> &selected,
                              const std::function<void(cv::Mat &, double)> &adjustColor, OutputStats &stats,
                              const cv::Mat &target = cv::Mat());

} // namespace mosaicraft
