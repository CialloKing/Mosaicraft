#pragma once

#include "MosaicService.h"
#include "OutputPlan.h"
#include <optional>
#include <vector>

namespace mosaicraft
{
class Database;

struct TargetGeometry
{
    int resizedWidth = 0, resizedHeight = 0, width = 0, height = 0;
    int columns = 0, rows = 0;
};

bool resolveTargetGeometry(int width, int height, const MosaicEngine::Config &config,
                           TargetGeometry &geometry, std::string &error);
void resizeTarget(cv::Mat &target, const TargetGeometry &geometry, const MosaicEngine::Config &config);
void padTarget(cv::Mat &target, const MosaicEngine::Config &config, int columns, int rows);
bool readLibraryDimensions(Database &database, int &width, int &height);
bool orientOutputTile(MosaicEngine::Config &config, int libraryWidth, int libraryHeight);

struct ByteRange
{
    uint64_t low = 0, high = 0;
};

struct MosaicPlan
{
    TargetGeometry target;
    detail::OutputPlan output;
    int sourceWidth = 0, sourceHeight = 0;
    int libraryWidth = 0, libraryHeight = 0, libraryCount = 0;
    uint64_t rawBytes = 0, files = 0;
    std::string mode, writeMode;
    ByteRange matchingRam, writingRam, fileBytes;
    std::optional<ByteRange> matchingVram;
    std::vector<std::string> warnings;
};

ServiceResult planMosaic(MosaicRequest request, MosaicPlan &plan);

struct FileSizeSample
{
    ByteRange bytes;
    uint64_t pixels = 0;
    int regions = 0;
};

ServiceResult sampleMosaicSize(const MosaicRequest &request, const MosaicSelection &selection,
                               const std::atomic_bool &canceled, FileSizeSample &sample);
} // namespace mosaicraft
