#include "NeighborhoodState.h"
#include <algorithm>
#include <array>
#include <cmath>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace mosaicraft::detail
{
namespace
{
constexpr double gridThreshold = 0.010;
constexpr double gridPenalty = 200.0;
constexpr int gridWindow = 50;
} // namespace

int resolveNeighborWindow(int requested, int columns, size_t records)
{
    if (requested > 0)
    {
        return requested;
    }
    // 至少覆盖两行；图库大小只调整 300 到 400 之间的基准。
    const int dynamic = static_cast<int>(std::sqrt(static_cast<double>(records)) * 1.5);
    return std::max({300, columns * 2, std::min(dynamic, 400)});
}

// 最近 50 个成功选图各占一位。只排除不可能相似的格，余下仍交原来的完整距离判断。
struct NeighborhoodState::RecentGridFilter
{
    static_assert(gridWindow <= 64);
    static constexpr std::array<int, 4> components{0, 3 * 27, 3 * 27 + 1, 3 * 63};
    static constexpr double weights[] =
    {
#include "GridWeights.inc"
    };
    static constexpr int binWidth = 16;
    static_assert(weights[0] > gridThreshold * 6400 / (binWidth * binWidth) &&
                  weights[27] > gridThreshold * 6400 / (binWidth * binWidth) &&
                  weights[63] > gridThreshold * 6400 / (binWidth * binWidth));
    const std::vector<ImageRecord> &records;
    std::array<int, gridWindow> indices;
    uint64_t bins[components.size()][18]{};
    unsigned next = 0;

    explicit RecentGridFilter(const std::vector<ImageRecord> &rows) : records(rows)
    {
        indices.fill(-1);
    }

    static int bin(float value)
    {
        // 限幅只会缩短距离。先比较再转换，NaN 或极端 float 均不会触发整数转换溢出。
        return !(value > 0) ? 1 : value >= 255 ? 16 : 1 + static_cast<int>(value) / binWidth;
    }

    void toggle(int index, uint64_t bit)
    {
        const auto &grid = records[index].gridLab;
        if (grid.size() == 192)
        {
            for (size_t k = 0; k < components.size(); ++k)
            {
                bins[k][bin(grid[components[k]])] ^= bit;
            }
        }
    }

    void push(int index)
    {
        const uint64_t bit = uint64_t{1} << next;
        if (indices[next] >= 0)
        {
            toggle(indices[next], bit);
        }
        indices[next] = index;
        toggle(index, bit);
        next = (next + 1) % gridWindow;
    }

    static unsigned firstSlot(uint64_t mask)
    {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
        unsigned long slot;
        _BitScanForward64(&slot, mask);
        return slot;
#elif defined(__GNUC__) || defined(__clang__)
        return static_cast<unsigned>(__builtin_ctzll(mask));
#else
        unsigned slot = 0;
        while (!(mask & 1))
        {
            ++slot;
            mask >>= 1;
        }
        return slot;
#endif
    }

    bool similar(int index, GridDuplicateCache &cache) const
    {
        const auto &grid = records[index].gridLab;
        if (grid.size() != 192)
        {
            return false;
        }
        uint64_t mask = ~uint64_t{0};
        for (size_t k = 0; k < components.size(); ++k)
        {
            // 分隔一个完整桶意味着单通道差至少 16；其一项加权平方已超过整格门槛。
            const int b = bin(grid[components[k]]);
            mask &= bins[k][b - 1] | bins[k][b] | bins[k][b + 1];
        }
        while (mask)
        {
            if (cache.similar(index, indices[firstSlot(mask)], gridThreshold))
            {
                return true;
            }
            mask &= mask - 1;
        }
        return false;
    }
};

NeighborhoodState::~NeighborhoodState() = default;

size_t NeighborhoodState::filterBytes() const
{
    return m_filter ? sizeof(RecentGridFilter) : 0;
}

NeighborhoodState::NeighborhoodState(const std::vector<ImageRecord> &records, int tilesX, int window, double penalty,
                                     NeighborCounters *counters, unsigned slots, unsigned ways, bool filterGrids)
    : m_filter(filterGrids ? std::make_unique<RecentGridFilter>(records) : nullptr),
      m_duplicateCache(records, counters, slots, ways), m_counters(counters), m_window(window), m_minGap(std::max(50, tilesX)), m_penalty(penalty),
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
    const bool similar = m_filter ? m_filter->similar(libraryIndex, m_duplicateCache) :
        std::any_of(m_recentGrids.begin(), m_recentGrids.end(), [&](int recent)
        {
            return m_duplicateCache.similar(libraryIndex, recent, gridThreshold);
        });
    if (similar)
    {
        score += gridPenalty;
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
    if (m_filter)
    {
        m_filter->push(libraryIndex);
    }
    else
    {
        m_recentGrids.push_back(libraryIndex);
        if (static_cast<int>(m_recentGrids.size()) > gridWindow)
        {
            m_recentGrids.pop_front();
        }
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
