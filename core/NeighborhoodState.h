#pragma once
#include "GridDuplicateCache.h"
#include <algorithm>
#include <deque>
#include <vector>

namespace mosaicraft::detail
{
// 保存已算完的前 K 个分数；门槛只能下降，平分必须继续走原来的排序规则。
class RankBound
{
  public:
    explicit RankBound(size_t limit) : m_limit(limit)
    {
        m_best.reserve(limit);
    }
    void reset()
    {
        m_best.clear();
    }
    bool excludes(double score) const
    {
        return m_limit && m_best.size() == m_limit && score > m_best.front();
    }
    void observe(double score)
    {
        if (m_best.size() < m_limit)
        {
            m_best.push_back(score);
            std::push_heap(m_best.begin(), m_best.end());
        }
        else if (m_limit && score < m_best.front())
        {
            std::pop_heap(m_best.begin(), m_best.end());
            m_best.back() = score;
            std::push_heap(m_best.begin(), m_best.end());
        }
    }
  private:
    size_t m_limit;
    std::vector<double> m_best;
};

// 整个任务共用一个窗口；批次切换和无候选的小块不会重置状态。
class NeighborhoodState
{
  public:
    NeighborhoodState(const std::vector<ImageRecord> &records, int tilesX, int window, double penalty,
                      NeighborCounters *counters = nullptr, unsigned slots = 65536, unsigned ways = 1);

    // 只接收已验证的图库索引，不排序、不取随机数，也不改变选图窗口。
    void applyPenalty(double &score, int libraryIndex, int tile, RankBound *bound = nullptr);
    void recordSelection(int libraryIndex, int tile);
    void replay(const std::vector<int> &indices, int tile);

  private:
    GridDuplicateCache m_duplicateCache;
    NeighborCounters *m_counters;
    int m_window;
    int m_minGap;
    double m_penalty;
    std::deque<int> m_recentIndices;
    std::vector<int> m_frequency, m_lastUsedAt;
    std::deque<int> m_recentGrids;
};
} // namespace mosaicraft::detail
