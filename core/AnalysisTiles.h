#pragma once
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mosaicraft
{
struct WorstTile
{
    std::pair<double, int> rank;
    int tile = 0;
    std::array<double, 3> lab{};
    double edge = 0;
    std::array<float, 192> grid{};
};

// 报告只导出最差 20 块；标量排名与详细特征分开，避免整图保留大矩阵。
class WorstTiles
{
    std::vector<WorstTile> m_tiles;

  public:
    template <class Grid>
    void remember(double score, int sample, int tile, double l, double a, double b, double edge, const Grid &grid)
    {
        const std::pair<double, int> rank{score, sample};
        auto destination = m_tiles.end();
        if (m_tiles.size() == 20)
        {
            destination = std::min_element(m_tiles.begin(), m_tiles.end(),
                                           [](const auto &left, const auto &right)
                                           {
                                               return left.rank < right.rank;
                                           });
            if (rank <= destination->rank)
            {
                return;
            }
        }
        else
        {
            m_tiles.emplace_back();
            destination = m_tiles.end() - 1;
        }
        destination->rank = rank;
        destination->tile = tile;
        destination->lab = {l, a, b};
        destination->edge = edge;
        std::copy_n(grid.data(), 192, destination->grid.data());
    }
    const WorstTile &at(int sample) const
    {
        const auto tile = std::find_if(m_tiles.begin(), m_tiles.end(),
                                       [&](const auto &entry)
                                       {
                                           return entry.rank.second == sample;
                                       });
        if (tile == m_tiles.end())
        {
            throw std::out_of_range("missing worst tile snapshot");
        }
        return *tile;
    }
    std::vector<std::pair<double, int>> ranks() const
    {
        std::vector<std::pair<double, int>> result;
        for (const auto &tile : m_tiles)
        {
            result.push_back(tile.rank);
        }
        std::sort(result.rbegin(), result.rend());
        return result;
    }
};
} // namespace mosaicraft
