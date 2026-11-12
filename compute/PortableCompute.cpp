#include "PortableCompute.h"
#include "core/FeatureExtractor.h"
#include "core/FeatureUtils.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace mosaicraft::compute
{
namespace
{
constexpr size_t fields = 451, stride = 518;
void put(uint32_t *to, float value)
{
    std::memcpy(to, &value, sizeof(value));
}
template <class Grid, class Tiny, class Lbp>
void pack(uint32_t *to, double l, double a, double b, double edge, const Grid &grid, const Tiny &tiny, const Lbp &lbp,
          bool tinyValid, bool lbpValid)
{
    put(to, static_cast<float>(l));
    put(to + 1, static_cast<float>(a));
    put(to + 2, static_cast<float>(b));
    put(to + 3, static_cast<float>(edge));
    for (size_t i = 0; i < 192; ++i)
    {
        put(to + 4 + i, grid[i]);
    }
    for (size_t i = 0; i < 64; ++i)
    {
        uint32_t value = 0;
        for (size_t j = 0; j < 4; ++j)
        {
            value |= uint32_t(tiny[i * 4 + j]) << (j * 8);
        }
        to[196 + i] = value;
    }
    for (size_t i = 0; i < 256; ++i)
    {
        put(to + 260 + i, lbp[i]);
    }
    to[516] = tinyValid;
    to[517] = lbpValid;
}
} // namespace

std::array<float, 5> fp32Distances(const MatchingBuffers &tiles, size_t tile, const ImageRecord &record,
                                   const FeaturePack::Reader::Row &payload)
{
    constexpr double weights[64] = {
#include "core/GridWeights.inc"
    };
    const float dl = float(tiles.l[tile]) - float(record.avgL);
    const float da = float(tiles.a[tile]) - float(record.avgA);
    const float db = float(tiles.b[tile]) - float(record.avgB);
    std::array<float, 5> result{std::sqrt(dl * dl + da * da + db * db) / 100.0f, 0, 0,
                                std::abs(float(tiles.edge[tile]) - float(record.edgeDensity)), 0};
    for (size_t cell = 0; cell < 64; ++cell)
    {
        const size_t offset = cell * 3;
        const float l = tiles.grid[tile][offset] - record.gridLab[offset];
        const float a = tiles.grid[tile][offset + 1] - record.gridLab[offset + 1];
        const float b = tiles.grid[tile][offset + 2] - record.gridLab[offset + 2];
        result[1] += std::sqrt(l * l + a * a + b * b) * float(weights[cell]);
    }
    result[1] = result[1] / 64.0f / 100.0f;
    for (size_t bin = 0; bin < 256; ++bin)
    {
        const float diff = float(tiles.tiny[tile][bin]) - float(payload.tiny[bin]);
        result[2] += diff * diff;
        result[4] += std::abs(tiles.lbp[tile][bin] - payload.lbp[bin]);
    }
    result[2] = payload.tinyValid ? result[2] / (256.0f * 255.0f * 255.0f) : 1.0f;
    result[4] = payload.lbpValid ? result[4] / 2.0f : 1.0f;
    return result;
}

bool FeatureWorkspace::prepare(int w, int h, int maximum, size_t hostBudget, size_t deviceBudget)
{
    session.reset();
    rows = 0;
    host = storage = 0;
    if (w < 1 || h < 1 || w > 8192 || h > 8192 || maximum < 1)
    {
        throw std::invalid_argument("invalid portable feature dimensions");
    }
    const size_t pixels = size_t(w) * h;
    const size_t partial = ((pixels + 4095) / 4096) * fields * sizeof(uint32_t);
    const size_t deviceRow = pixels * 4 + partial;
    // 包含调用方 BGR、LAB/gray、打包像素、统计下载和 OpenCV 可见临时图。
    const size_t hostRow = pixels * 16 + partial + sizeof(Feature);
    rows = static_cast<int>(std::min({size_t(maximum), size_t(256), hostBudget / hostRow, deviceBudget / deviceRow}));
    if (!rows)
    {
        return false;
    }
    try
    {
        session = open(device);
        session->buffer(0, size_t(rows) * pixels * 4);
        session->buffer(1, size_t(rows) * partial);
    }
    catch (const std::exception &)
    {
        session.reset();
        rows = 0;
        return false;
    }
    width = w;
    height = h;
    blocks = static_cast<int>((pixels + 4095) / 4096);
    host = size_t(rows) * hostRow;
    storage = size_t(rows) * deviceRow;
    return true;
}

void FeatureWorkspace::extract(const std::vector<cv::Mat> &images, std::vector<Feature> &output, WorkerPool &workers,
                               const std::vector<ImageRecord *> &statistics)
{
    output.clear();
    if (!session || images.size() > size_t(rows) || (!statistics.empty() && statistics.size() != images.size()))
    {
        throw std::invalid_argument("portable feature batch is not prepared");
    }
    if (images.empty())
    {
        return;
    }
    const size_t pixels = size_t(width) * height;
    std::vector<uint32_t> packed(images.size() * pixels);
    std::vector<Feature> next(images.size());
    workers.run(images.size(),
                [&](size_t i)
                {
                    if (images[i].type() != CV_8UC3 || images[i].cols != width || images[i].rows != height)
                    {
                        throw std::invalid_argument("portable feature inputs must be equally sized BGR images");
                    }
                    cv::Mat lab, gray;
                    cv::cvtColor(images[i], lab, cv::COLOR_BGR2Lab);
                    cv::cvtColor(images[i], gray, cv::COLOR_BGR2GRAY);
                    for (int y = 0; y < height; ++y)
                    {
                        const auto *color = lab.ptr<uint8_t>(y), *mono = gray.ptr<uint8_t>(y);
                        for (int x = 0; x < width; ++x)
                        {
                            packed[i * pixels + size_t(y) * width + x] =
                                uint32_t(color[x * 3]) | (uint32_t(color[x * 3 + 1]) << 8) |
                                (uint32_t(color[x * 3 + 2]) << 16) | (uint32_t(mono[x]) << 24);
                        }
                    }
                    cv::Mat tiny(16, 16, CV_8UC1, next[i].tiny.data());
                    cv::resize(gray, tiny, tiny.size(), 0, 0, cv::INTER_AREA);
                    if (statistics.empty())
                    {
                        next[i].edge = computeEdgeDensityFromGray(gray);
                    }
                    else
                    {
                        FeatureExtractor::computeStatistics(images[i], *statistics[i], lab, gray);
                        next[i].edge = statistics[i]->edgeDensity;
                    }
                });
    std::vector<uint32_t> partials(images.size() * size_t(blocks) * fields);
    session->buffer(0, packed.size() * sizeof(uint32_t), packed.data());
    session->dispatch(false, {uint32_t(width), uint32_t(height), uint32_t(blocks), uint32_t(images.size())},
                      uint32_t(blocks), uint32_t(images.size()));
    session->read(1, partials.size() * sizeof(uint32_t), partials.data());
    for (size_t i = 0; i < images.size(); ++i)
    {
        std::array<uint64_t, fields> sums{};
        for (int block = 0; block < blocks; ++block)
        {
            const auto *values = partials.data() + (i * blocks + block) * fields;
            for (size_t field = 0; field < fields; ++field)
            {
                sums[field] += values[field];
            }
        }
        auto &feature = next[i];
        for (int channel = 0; channel < 3; ++channel)
        {
            feature.lab[channel] = static_cast<double>(sums[channel]) * (1.0 / pixels);
        }
        const int cw = (width + 7) / 8, ch = (height + 7) / 8;
        for (int cell = 0; cell < 64; ++cell)
        {
            const int w = std::min(cw, width - cell % 8 * cw), h = std::min(ch, height - cell / 8 * ch);
            const double scale = width >= 8 && height >= 8 && w > 0 && h > 0 ? 1.0 / (w * h) : 0;
            for (int channel = 0; channel < 3; ++channel)
            {
                feature.grid[cell * 3 + channel] = static_cast<float>(double(sums[3 + cell * 3 + channel]) * scale);
            }
        }
        for (int bin = 0; bin < 256; ++bin)
        {
            feature.lbp[bin] = compatibleLbpCount(static_cast<uint32_t>(sums[195 + bin]));
        }
        normalizeLbpHistogram(feature.lbp);
    }
    output.swap(next);
}

Timings FeatureWorkspace::timings() const
{
    return session ? session->timings : Timings{};
}

bool ScoreWorkspace::prepare(int maximum, int count, size_t libraryCount, size_t hostBudget, size_t deviceBudget)
{
    session.reset();
    records = nullptr;
    rows = 0;
    if (maximum < 1 || count < 1 || libraryCount < 1 || libraryCount > UINT32_MAX / stride)
    {
        throw std::invalid_argument("invalid portable score dimensions");
    }
    library = checkedMatchProduct(libraryCount, stride * sizeof(uint32_t));
    const size_t perRow = stride * 4 + checkedMatchProduct(size_t(count), sizeof(int) + 5 * sizeof(float));
    rows = static_cast<int>(std::min(
        {size_t(maximum), size_t(256), deviceBudget / perRow, hostBudget / (perRow + size_t(count) * sizeof(double))}));
    uploadRows = std::min({libraryCount, size_t(4096), hostBudget / (stride * 4)});
    if (!rows || !uploadRows || library > device.maxBufferBytes)
    {
        rows = 0;
        return false;
    }
    try
    {
        session = open(device);
        session->buffer(0, library);
        session->buffer(1, size_t(rows) * stride * 4);
        session->buffer(2, size_t(rows) * count * sizeof(int));
        session->buffer(3, size_t(rows) * count * 5 * sizeof(float));
    }
    catch (const std::exception &)
    {
        session.reset();
        rows = 0;
        return false;
    }
    candidates = count;
    temporary = size_t(rows) * perRow;
    return true;
}

void ScoreWorkspace::uploadLibrary(const std::vector<ImageRecord> &images, FeaturePack::Reader &reader)
{
    if (!session || images.size() * stride * 4 != library)
    {
        throw std::invalid_argument("portable score library size mismatch");
    }
    std::vector<uint32_t> packed(uploadRows * stride);
    for (size_t first = 0; first < images.size(); first += uploadRows)
    {
        const size_t count = std::min(uploadRows, images.size() - first);
        for (size_t row = 0; row < count; ++row)
        {
            const auto &r = images[first + row];
            const auto &f = reader.get(static_cast<int>(first + row));
            pack(packed.data() + row * stride, r.avgL, r.avgA, r.avgB, r.edgeDensity, r.gridLab, f.tiny, f.lbp,
                 f.tinyValid, f.lbpValid);
        }
        session->write(0, first * stride * 4, count * stride * 4, packed.data());
    }
    records = &images;
}

void ScoreWorkspace::score(const MatchingBuffers &tiles, int count, double penalty, double *scores)
{
    if (!session || !records || count < 1 || count > rows)
    {
        throw std::invalid_argument("portable scoring batch is not prepared");
    }
    std::vector<uint32_t> packed(size_t(count) * stride);
    for (int i = 0; i < count; ++i)
    {
        pack(packed.data() + size_t(i) * stride, tiles.l[i], tiles.a[i], tiles.b[i], tiles.edge[i], tiles.grid[i],
             tiles.tiny[i], tiles.lbp[i], true, true);
    }
    const size_t entries = size_t(count) * candidates;
    std::vector<float> distances(entries * 5);
    session->buffer(1, packed.size() * 4, packed.data());
    session->buffer(2, entries * sizeof(int), tiles.indices.data());
    session->dispatch(true, {uint32_t(count), uint32_t(candidates), uint32_t(records->size()), 0},
                      uint32_t((entries + 127) / 128));
    session->read(3, distances.size() * sizeof(float), distances.data());
    for (size_t position = 0; position < entries; ++position)
    {
        const int index = tiles.indices[position];
        if (index < 0 || size_t(index) >= records->size())
        {
            scores[position] = std::numeric_limits<double>::max();
            continue;
        }
        const size_t tile = position / candidates;
        const auto *d = distances.data() + position * 5;
        // 权重及历史使用惩罚保持 FP64，不让很大的 useCount 吞没距离差异。
        scores[position] = tiles.labW[tile] * d[0] + tiles.gridW[tile] * d[1] + tiles.tinyW[tile] * d[2] +
                           tiles.edgeW[tile] * d[3] + tiles.lbpW[tile] * d[4] + (*records)[index].useCount * penalty;
        if (!std::isfinite(scores[position]))
        {
            throw std::runtime_error("non-finite portable GPU score");
        }
    }
}

Timings ScoreWorkspace::timings() const
{
    return session ? session->timings : Timings{};
}
} // namespace mosaicraft::compute
