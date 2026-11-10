#pragma once
#include "NeighborhoodState.h"
#include <cmath>
#include <limits>
#include <memory>
#include <random>

namespace mosaicraft
{
// 仅 MatchingAccess 可注入；产品配置和图库格式不携带实验策略。
struct SelectionOptions
{
    bool quality = false;
    bool spatial = false;
    bool expand = false;
    double relative = 0.05;
    double absolute = 0.0001;
    bool enabled() const
    {
        return quality || spatial || expand;
    }
};

struct SelectionDiagnostic
{
    int selected = -1, candidates = 0, qualityCandidates = 0;
    int radius = -1, radius3Candidates = -1;
    bool similarFallback = false;
    int initialCandidates = 0, initialRadius3Candidates = -1, queried = 0, expansionSteps = 0;
    double initialMinimum = std::numeric_limits<double>::quiet_NaN();
    double expansionMs = 0;
    double minimum = std::numeric_limits<double>::quiet_NaN();
    double limit = std::numeric_limits<double>::quiet_NaN();
    double error = std::numeric_limits<double>::quiet_NaN();
};

namespace detail
{
class SelectionPolicy
{
    struct Candidate
    {
        int index;
        double error, cost;
    };
    const std::vector<ImageRecord> &m_records;
    NeighborhoodState &m_legacy;
    const SelectionOptions &m_options;
    double m_usePenalty;
    const std::vector<int> *m_selected;
    int m_columns;
    std::unique_ptr<GridDuplicateCache> m_duplicates;
    std::vector<Candidate> m_candidates;
    std::vector<size_t> m_pool, m_quality;

    template <class Predicate> bool neighbor(int tile, int radius, Predicate matches) const
    {
        const int x = tile % m_columns, y = tile / m_columns;
        for (int dy = -radius; dy <= radius; ++dy)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                const int nx = x + dx, ny = y + dy;
                if (dx * dx + dy * dy > radius * radius || nx < 0 || nx >= m_columns || ny < 0)
                {
                    continue;
                }
                const size_t position = static_cast<size_t>(ny) * m_columns + nx;
                if (position >= m_selected->size() || position == static_cast<size_t>(tile))
                {
                    continue;
                }
                const int index = (*m_selected)[position];
                if (index >= 0 && matches(index))
                {
                    return true;
                }
            }
        }
        return false;
    }
    bool sameNearby(int tile, int index, int radius) const
    {
        return neighbor(tile, radius, [&](int other)
        {
            return m_records[index].id == m_records[other].id;
        });
    }
    bool similarNearby(int tile, int index)
    {
        return neighbor(tile, 1, [&](int other)
        {
            return m_records[index].id != m_records[other].id && m_duplicates->similar(index, other, 0.010);
        });
    }

  public:
    SelectionPolicy(const std::vector<ImageRecord> &records, NeighborhoodState &legacy,
                    const SelectionOptions &options, double usePenalty, size_t capacity,
                    const std::vector<int> *selected = nullptr, int columns = 0)
        : m_records(records), m_legacy(legacy), m_options(options), m_usePenalty(usePenalty),
          m_selected(selected), m_columns(columns)
    {
        m_candidates.reserve(capacity);
        m_pool.reserve(capacity);
        m_quality.reserve(capacity);
        if (options.spatial)
        {
            if (!selected || columns <= 0)
            {
                throw std::invalid_argument("spatial selection requires a position grid");
            }
            m_duplicates = std::make_unique<GridDuplicateCache>(records);
        }
    }
    void clear()
    {
        m_candidates.clear();
    }
    void add(int index, double error)
    {
        if (index < 0 || static_cast<size_t>(index) >= m_records.size() || !std::isfinite(error))
        {
            return;
        }
        for (const auto &candidate : m_candidates)
        {
            if (m_records[candidate.index].id == m_records[index].id)
            {
                return;
            }
        }
        m_candidates.push_back({index, error, 0});
    }
    bool contains(int index) const
    {
        return std::any_of(m_candidates.begin(), m_candidates.end(), [&](const Candidate &candidate)
        {
            return m_records[candidate.index].id == m_records[index].id;
        });
    }
    SelectionDiagnostic inspect(int tile)
    {
        SelectionDiagnostic diagnostic;
        diagnostic.candidates = static_cast<int>(m_candidates.size());
        if (m_candidates.empty())
        {
            return diagnostic;
        }
        diagnostic.minimum = std::min_element(m_candidates.begin(), m_candidates.end(),
                                              [](const Candidate &a, const Candidate &b)
                                              {
                                                  return a.error < b.error;
                                              })->error;
        diagnostic.limit = diagnostic.minimum + m_options.absolute + m_options.relative * diagnostic.minimum;
        m_quality.clear();
        for (size_t i = 0; i < m_candidates.size(); ++i)
        {
            const auto &candidate = m_candidates[i];
            if (m_options.quality && candidate.error > diagnostic.limit)
            {
                continue;
            }
            m_quality.push_back(i);
        }
        diagnostic.qualityCandidates = static_cast<int>(m_quality.size());
        if (m_options.spatial)
        {
            diagnostic.radius3Candidates = static_cast<int>(std::count_if(m_quality.begin(), m_quality.end(),
                [&](size_t i)
                {
                    return !sameNearby(tile, m_candidates[i].index, 3);
                }));
        }
        return diagnostic;
    }
    int choose(int tile, int topN, std::mt19937 &rng, SelectionDiagnostic &diagnostic)
    {
        diagnostic = inspect(tile);
        if (m_candidates.empty())
        {
            return -1;
        }
        m_pool.assign(m_quality.begin(), m_quality.end());
        if (m_options.spatial)
        {
            for (int radius = 3; radius >= 0; --radius)
            {
                m_pool.clear();
                for (const size_t i : m_quality)
                {
                    if (!sameNearby(tile, m_candidates[i].index, radius))
                    {
                        m_pool.push_back(i);
                    }
                }
                if (!m_pool.empty())
                {
                    diagnostic.radius = radius;
                    break;
                }
            }
            const auto similar = [&](size_t i)
            {
                return similarNearby(tile, m_candidates[i].index);
            };
            diagnostic.similarFallback = std::all_of(m_pool.begin(), m_pool.end(), similar);
            if (!diagnostic.similarFallback)
            {
                m_pool.erase(std::remove_if(m_pool.begin(), m_pool.end(), similar), m_pool.end());
            }
        }
        for (const size_t i : m_pool)
        {
            auto &candidate = m_candidates[i];
            candidate.cost = candidate.error + m_records[candidate.index].useCount * m_usePenalty;
            if (!m_options.spatial)
            {
                // 二维策略整体替换旧的三个扫描惩罚，不能悄悄保留固定间隔惩罚。
                m_legacy.applyPenalty(candidate.cost, candidate.index, tile);
            }
        }
        std::sort(m_pool.begin(), m_pool.end(), [&](size_t a, size_t b)
        {
            const auto &left = m_candidates[a], &right = m_candidates[b];
            if (left.cost != right.cost)
            {
                return left.cost < right.cost;
            }
            return m_records[left.index].id < m_records[right.index].id;
        });
        const int count = std::min(std::max(1, topN), static_cast<int>(m_pool.size()));
        const auto &chosen = m_candidates[m_pool[std::uniform_int_distribution<int>(0, count - 1)(rng)]];
        diagnostic.selected = chosen.index;
        diagnostic.error = chosen.error;
        return chosen.index;
    }
    size_t capacityBytes() const
    {
        return m_candidates.capacity() * sizeof(Candidate) +
            (m_pool.capacity() + m_quality.capacity()) * sizeof(size_t) +
            (m_duplicates ? m_duplicates->capacityBytes() : 0);
    }
};
} // namespace detail
} // namespace mosaicraft
