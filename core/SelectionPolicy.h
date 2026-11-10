#pragma once
#include "NeighborhoodState.h"
#include <cmath>
#include <limits>
#include <random>

namespace mosaicraft
{
// 仅 MatchingAccess 可注入；产品配置和图库格式不携带实验策略。
struct SelectionOptions
{
    bool quality = false;
    double relative = 0.05;
    double absolute = 0.0001;
    bool enabled() const
    {
        return quality;
    }
};

struct SelectionDiagnostic
{
    int selected = -1, candidates = 0, qualityCandidates = 0;
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
    std::vector<Candidate> m_candidates;
    std::vector<size_t> m_pool;

  public:
    SelectionPolicy(const std::vector<ImageRecord> &records, NeighborhoodState &legacy,
                    const SelectionOptions &options, double usePenalty, size_t capacity)
        : m_records(records), m_legacy(legacy), m_options(options), m_usePenalty(usePenalty)
    {
        m_candidates.reserve(capacity);
        m_pool.reserve(capacity);
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
    int choose(int tile, int topN, std::mt19937 &rng, SelectionDiagnostic &diagnostic)
    {
        diagnostic = {};
        diagnostic.candidates = static_cast<int>(m_candidates.size());
        if (m_candidates.empty())
        {
            return -1;
        }
        diagnostic.minimum = std::min_element(m_candidates.begin(), m_candidates.end(),
                                              [](const Candidate &a, const Candidate &b)
                                              {
                                                  return a.error < b.error;
                                              })->error;
        diagnostic.limit = diagnostic.minimum + m_options.absolute + m_options.relative * diagnostic.minimum;
        m_pool.clear();
        for (size_t i = 0; i < m_candidates.size(); ++i)
        {
            auto &candidate = m_candidates[i];
            if (m_options.quality && candidate.error > diagnostic.limit)
            {
                continue;
            }
            candidate.cost = candidate.error + m_records[candidate.index].useCount * m_usePenalty;
            m_legacy.applyPenalty(candidate.cost, candidate.index, tile);
            m_pool.push_back(i);
        }
        diagnostic.qualityCandidates = static_cast<int>(m_pool.size());
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
        return m_candidates.capacity() * sizeof(Candidate) + m_pool.capacity() * sizeof(size_t);
    }
};
} // namespace detail
} // namespace mosaicraft
