#pragma once

#include <string>

namespace mosaicraft
{
namespace detail
{
struct OutputFormat
{
    std::string format;
    std::string source;
};

std::string normalizeOutputFormat(std::string format);
bool resolveOutputFormat(const std::string &inputPath, const std::string &outputPath,
                         const std::string &requestedFormat, bool explicitFormat, bool singleImage,
                         OutputFormat &result, std::string &error, std::string *field = nullptr);
std::string outputPathForFormat(const std::string &path, const std::string &format);

} // namespace detail
} // namespace mosaicraft
