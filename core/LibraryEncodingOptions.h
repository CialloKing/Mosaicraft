#pragma once

#include <optional>

namespace mosaicraft
{

struct LibraryEncodingOptions
{
    int jpgQuality = 100;
    std::optional<int> webpQuality; // 省略质量参数时使用无损 WebP。
    int pngLevel = 9;
};

} // namespace mosaicraft
