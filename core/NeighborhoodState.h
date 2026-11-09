#pragma once
#include "GridDuplicateCache.h"
#include <deque>
#include <unordered_map>

namespace mosaicraft::detail
{
// 整个任务共用一个窗口；批次切换和无候选的小块不会重置状态。
class NeighborhoodState
{
  public:
    NeighborhoodState(const std::vector<ImageRecord> &records, int tilesX, int window, double penalty);

    // 只接收已验证的图库索引，不排序、不取随机数，也不改变选图窗口。
    void applyPenalty(double &score, int libraryIndex, int tile);
    void recordSelection(int libraryIndex, int tile);

  private:
    const std::vector<ImageRecord> &m_records;
    GridDuplicateCache m_duplicateCache;
    int m_window;
    int m_minGap;
    double m_penalty;
    std::deque<int> m_recentIds;
    std::unordered_map<int, int> m_frequency;
    std::unordered_map<int, int> m_lastUsedAt;
    std::deque<int> m_recentGrids;
};
} // namespace mosaicraft::detail
