#pragma once
#include "FeatureMatrix.h"
#include "FeaturePack.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mosaicraft
{
struct MatchingLimits
{
    size_t hostBytes = 256ULL * 1024 * 1024;
    size_t featureBytes = 256ULL * 1024 * 1024;
    size_t maxTiles = 1024;
};

inline size_t checkedMatchProduct(size_t left, size_t right)
{
    if (right && left > std::numeric_limits<size_t>::max() / right)
    {
        throw std::overflow_error("matching buffer size overflow");
    }
    return left * right;
}

inline size_t matchingBatchRows(const MatchingLimits &limits, int width, int height, int candidates,
                                size_t libraryCount, bool gpu)
{
    if (width <= 0 || height <= 0 || candidates <= 0 || limits.maxTiles == 0)
    {
        throw std::invalid_argument("invalid matching batch limits");
    }
    // RGB、五特征、ANN 查询、权重、候选、评分及候选去重临时数组均计入可控缓冲。
    const size_t rgb = checkedMatchProduct(checkedMatchProduct(width, height), 3);
    const size_t candidateBytes = checkedMatchProduct(static_cast<size_t>(candidates), 32);
    const size_t fixed =
        2080 + 196 * sizeof(float) + 8 * sizeof(double) + 48 * sizeof(float) + sizeof(std::vector<float>);
    if (rgb > std::numeric_limits<size_t>::max() - fixed ||
        candidateBytes > std::numeric_limits<size_t>::max() - rgb - fixed)
    {
        throw std::overflow_error("matching buffer size overflow");
    }
    size_t rows = std::min(limits.maxTiles, std::max<size_t>(1, limits.hostBytes / (rgb + fixed + candidateBytes)));
    if (!gpu && libraryCount > limits.featureBytes / sizeof(FeaturePack::Reader::Row))
    {
        // 大库按批次固定候选缓存；最坏情况下每个 tile 的候选都互不相同。
        rows = std::min(rows, std::max<size_t>(1, limits.featureBytes / sizeof(FeaturePack::Reader::Row) /
                                                      std::min<size_t>(libraryCount, candidates)));
    }
    return rows;
}

struct MatchingBuffers
{
    std::vector<double> l, a, b, edge, lab, labW, gridW, tinyW, edgeW, lbpW, scores;
    FeatureMatrix<float, 192> grid{0};
    FeatureMatrix<uint8_t, 256> tiny{0};
    FeatureMatrix<float, 256> lbp{0};
    std::vector<float> queries;
    std::vector<int> indices;
    std::vector<uint8_t> images;
    std::vector<std::vector<float>> grid4;

    void resize(size_t count, size_t imageBytes, int candidates, bool gpu, bool analyze)
    {
        l.resize(count);
        a.resize(count);
        b.resize(count);
        edge.resize(count);
        grid.resize(count);
        tiny.resize(count);
        lbp.resize(count);
        labW.resize(count);
        gridW.resize(count);
        tinyW.resize(count);
        edgeW.resize(count);
        lbpW.resize(count);
        queries.resize(checkedMatchProduct(count, 196));
        indices.resize(checkedMatchProduct(count, candidates));
        if (gpu)
        {
            images.resize(checkedMatchProduct(std::min<size_t>(count, 256), imageBytes));
            lab.resize(count * 3);
            scores.resize(checkedMatchProduct(count, candidates));
            if (analyze)
            {
                grid4.resize(count);
                for (auto &row : grid4)
                {
                    row.resize(48);
                }
            }
        }
    }
    size_t capacityBytes() const
    {
        size_t result = grid.capacityBytes() + tiny.capacityBytes() + lbp.capacityBytes() + images.capacity();
        for (const auto *values : {&l, &a, &b, &edge, &lab, &labW, &gridW, &tinyW, &edgeW, &lbpW, &scores})
        {
            result += values->capacity() * sizeof(double);
        }
        result += queries.capacity() * sizeof(float) + indices.capacity() * sizeof(int);
        result += grid4.capacity() * sizeof(std::vector<float>);
        for (const auto &row : grid4)
        {
            result += row.capacity() * sizeof(float);
        }
        return result;
    }
};

inline size_t gpuMatchingBufferBytes(size_t rows, size_t imageBytes, int candidates, bool analyze)
{
    const size_t imageStorage = checkedMatchProduct(std::min<size_t>(rows, 256), imageBytes);
    const size_t rowBytes = 2048 + 12 * sizeof(double) + 196 * sizeof(float) +
                            checkedMatchProduct(candidates, sizeof(int) + sizeof(double)) +
                            (analyze ? 48 * sizeof(float) + sizeof(std::vector<float>) : 0);
    const size_t featureStorage = checkedMatchProduct(rows, rowBytes);
    if (imageStorage > std::numeric_limits<size_t>::max() - featureStorage)
    {
        throw std::overflow_error("matching buffer size overflow");
    }
    return imageStorage + featureStorage;
}
} // namespace mosaicraft
