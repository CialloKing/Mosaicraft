#include "PreviewCache.h"
#include "HostMemory.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string_view>

namespace mosaicraft
{
namespace
{
std::array<std::atomic_size_t, 3> reserved{};
std::atomic_size_t total{0};

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
} // namespace

PreviewCachePolicy PreviewCachePolicy::select(uint64_t available, size_t hostBudget, bool largeScores, bool largeFeatures)
{
    PreviewCachePolicy policy;
    constexpr uint64_t reserveBytes = 512 * MiB;
    if (available == 0)
    {
        return policy; // 无测量值时沿用旧上限，不自动扩容。
    }
    if (available <= hostBudget || available - hostBudget <= reserveBytes)
    {
        return {Tier::Disabled, {}, 0, 0};
    }
    const auto extra = available - hostBudget - reserveBytes;
    const auto fullBytes = (256 + 128 * largeScores + 64 * largeFeatures) * MiB;
    // 已驻留对象已经反映在系统可用内存里，不要从余量中扣两次。
    const size_t live = PreviewCacheReservation::totalBytes();
    if ((largeScores || largeFeatures) && extra >= fullBytes - std::min(live, fullBytes))
    {
        policy.tier = Tier::Full;
        policy.limits[0] += largeScores ? 128 * MiB : 0;
        policy.limits[1] += largeFeatures ? 64 * MiB : 0;
        policy.totalLimit = fullBytes;
    }
    policy.incrementalLimit = static_cast<size_t>(std::min<uint64_t>(extra, policy.totalLimit));
    const auto retained = std::min(live, policy.totalLimit);
    policy.totalLimit = retained + std::min(policy.totalLimit - retained, policy.incrementalLimit);
    return policy;
}

PreviewCachePolicy PreviewCachePolicy::forTask(size_t hostBudget, bool expand)
{
    // 只供私有快照复跑；未验收的扩容不改变服务默认。
    const char *trial = std::getenv("MOSAICRAFT_BENCH_PREVIEW_CACHE");
    const std::string_view variant = trial ? trial : "";
    const bool automatic = variant.empty() || variant == "auto";
    return select(availableMemory(), hostBudget, (automatic && expand) || variant == "scores" || variant == "both",
                  (automatic && expand) || variant == "features" || variant == "both");
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
