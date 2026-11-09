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

NeighborhoodState::NeighborhoodState(const std::vector<ImageRecord> &records, int tilesX, int window, double penalty)
    : m_records(records), m_duplicateCache(records), m_window(window), m_minGap(std::max(50, tilesX)), m_penalty(penalty)
{
}

void NeighborhoodState::applyPenalty(double &score, int libraryIndex, int tile)
{
    const int id = m_records[libraryIndex].id;
    const auto frequency = m_frequency.find(id);
    const int count = frequency != m_frequency.end() ? frequency->second : 0;
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
    const auto previous = m_lastUsedAt.find(id);
    if (previous != m_lastUsedAt.end() && tile - previous->second < m_minGap)
    {
        score += 500.0;
    }
    for (const int recent : m_recentGrids)
    {
        if (m_duplicateCache.similar(libraryIndex, recent, gridThreshold))
        {
            score += gridPenalty;
            break;
        }
    }
}

void NeighborhoodState::recordSelection(int libraryIndex, int tile)
{
    const int id = m_records[libraryIndex].id;
    m_recentIds.push_back(id);
    ++m_frequency[id];
    m_lastUsedAt[id] = tile;
    m_recentGrids.push_back(libraryIndex);
    while (static_cast<int>(m_recentGrids.size()) > gridWindow)
    {
        m_recentGrids.pop_front();
    }
    if (static_cast<int>(m_recentIds.size()) > m_window)
    {
        const int oldId = m_recentIds.front();
        m_recentIds.pop_front();
        if (--m_frequency[oldId] <= 0)
        {
            m_frequency.erase(oldId);
        }
    }
}
} // namespace mosaicraft::detail
