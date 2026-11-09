#include "OutputPlan.h"
#include "UnicodeIO.h"
#include <algorithm>
#include <cctype>

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

} // namespace detail
} // namespace mosaicraft
