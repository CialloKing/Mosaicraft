#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace mosaicraft
{

inline void encodeUnsigned(uint8_t *out, uint64_t value, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
    {
        out[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}
inline uint64_t decodeUnsigned(const uint8_t *in, size_t bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i)
    {
        value |= static_cast<uint64_t>(in[i]) << (i * 8);
    }
    return value;
}
inline void encodeFloats(const float *values, size_t count, uint8_t *bytes)
{
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE float32 required");
    for (size_t i = 0; i < count; ++i)
    {
        uint32_t bits;
        std::memcpy(&bits, values + i, 4);
        encodeUnsigned(bytes + i * 4, bits, 4);
    }
}
inline std::vector<uint8_t> encodeFloats(const float *values, size_t count)
{
    std::vector<uint8_t> bytes(count * 4);
    encodeFloats(values, count, bytes.data());
    return bytes;
}
inline void decodeFloats(const uint8_t *bytes, float *values, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        const auto bits = static_cast<uint32_t>(decodeUnsigned(bytes + i * 4, 4));
        std::memcpy(values + i, &bits, 4);
    }
}
}
