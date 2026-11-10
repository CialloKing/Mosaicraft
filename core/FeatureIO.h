#pragma once

#include "UnicodeIO.h"
#include "Version.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mosaicraft
{
inline constexpr size_t kFeatureHeaderBytes = 16;
enum class FeatureKind : uint16_t
{
    Tiny = 1,
    Lbp = 2
};

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
inline std::vector<uint8_t> encodeFloats(const float *values, size_t count)
{
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE float32 required");
    std::vector<uint8_t> bytes(count * 4);
    for (size_t i = 0; i < count; ++i)
    {
        uint32_t bits;
        std::memcpy(&bits, values + i, 4);
        encodeUnsigned(bytes.data() + i * 4, bits, 4);
    }
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
inline void writeFeature(const std::string &path, FeatureKind kind, const void *data)
{
    std::array<uint8_t, kFeatureHeaderBytes> header{'M', 'C', 'R', 'F', 'E', 'A', 'T', 0};
    encodeUnsigned(header.data() + 8, kFeatureVersion, 2);
    encodeUnsigned(header.data() + 10, static_cast<uint16_t>(kind), 2);
    encodeUnsigned(header.data() + 12, 256, 4);
    std::ofstream output(u8path(path), std::ios::binary);
    output.write(reinterpret_cast<const char *>(header.data()), header.size());
    if (kind == FeatureKind::Tiny)
    {
        output.write(static_cast<const char *>(data), 256);
    }
    else
    {
        const auto *values = static_cast<const float *>(data);
        for (size_t i = 0; i < 256; ++i)
        {
            if (!std::isfinite(values[i]))
            {
                throw std::runtime_error("nonfinite feature: " + path);
            }
        }
        const auto payload = encodeFloats(values, 256);
        output.write(reinterpret_cast<const char *>(payload.data()), payload.size());
    }
    output.close();
    if (!output)
    {
        throw std::runtime_error("feature write failed: " + path);
    }
}
inline bool readFeature(const std::string &path, FeatureKind kind, void *data)
{
    std::ifstream input(u8path(path), std::ios::binary);
    std::array<uint8_t, kFeatureHeaderBytes> header{};
    input.read(reinterpret_cast<char *>(header.data()), header.size());
    if (!input || std::memcmp(header.data(), "MCRFEAT", 8) != 0 ||
        decodeUnsigned(header.data() + 8, 2) != kFeatureVersion ||
        decodeUnsigned(header.data() + 10, 2) != static_cast<uint16_t>(kind) ||
        decodeUnsigned(header.data() + 12, 4) != 256)
    {
        return false;
    }
    if (kind == FeatureKind::Tiny)
    {
        input.read(static_cast<char *>(data), 256);
    }
    else
    {
        std::array<uint8_t, 1024> bytes{};
        input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
        auto *values = static_cast<float *>(data);
        decodeFloats(bytes.data(), values, 256);
        for (size_t i = 0; i < 256; ++i)
        {
            if (!std::isfinite(values[i]))
            {
                return false;
            }
        }
    }
    return input && input.peek() == std::char_traits<char>::eof();
}
}
