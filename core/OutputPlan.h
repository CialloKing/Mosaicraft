#pragma once

#include <string>
#include <cstdint>

namespace mosaicraft
{
namespace detail
{
struct OutputFormat
{
    std::string format;
    std::string source;
};

struct OutputPlan
{
    std::string formatSource, requestedFormat, format, path;
    int64_t originalWidth = 0, originalHeight = 0;
    int originalTileWidth = 0, originalTileHeight = 0;
    int tileWidth = 0, tileHeight = 0, width = 0, height = 0;
    int tilesX = 0, tilesY = 0;
    bool scaled = false, formatChanged = false;
};

std::string normalizeOutputFormat(std::string format);
bool resolveOutputFormat(const std::string &inputPath, const std::string &outputPath,
                         const std::string &requestedFormat, bool explicitFormat, bool singleImage,
                         OutputFormat &result, std::string &error, std::string *field = nullptr);
std::string outputPathForFormat(const std::string &path, const std::string &format);
bool makeOutputPlan(const OutputFormat &format, const std::string &path, int tilesX, int tilesY,
                    int tileWidth, int tileHeight, bool singleImage, OutputPlan &result, std::string &error);

} // namespace detail
} // namespace mosaicraft
