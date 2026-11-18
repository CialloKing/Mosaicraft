#pragma once
#include "MatchingBatch.h"
#include "RuntimeContext.h"
#include <charconv>
#include <cstdlib>
#include <initializer_list>
#include <string_view>

namespace mosaicraft::detail
{
// 仅供同一 CLI 的隔离进程配对，不持久化为工程配置。保留调用方注入的低内存测试限额。
struct CpuTrial
{
    unsigned featureGrain = 1, scoreGrain = 1, slots = 65536, ways = 1;
    int prefetch = -1, share = -1;
    bool planar = false;

    void read(MatchingLimits &limits, RuntimeContext &runtime)
    {
        const auto get = [&](const char *env, const char *key, int fallback, std::initializer_list<int> choices)
        {
            const char *raw = std::getenv(env);
            if (!raw)
            {
                return fallback;
            }
            const std::string_view text(raw);
            int value = 0;
            const auto end = raw + text.size();
            const auto parsed = std::from_chars(raw, end, value);
            if (parsed.ec != std::errc{} || parsed.ptr != end || std::find(choices.begin(), choices.end(), value) == choices.end())
            {
                throw std::invalid_argument(std::string("Invalid benchmark setting: ") + env);
            }
            runtime.metric(std::string("trial_") + key, value);
            return value;
        };
        constexpr size_t mib = 1024 * 1024;
        const auto host = get("MOSAICRAFT_BENCH_HOST_MIB", "host_mib", 0, {256, 512, 1024});
        const auto features = get("MOSAICRAFT_BENCH_FEATURE_MIB", "feature_mib", 0, {256, 512, 1024});
        const auto batch = get("MOSAICRAFT_BENCH_BATCH", "batch", 0, {1024, 2048});
        if (host)
        {
            limits.hostBytes = host * mib;
        }
        if (features)
        {
            limits.featureBytes = features * mib;
        }
        if (batch)
        {
            limits.maxTiles = batch;
        }
        featureGrain = get("MOSAICRAFT_BENCH_FEATURE_GRAIN", "feature_grain", 1, {1, 4, 8, 16});
        scoreGrain = get("MOSAICRAFT_BENCH_SCORE_GRAIN", "score_grain", 1, {1, 4, 8, 16});
        slots = get("MOSAICRAFT_BENCH_SLOTS", "slots", 65536, {65536, 262144, 1048576});
        planar = get("MOSAICRAFT_BENCH_PLANAR", "planar", 0, {0, 1}) != 0;
        ways = get("MOSAICRAFT_BENCH_WAYS", "ways", 1, {1, 2});
        prefetch = get("MOSAICRAFT_BENCH_PREFETCH", "prefetch", -1, {0, 1});
        share = get("MOSAICRAFT_BENCH_SHARE", "share", -1, {0, 1});
    }
};
} // namespace mosaicraft::detail
