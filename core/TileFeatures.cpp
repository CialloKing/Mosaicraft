#include "TileFeatures.h"
#include "FeatureUtils.h"
#include <opencv2/core/hal/intrin.hpp>
#include <array>

namespace mosaicraft::detail
{
cv::Scalar gridAndMean(const cv::Mat &lab, float *grid)
{
    if (lab.cols < 8 || lab.rows < 8)
    {
        std::fill_n(grid, 192, 0.0f);
        return cv::mean(lab);
    }
    const int cw = (lab.cols + 7) / 8, ch = (lab.rows + 7) / 8;
    cv::Scalar total;
    for (int r = 0; r < 8; ++r)
    {
        for (int c = 0; c < 8; ++c)
        {
            const int w = std::min(cw, lab.cols - cw * c), h = std::min(ch, lab.rows - ch * r);
            cv::Scalar sum;
            double scale = 0;
            if (w > 0 && h > 0)
            {
                sum = cv::sum(lab(cv::Rect(c * cw, r * ch, w, h)));
                scale = 1.0 / (w * h);
            }
            // 8 位像素的整数和在 double 中精确；先合并再归一化，不能平均各格的均值。
            total += sum;
            for (int k = 0; k < 3; ++k)
            {
                *grid++ = static_cast<float>(sum[k] * scale);
            }
        }
    }
    return total * (1.0 / lab.total());
}

cv::Scalar gridAndMeanByColumns(const cv::Mat &lab, float *grid)
{
    // 每列只跨一个 Grid 行累加。8 位 LAB 的列和在 257 行以内可精确装入 16 位；
    // 横向再按原 ceil 边界汇总，避免为 64 个小 ROI 分别调用通用 sum。
    constexpr int maxWidth = 512;
    const int ch = (lab.rows + 7) / 8, cw = (lab.cols + 7) / 8;
    if (lab.cols < 8 || lab.rows < 8 || lab.cols > maxWidth || ch > 257)
    {
        return gridAndMean(lab, grid);
    }
    std::array<uint16_t, maxWidth * 3> columns;
    cv::Scalar total;
    for (int r = 0; r < 8; ++r)
    {
        const int first = r * ch, end = std::min(first + ch, lab.rows);
        std::fill_n(columns.data(), lab.cols * 3, uint16_t{0});
        for (int y = first; y < end; ++y)
        {
            const auto *row = lab.ptr<uint8_t>(y);
            for (int i = 0; i < lab.cols * 3; ++i)
            {
                columns[i] += row[i];
            }
        }
        for (int c = 0; c < 8; ++c)
        {
            const int x0 = c * cw, x1 = std::min(x0 + cw, lab.cols);
            unsigned sums[3]{};
            for (int x = x0; x < x1; ++x)
            {
                for (int k = 0; k < 3; ++k)
                {
                    sums[k] += columns[x * 3 + k];
                }
            }
            const double scale = end > first && x1 > x0 ? 1.0 / ((end - first) * (x1 - x0)) : 0.0;
            for (int k = 0; k < 3; ++k)
            {
                total[k] += sums[k];
                *grid++ = static_cast<float>(sums[k] * scale);
            }
        }
    }
    return total * (1.0 / lab.total());
}

void lbpInto(const cv::Mat &gray, float *hist, bool vectorize)
{
    if (gray.cols < 3 || gray.rows < 3)
    {
        std::fill_n(hist, 256, 0.0f);
        return;
    }
    std::array<uint32_t, 256> counts{};
    for (int y = 1; y + 1 < gray.rows; ++y)
    {
        const uint8_t *a = gray.ptr(y - 1), *b = gray.ptr(y), *c = gray.ptr(y + 1);
        const uint8_t *neighbors[] =
        {
            a, a + 1, a + 2, b + 2, c + 2, c + 1, c, b
        };
        int x = 1;
#if CV_SIMD128
        if (vectorize)
        {
            for (; x + 16 < gray.cols; x += 16)
            {
                const auto center = cv::v_load(b + x);
                auto code = cv::v_setzero_u8();
                for (int k = 0; k < 8; ++k)
                {
                    const auto neighbor = cv::v_load(neighbors[k] + x - 1);
                    const auto bit = cv::v_setall_u8(static_cast<uint8_t>(1u << k));
                    // OpenCV 4.11 将向量运算符改为具名接口；两种写法产生相同的无符号比较掩码。
#if CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR < 11
                    code = code | ((neighbor >= center) & bit);
#else
                    code = cv::v_or(code, cv::v_and(cv::v_ge(neighbor, center), bit));
#endif
                }
                uint8_t values[16];
                cv::v_store(values, code);
                for (uint8_t value : values)
                {
                    ++counts[value];
                }
            }
        }
#else
        (void)vectorize;
#endif
        for (; x + 1 < gray.cols; ++x)
        {
            unsigned code = 0;
            for (int k = 0; k < 8; ++k)
            {
                code |= unsigned(neighbors[k][x - 1] >= b[x]) << k;
            }
            ++counts[code];
        }
    }
    std::array<float, 256> values;
    std::transform(counts.begin(), counts.end(), values.begin(), compatibleLbpCount);
    normalizeLbpHistogram(values);
    std::copy(values.begin(), values.end(), hist);
}
}
