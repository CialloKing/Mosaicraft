#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mosaicraft
{
inline std::string encodeBase64(const std::vector<unsigned char> &bytes, std::string output = {})
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    output.reserve(output.size() + (bytes.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3)
    {
        const unsigned first = bytes[offset];
        const unsigned second = offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
        const unsigned third = offset + 2 < bytes.size() ? bytes[offset + 2] : 0;
        output += alphabet[first >> 2];
        output += alphabet[((first & 3) << 4) | (second >> 4)];
        output += offset + 1 < bytes.size() ? alphabet[((second & 15) << 2) | (third >> 6)] : '=';
        output += offset + 2 < bytes.size() ? alphabet[third & 63] : '=';
    }
    return output;
}
} // namespace mosaicraft
