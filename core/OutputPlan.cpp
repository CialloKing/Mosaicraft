#include "OutputPlan.h"
#include "UnicodeIO.h"
#include <algorithm>
#include <cctype>
#include <limits>

namespace mosaicraft
{
namespace detail
{
namespace
{
bool supported(const std::string &format)
{
    return format == "jpg" || format == "png" || format == "webp" || format == "tiff";
}

std::string extensionFormat(const std::string &path)
{
    const auto extension = pathToUtf8(u8path(path).extension());
    return normalizeOutputFormat(extension.empty() ? std::string{} : extension.substr(1));
}
} // namespace

std::string normalizeOutputFormat(std::string format)
{
    std::transform(format.begin(), format.end(), format.begin(),
                   [](unsigned char value)
                   {
                       return static_cast<char>(std::tolower(value));
                   });
    if (format == "jpeg")
    {
        return "jpg";
    }
    if (format == "tif")
    {
        return "tiff";
    }
    return format;
}

bool resolveOutputFormat(const std::string &inputPath, const std::string &outputPath,
                         const std::string &requestedFormat, bool explicitFormat, bool singleImage,
                         OutputFormat &result, std::string &error, std::string *field)
{
    result = {};
    if (explicitFormat || !singleImage)
    {
        result = {normalizeOutputFormat(requestedFormat), explicitFormat ? "explicit" : "fallback"};
    }
    else if (!u8path(outputPath).extension().empty())
    {
        result = {extensionFormat(outputPath), "output_extension"};
    }
    else
    {
        result = {extensionFormat(inputPath), "input_extension"};
        if (!supported(result.format))
        {
            result = {"png", "fallback"};
        }
    }
    if (!supported(result.format))
    {
        if (field)
        {
            *field = result.source == "output_extension" ? "outputPath" : "format";
        }
        error = "output format must be jpg, png, webp or tiff";
        return false;
    }
    return true;
}

std::string outputPathForFormat(const std::string &path, const std::string &format)
{
    // 合法别名及大小写属于用户文件名；只有缺失或冲突时才改后缀。
    if (extensionFormat(path) == format)
    {
        return path;
    }
    auto result = u8path(path);
    result.replace_extension("." + format);
    return pathToUtf8(result);
}

bool makeOutputPlan(const OutputFormat &format, const std::string &path, int tilesX, int tilesY,
                    int tileWidth, int tileHeight, bool singleImage, OutputPlan &result, std::string &error)
{
    result = {};
    if (tilesX <= 0 || tilesY <= 0 || tileWidth <= 0 || tileHeight <= 0 || !supported(format.format))
    {
        error = "invalid output grid, tile dimensions or format";
        return false;
    }
    result.formatSource = format.source;
    result.requestedFormat = result.format = format.format;
    result.originalTileWidth = result.tileWidth = tileWidth;
    result.originalTileHeight = result.tileHeight = tileHeight;
    result.tilesX = tilesX;
    result.tilesY = tilesY;
    result.originalWidth = static_cast<int64_t>(tilesX) * tileWidth;
    result.originalHeight = static_cast<int64_t>(tilesY) * tileHeight;
    const int limit = format.format == "jpg" ? 65500 : format.format == "webp" ? 16383 : 0;
    const int64_t longest = std::max(result.originalWidth, result.originalHeight);
    if (singleImage && limit && longest > limit)
    {
        if (format.source != "explicit")
        {
            result.format = "png";
            result.formatChanged = true;
        }
        else
        {
            // 每块至少占一个像素；块数超限时不能靠 clamp(1) 假装适配成功。
            if (tilesX > limit || tilesY > limit)
            {
                error = "output grid exceeds " + format.format + " dimension limit " + std::to_string(limit) +
                        "; tiles cannot shrink below one pixel. Choose --format png/tiff or --tiled.";
                return false;
            }
            // 同一缩放比例用整数计算，避免浮点临界值使向下取整出现一像素误差。
            result.tileWidth = static_cast<int>(std::max<int64_t>(1, static_cast<int64_t>(tileWidth) * limit / longest));
            result.tileHeight = static_cast<int>(std::max<int64_t>(1, static_cast<int64_t>(tileHeight) * limit / longest));
            result.scaled = true;
        }
    }
    const int64_t width = static_cast<int64_t>(tilesX) * result.tileWidth;
    const int64_t height = static_cast<int64_t>(tilesY) * result.tileHeight;
    if (width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() ||
        (singleImage && limit && result.format != "png" && (width > limit || height > limit)))
    {
        error = "output dimensions exceed the selected encoder or INT_MAX limit";
        return false;
    }
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.path = singleImage ? outputPathForFormat(path, result.format) : path;
    return true;
}

} // namespace detail
} // namespace mosaicraft
