#pragma once
#include <cstdint>

namespace mosaicraft
{
constexpr int projectFormatVersion = 2;

inline bool supportedProjectVersion(int64_t version)
{
    return version >= 1 && version <= projectFormatVersion;
}
} // namespace mosaicraft
