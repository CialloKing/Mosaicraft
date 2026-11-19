#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mosaicraft
{
// 尝试缩小已成功解码的 PNG；只有新 IDAT 更小时才替换。
void compactLibraryPng(std::vector<uint8_t>& png, std::size_t decodedPixels);
}
