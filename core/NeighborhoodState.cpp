#include "NeighborhoodState.h"
#include <algorithm>

namespace mosaicraft::detail
{
namespace
{
constexpr double gridThreshold = 0.010;
constexpr double gridPenalty = 200.0;
constexpr int gridWindow = 50;
} // namespace

NeighborhoodState::NeighborhoodState(const std::vector<ImageRecord> &records, int tilesX, int window, double penalty,
                                     NeighborCounters *counters, unsigned slots, unsigned ways)
    : m_duplicateCache(records, counters, slots, ways), m_counters(counters), m_window(window), m_minGap(std::max(50, tilesX)), m_penalty(penalty),
      m_frequency(records.size()), m_lastUsedAt(records.size(), -1)
{
}

void NeighborhoodState::applyPenalty(double &score, int libraryIndex, int tile, RankBound *bound)
{
    if (m_counters)
    {
        ++m_counters->candidates;
    }
    const int count = m_frequency[libraryIndex];
    // 逐项加到原分数上；先合并惩罚再相加会改变浮点舍入和平分结果。
    if (count >= 3)
    {
        score += m_penalty;
    }
    else if (count == 2)
    {
        score += m_penalty * 0.4;
    }
    else if (count == 1)
    {
        score += m_penalty * 0.1;
    }
    const int previous = m_lastUsedAt[libraryIndex];
    if (previous >= 0 && tile - previous < m_minGap)
    {
        score += 500.0;
    }
    // 余下的 Grid 惩罚只会增加分数；保留候选的位置，避免改变旧路径的平分排序。
    if (bound && bound->excludes(score))
    {
        if (m_counters)
        {
            ++m_counters->pruned;
        }
        return;
    }
    for (const int recent : m_recentGrids)
    {
        if (m_duplicateCache.similar(libraryIndex, recent, gridThreshold))
        {
            score += gridPenalty;
            break;
        }
    }
    if (bound)
    {
        bound->observe(score);
    }
}

void NeighborhoodState::recordSelection(int libraryIndex, int tile)
{
    // 下标只在当前图库快照内有效；不依赖持久化 ID 连续或从零开始。
    m_recentIndices.push_back(libraryIndex);
    ++m_frequency[libraryIndex];
    m_lastUsedAt[libraryIndex] = tile;
    m_recentGrids.push_back(libraryIndex);
    while (static_cast<int>(m_recentGrids.size()) > gridWindow)
    {
        m_recentGrids.pop_front();
    }
    if (static_cast<int>(m_recentIndices.size()) > m_window)
    {
        --m_frequency[m_recentIndices.front()];
        m_recentIndices.pop_front();
    }
}

void NeighborhoodState::replay(const std::vector<int> &indices, int tile)
{
    // 频次和相似度窗口按成功选图计数，最小间距按实际格子位置计数。
    int first = tile;
    int selected = 0;
    const int recent = std::max(m_window, gridWindow);
    while (first > 0 && (selected < recent || tile - first < m_minGap - 1))
    {
        selected += indices[--first] >= 0;
    }
    for (int position = first; position < tile; ++position)
    {
        if (indices[position] >= 0)
        {
            recordSelection(indices[position], position);
        }
    }
}
} // namespace mosaicraft::detail
