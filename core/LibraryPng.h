#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mosaicraft
{
// 删除 OpenCV 无法读取的超大 iTXt；其余块保持原字节，无法安全改写时保持输入不变。
bool stripOversizedPngText(std::vector<uint8_t>& png);

// 尝试缩小已成功解码的 PNG；只有新 IDAT 更小时才替换。
void compactLibraryPng(std::vector<uint8_t>& png, std::size_t decodedPixels);
}
