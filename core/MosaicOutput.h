#pragma once

#include "Database.h"
#include "MosaicEngine.h"
#include <functional>
#include <opencv2/core.hpp>

namespace mosaicraft
{

struct OutputStats
{
    int matched = 0;
    int failed = 0;
    double placementMs = 0;
    double encodingMs = 0;
    std::string path;
};

bool writeMosaicOutput(const MosaicEngine::Config &config, const std::string &path, int tilesX, int tilesY, int tileW,
                       int tileH, const std::vector<ImageRecord> &records, const std::vector<int> &selected,
                       const std::function<void(cv::Mat &, double)> &adjustColor, OutputStats &stats);

} // namespace mosaicraft
