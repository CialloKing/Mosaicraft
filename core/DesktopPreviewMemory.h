#pragma once

#include "HostMemory.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace mosaicraft
{
enum class PreviewPixels
{
    Canvas,
    Worker
};

// 只预算能随时从文件重建的桌面像素，不把匹配工作区或导出缓冲冒充缓存。
inline size_t desktopPixelBudget(PreviewPixels owner, uint64_t total, uint64_t available, uint64_t resident)
{
    constexpr uint64_t MiB = 1024 * 1024, GiB = 1024 * MiB;
    if (total == 0)
    {
        return 64 * MiB; // 探测失败时，两进程各退回原来的保守上限。
    }
    const auto share = [owner](uint64_t bytes)
    {
        // 两进程独立采样，不允许它们把同一份可用内存都花一遍。
        return owner == PreviewPixels::Worker ? bytes / 4 : bytes - bytes / 4;
    };
    const uint64_t cap = share(std::min({total / 4, 8 * GiB, uint64_t(std::numeric_limits<size_t>::max())}));
    const uint64_t reserve = std::clamp(total / 5, 512 * MiB, 6 * GiB);
    resident = std::min(resident, cap);
    if (available < reserve)
    {
        return static_cast<size_t>(resident - std::min(resident, reserve - available));
    }
    // 系统余量已扣除了驻留像素，正常情况下只对新增长的空间分配份额。
    return static_cast<size_t>(resident + std::min(cap - resident, share(available - reserve)));
}

inline size_t desktopPixelBudget(PreviewPixels owner, uint64_t resident)
{
    uint64_t total = 0;
    const auto available = availableMemory(&total);
    return desktopPixelBudget(owner, total, available, resident);
}
} // namespace mosaicraft
