#pragma once
#include "Database.h"
#include "FeatureUtils.h"
#include <array>
#include <cstdint>
#include <stdexcept>

namespace mosaicraft
{
struct NeighborCounters
{
    uint64_t candidates = 0, hits = 0, distances = 0, earlyExits = 0, pruned = 0, replacements = 0;
};

class GridDuplicateCache
{
    struct Entry
    {
        uint64_t key = UINT64_MAX;
        bool similar = false;
    };
    const std::vector<ImageRecord> &m_records;
    std::vector<bool> m_finite;
    std::vector<Entry> m_slots;
    unsigned m_ways;
    double m_threshold = -1;
    NeighborCounters *m_counters;

  public:
    static size_t extraBytes(unsigned slots)
    {
        return slots > 65536 ? (slots - 65536) * sizeof(Entry) : 0;
    }
    explicit GridDuplicateCache(const std::vector<ImageRecord> &records, NeighborCounters *counters = nullptr,
                                unsigned slots = 65536, unsigned ways = 1)
        : m_records(records), m_slots(slots), m_ways(ways), m_counters(counters)
    {
        if (slots == 0 || (slots & (slots - 1)) || (ways != 1 && ways != 2) || slots < ways)
        {
            throw std::invalid_argument("invalid neighbor cache geometry");
        }
        for (const auto &record : records)
        {
            const auto &grid = record.gridLab;
            m_finite.push_back(grid.size() == 192 &&
                               std::all_of(grid.begin(), grid.end(), [](float value) { return std::isfinite(value); }));
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
        const size_t slot = hash & (m_slots.size() - 1);
        auto &entry = m_slots[m_ways == 2 ? slot & ~size_t(1) : slot];
        if (m_ways == 2 && entry.key != key)
        {
            // 首路为最近访问项。总槽数不变，两路对照不额外增加内存。
            std::swap(entry, m_slots[slot | 1]);
        }
        if (entry.key != key)
        {
            const auto &a = m_records[first].gridLab;
            const auto &b = m_records[second].gridLab;
            const double stop = m_finite[first] && m_finite[second] ? threshold : INFINITY;
            if (m_counters && entry.key != UINT64_MAX)
            {
                ++m_counters->replacements;
            }
            entry = {key, gridDistance8x8(a, b, false, stop, m_counters ? &m_counters->earlyExits : nullptr) < threshold};
            if (m_counters)
            {
                ++m_counters->distances;
            }
        }
        else if (m_counters)
        {
            ++m_counters->hits;
        }
        return entry.similar;
    }
    size_t capacityBytes() const
    {
        return m_slots.capacity() * sizeof(Entry) + (m_finite.capacity() + 7) / 8;
    }
};
} // namespace mosaicraft
