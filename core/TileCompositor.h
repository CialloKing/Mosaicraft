#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <vector>

namespace mosaicraft
{

// 只读参考图由输出任务持有；每次采样只分配一个输出小块，可供工作线程并行调用。
class TileCompositor
{
  public:
    TileCompositor(const cv::Mat &target, int tilesX, int tilesY, cv::Size tileSize)
        : m_target(target), m_tilesX(tilesX), m_tilesY(tilesY), m_tileSize(tileSize)
    {
        if (target.empty() || target.type() != CV_8UC3 || tilesX <= 0 || tilesY <= 0 ||
            tileSize.width <= 0 || tileSize.height <= 0 || target.cols % tilesX != 0 || target.rows % tilesY != 0)
        {
            throw std::invalid_argument("overlay requires a padded BGR target matching the tile grid");
        }
        m_scaleX = target.cols / (static_cast<double>(tilesX) * tileSize.width);
        m_scaleY = target.rows / (static_cast<double>(tilesY) * tileSize.height);
    }

    static uint64_t workingBytes(cv::Size tileSize)
    {
        // 一份素材副本、一份参考小块和水平插值表；不随完整输出面积增长。
        return static_cast<uint64_t>(tileSize.width) * tileSize.height * 6 +
               static_cast<uint64_t>(tileSize.width) * sizeof(Sample);
    }

    void sample(size_t index, cv::Mat &reference) const
    {
        if (index >= static_cast<size_t>(m_tilesX) * m_tilesY)
        {
            throw std::out_of_range("overlay tile index exceeds grid");
        }
        reference.create(m_tileSize, CV_8UC3);
        std::vector<Sample> horizontal(m_tileSize.width);
        const double left = static_cast<double>(index % m_tilesX) * m_tileSize.width;
        const double top = static_cast<double>(index / m_tilesX) * m_tileSize.height;
        for (int x = 0; x < m_tileSize.width; ++x)
        {
            horizontal[x] = axis((left + x + 0.5) * m_scaleX - 0.5, m_target.cols);
        }
        for (int y = 0; y < m_tileSize.height; ++y)
        {
            const auto vertical = axis((top + y + 0.5) * m_scaleY - 0.5, m_target.rows);
            const auto *upper = m_target.ptr<cv::Vec3b>(vertical.first);
            const auto *lower = m_target.ptr<cv::Vec3b>(vertical.second);
            auto *output = reference.ptr<cv::Vec3b>(y);
            for (int x = 0; x < m_tileSize.width; ++x)
            {
                const auto &h = horizontal[x];
                for (int c = 0; c < 3; ++c)
                {
                    const double a = upper[h.first][c] * (1.0 - h.weight) + upper[h.second][c] * h.weight;
                    const double b = lower[h.first][c] * (1.0 - h.weight) + lower[h.second][c] * h.weight;
                    output[x][c] = cv::saturate_cast<uchar>(a * (1.0 - vertical.weight) + b * vertical.weight);
                }
            }
        }
    }

  private:
    struct Sample
    {
        int first, second;
        double weight;
    };

    static Sample axis(double coordinate, int size)
    {
        // 按全图像素中心映射，跨小块读取邻居；只有参考图外边界夹紧，避免块间接缝。
        const double clipped = std::clamp(coordinate, 0.0, static_cast<double>(size - 1));
        const int first = static_cast<int>(std::floor(clipped));
        return {first, std::min(first + 1, size - 1), clipped - first};
    }

    const cv::Mat &m_target;
    int m_tilesX, m_tilesY;
    cv::Size m_tileSize;
    double m_scaleX, m_scaleY;
};

} // namespace mosaicraft
