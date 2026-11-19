#include "PreviewCache.h"
#include "HostMemory.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <initializer_list>
#include <string_view>

namespace mosaicraft
{
namespace
{
std::array<std::atomic_size_t, 3> reserved{};
std::atomic_size_t total{0};

using CacheLimits = std::array<size_t, 3>;
constexpr size_t MiB = PreviewCachePolicy::MiB;
constexpr CacheLimits kExpandedLimits{256 * MiB, 128 * MiB, 64 * MiB};
constexpr CacheLimits kMediumLimits{512 * MiB, 128 * MiB, 192 * MiB};
constexpr CacheLimits kLargeLimits{1024 * MiB, 256 * MiB, 384 * MiB};

bool reserve(std::atomic_size_t &counter, size_t delta, size_t limit)
{
    size_t used = counter.load(std::memory_order_relaxed);
    do
    {
        if (used > limit || delta > limit - used)
        {
            return false;
        }
    } while (!counter.compare_exchange_weak(used, used + delta, std::memory_order_relaxed));
    return true;
}

PreviewCachePolicy selectLimits(uint64_t available, size_t hostBudget, std::initializer_list<CacheLimits> tiers)
{
    PreviewCachePolicy policy;
    constexpr uint64_t reserveBytes = 512 * MiB;
    if (available == 0)
    {
        return policy; // 无测量值时沿用旧上限，不自动扩容。
    }
    if (available <= hostBudget || available - hostBudget <= reserveBytes)
    {
        return {PreviewCachePolicy::Tier::Disabled, {}, 0, 0};
    }
    const auto extra = available - hostBudget - reserveBytes;
    // 已驻留对象已经反映在系统可用内存里，不要从余量中扣两次。
    const size_t live = PreviewCacheReservation::totalBytes();
    for (const auto &limits : tiers)
    {
        const auto fullBytes = limits[0] + limits[1] + limits[2];
        if (extra >= fullBytes - std::min(live, fullBytes))
        {
            policy.tier = PreviewCachePolicy::Tier::Full;
            policy.limits = limits;
            policy.totalLimit = fullBytes;
            break;
        }
    }
    policy.incrementalLimit = static_cast<size_t>(std::min<uint64_t>(extra, policy.totalLimit));
    const auto retained = std::min(live, policy.totalLimit);
    policy.totalLimit = retained + std::min(policy.totalLimit - retained, policy.incrementalLimit);
    return policy;
}

} // namespace

PreviewCachePolicy PreviewCachePolicy::select(uint64_t available, size_t hostBudget, bool allowLarge)
{
    if (allowLarge)
    {
        // 大档放不下时逐级回退，仍尽量保留目标特征和 ANN 候选。
        return selectLimits(available, hostBudget, {kLargeLimits, kMediumLimits, kExpandedLimits});
    }
    return selectLimits(available, hostBudget, {});
}

PreviewCachePolicy PreviewCachePolicy::forTask(size_t hostBudget, bool allowLarge)
{
    // 私有基准可固定上限对照；生产不需要设置环境变量。
    const char *trial = std::getenv("MOSAICRAFT_BENCH_PREVIEW_CACHE");
    const std::string_view variant = trial ? trial : "";
    const auto available = availableMemory();
    if (variant.empty() || variant == "auto")
    {
        return select(available, hostBudget, allowLarge);
    }
    if (variant == "512-128-64")
    {
        return selectLimits(available, hostBudget, {{512 * MiB, 128 * MiB, 64 * MiB}});
    }
    if (variant == "512-128-192")
    {
        return selectLimits(available, hostBudget, {kMediumLimits});
    }
    if (variant == "1024-256-384")
    {
        return selectLimits(available, hostBudget, {kLargeLimits});
    }
    if (variant == "both")
    {
        return selectLimits(available, hostBudget, {kExpandedLimits});
    }
    if (variant == "scores" || variant == "features")
    {
        auto limits = PreviewCachePolicy{}.limits;
        limits[variant == "scores" ? 0 : 1] *= 2;
        return selectLimits(available, hostBudget, {limits});
    }
    return select(available, hostBudget, false);
}

bool PreviewCacheReservation::grow(size_t bytes, const PreviewCachePolicy &policy, PreviewCacheMiss &miss)
{
    const auto slot = static_cast<size_t>(m_kind);
    if (policy.tier == PreviewCachePolicy::Tier::Disabled || bytes > policy.incrementalLimit)
    {
        miss = PreviewCacheMiss::Memory;
        return false;
    }
    if (bytes > policy.limits[slot])
    {
        miss = PreviewCacheMiss::Size;
        return false;
    }
    if (bytes <= m_bytes)
    {
        return true;
    }
    const size_t delta = bytes - m_bytes;
    if (!reserve(reserved[slot], delta, policy.limits[slot]))
    {
        miss = PreviewCacheMiss::ProcessBudget;
        return false;
    }
    if (!reserve(total, delta, policy.totalLimit))
    {
        reserved[slot].fetch_sub(delta, std::memory_order_relaxed);
        miss = PreviewCacheMiss::ProcessBudget;
        return false;
    }
    m_bytes = bytes;
    return true;
}

PreviewCacheReservation::~PreviewCacheReservation()
{
    total.fetch_sub(m_bytes, std::memory_order_relaxed);
    reserved[static_cast<size_t>(m_kind)].fetch_sub(m_bytes, std::memory_order_relaxed);
}

size_t PreviewCacheReservation::totalBytes()
{
    return total.load(std::memory_order_relaxed);
}
} // namespace mosaicraft
