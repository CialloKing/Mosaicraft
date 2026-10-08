#pragma once
#include "Database.h"
#include "FeatureUtils.h"
#include <array>
#include <cstdint>

namespace mosaicraft
{
class GridDuplicateCache
{
    struct Entry
    {
        uint64_t key = UINT64_MAX;
        bool similar = false;
    };
    const std::vector<ImageRecord> &m_records;
    std::vector<bool> m_finite;
    std::vector<Entry> m_slots{65536};
    double m_threshold = -1;

  public:
    explicit GridDuplicateCache(const std::vector<ImageRecord> &records) : m_records(records)
    {
        for (const auto &record : records)
        {
            const auto &grid = record.grid4x4;
            m_finite.push_back(grid.size() == 192 && std::all_of(grid.begin(), grid.end(),
                [](float value)
                {
                    return std::isfinite(value);
                }));
        }
    }

    bool similar(int first, int second, double threshold)
    {
        if (threshold != m_threshold)
        {
            std::fill(m_slots.begin(), m_slots.end(), Entry{});
            m_threshold = threshold;
        }
        if (first > second)
        {
            std::swap(first, second);
        }
        const uint64_t key = (static_cast<uint64_t>(first) << 32) | static_cast<uint32_t>(second);
        // 固定容量直接映射；碰撞只覆盖槽位，完整键校验避免误判。
        uint64_t hash = key ^ (key >> 30);
        hash *= UINT64_C(0xbf58476d1ce4e5b9);
        hash ^= hash >> 27;
        auto &entry = m_slots[hash & (m_slots.size() - 1)];
        if (entry.key != key)
        {
            const auto &a = m_records[first].grid4x4;
            const auto &b = m_records[second].grid4x4;
            const double stop = m_finite[first] && m_finite[second] ? threshold : INFINITY;
            entry = {key, gridDistance8x8(a, b, false, stop) < threshold};
        }
        return entry.similar;
    }
};
} // namespace mosaicraft
