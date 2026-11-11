#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <opencv2/imgproc.hpp>
#include <stdexcept>

namespace mosaicraft
{

// 只持有预览和行缓冲；所有坐标来自整图，输入条带的边界不影响面积权重。
class PreviewAccumulator
{
public:
    static constexpr int kMaxWidth = 1400;
    static constexpr int64_t kMaxPixels = 4000000;
    static constexpr int kJpegLimit = 65500;
    static constexpr int kJpegQuality = 92;

    static cv::Size outputSize(cv::Size source)
    {
        if (source.width <= 0 || source.height <= 0)
        {
            throw std::invalid_argument("preview source dimensions must be positive");
        }
        const double pixels = static_cast<double>(source.width) * source.height;
        const double scale = std::min({1.0, static_cast<double>(kMaxWidth) / source.width,
                                       static_cast<double>(kJpegLimit) / source.height,
                                       std::sqrt(kMaxPixels / pixels)});
        return {std::max(1, static_cast<int>(std::floor(source.width * scale))),
                std::max(1, static_cast<int>(std::floor(source.height * scale)))};
    }

    explicit PreviewAccumulator(cv::Size source) : m_source(source), m_image(outputSize(source), CV_8UC3)
    {
        m_sum = cv::Mat::zeros(1, m_image.cols, CV_64FC3);
    }

    void append(const cv::Mat &rows, int firstRow)
    {
        if (rows.empty() || rows.type() != CV_8UC3 || rows.cols != m_source.width || firstRow != m_rowsRead ||
            rows.rows > m_source.height - m_rowsRead)
        {
            throw std::invalid_argument("preview expects consecutive full-width three-channel rows");
        }
        for (int y = 0; y < rows.rows; ++y)
        {
            rows.row(y).convertTo(m_floatRow, CV_32FC3);
            cv::resize(m_floatRow, m_reducedRow, cv::Size(m_image.cols, 1), 0, 0, cv::INTER_AREA);
            const float *values = m_reducedRow.ptr<float>();
            double *sum = m_sum.ptr<double>();
            const int channels = m_image.cols * 3;
            // 以预览高度为分母使用整数坐标，避免累计浮点误差造成漏行或接缝。
            int64_t begin = static_cast<int64_t>(m_rowsRead) * m_image.rows;
            const int64_t end = static_cast<int64_t>(m_rowsRead + 1) * m_image.rows;
            while (begin < end)
            {
                const int64_t boundary = static_cast<int64_t>(m_outputRow + 1) * m_source.height;
                const int64_t next = std::min(end, boundary);
                const double weight = static_cast<double>(next - begin);
                for (int x = 0; x < channels; ++x)
                {
                    sum[x] += values[x] * weight;
                }
                if (next == boundary)
                {
                    auto *destination = m_image.ptr<uint8_t>(m_outputRow++);
                    for (int x = 0; x < channels; ++x)
                    {
                        destination[x] = cv::saturate_cast<uint8_t>(sum[x] / m_source.height);
                        sum[x] = 0;
                    }
                }
                begin = next;
            }
            ++m_rowsRead;
        }
    }

    const cv::Mat &image() const
    {
        if (m_rowsRead != m_source.height || m_outputRow != m_image.rows)
        {
            throw std::logic_error("preview image is incomplete");
        }
        return m_image;
    }

private:
    cv::Size m_source;
    cv::Mat m_image, m_floatRow, m_reducedRow, m_sum;
    int m_rowsRead = 0;
    int m_outputRow = 0;
};

} // namespace mosaicraft
