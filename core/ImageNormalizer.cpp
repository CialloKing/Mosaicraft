#include "ImageNormalizer.h"
#include "UnicodeIO.h"
#include "RequestValidation.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace mosaicraft
{

namespace
{
std::string imageFormat(const std::string& path)
{
    auto extension = pathToUtf8(u8path(path).extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    if (extension == ".jpeg")
    {
        return ".jpg";
    }
    if (extension == ".tif")
    {
        return ".tiff";
    }
    return extension;
}
} // namespace

ImageNormalizer::ImageNormalizer(int targetWidth, int targetHeight)
    : m_targetWidth(targetWidth)
    , m_targetHeight(targetHeight)
{
}

bool ImageNormalizer::process(const std::string& inputPath, const std::string& outputPath,
                              const std::vector<int>& writeParams, cv::Mat* storedPixels)
{
    if (storedPixels)
    {
        storedPixels->release();
    }
    try
    {
        // 保存与解码共用同一份字节，源文件随后变化也不会使特征与图库副本错配。
        auto encoded = readBytesUnicode(inputPath);
        cv::Mat src = encoded.empty() ? cv::Mat() : cv::imdecode(encoded, cv::IMREAD_COLOR);
        if (src.empty())
        {
            throw std::runtime_error("cannot decode input");
        }
        if (src.cols == m_targetWidth && src.rows == m_targetHeight && imageFormat(inputPath) == imageFormat(outputPath))
        {
            std::error_code ec;
            const bool sameFile = std::filesystem::equivalent(u8path(inputPath), u8path(outputPath), ec);
            if (!sameFile && !writeBytesUnicode(outputPath, encoded))
            {
                throw std::runtime_error("cannot save original bytes");
            }
            if (storedPixels)
            {
                *storedPixels = std::move(src);
            }
            return true;
        }
        // 不再需要原编码数据，避免它与缩放、编码的临时缓冲同时存活。
        std::vector<uint8_t>().swap(encoded);
        const cv::Mat normalized = normalize(src);
        if (!imwriteUnicode(outputPath, normalized, writeParams))
        {
            throw std::runtime_error("cannot save normalized image");
        }
        if (storedPixels)
        {
            // 有损重编码会改变像素；这里仍以实际落盘图片为特征来源。
            cv::Mat saved = imreadUnicode(outputPath, cv::IMREAD_COLOR);
            if (saved.empty())
            {
                throw std::runtime_error("cannot decode saved image");
            }
            *storedPixels = std::move(saved);
        }
        return true;
    }
    catch (const std::exception& error)
    {
        std::cerr << "ERROR: Cannot normalize " << inputPath << " -> " << outputPath << ": " << error.what() << std::endl;
        return false;
    }
}

cv::Mat ImageNormalizer::processToMat(const std::string& inputPath)
{
    cv::Mat src = imreadUnicode(inputPath, cv::IMREAD_COLOR);
    if (src.empty())
    {
        return {};
    }
    return normalize(src);
}

cv::Mat ImageNormalizer::normalize(const cv::Mat& src)
{
    if (src.cols == m_targetWidth && src.rows == m_targetHeight)
    {
        return src;
    }
    cv::Mat covered = resizeToCover(src);
    return centerCrop(covered);
}

cv::Mat ImageNormalizer::resizeToCover(const cv::Mat& src)
{
    const int srcW = src.cols;
    const int srcH = src.rows;

    // 计算缩放因子：取两轴中较大的那个，确保缩放后两个方向都 ≥ 目标尺寸
    // 例如：1000×500 → 180×320，scale = max(180/1000, 320/500) = 0.64，得 640×320
    // 例如：500×1000 → 180×320，scale = max(180/500, 320/1000) = 0.36，得 180×360
    double scaleW = static_cast<double>(m_targetWidth) / srcW;
    double scaleH = static_cast<double>(m_targetHeight) / srcH;
    double scale = (scaleW > scaleH) ? scaleW : scaleH;

    double width = std::ceil(srcW * scale);
    double height = std::ceil(srcH * scale);
    if (width > 1000000 || height > 1000000 || !validImageSize(static_cast<int64_t>(width), static_cast<int64_t>(height)))
    {
        throw std::runtime_error("normalized cover image exceeds 100 million pixels");
    }
    int newW = static_cast<int>(width);
    int newH = static_cast<int>(height);

    // 保证像素精度：缩放后尺寸可能因取整略小，向上补 1
    if (newW < m_targetWidth)
    {
        newW = m_targetWidth;
    }
    if (newH < m_targetHeight)
    {
        newH = m_targetHeight;
    }

    cv::Mat result;
    // INTER_AREA 对缩小图像质量最好；对放大退化为 INTER_NEAREST（一般不应放大）
    cv::resize(src, result, cv::Size(newW, newH), 0, 0, cv::INTER_AREA);

    return result;
}

cv::Mat ImageNormalizer::centerCrop(const cv::Mat& src)
{
    // 计算裁剪起点：居中偏移
    int x = (src.cols - m_targetWidth) / 2;
    int y = (src.rows - m_targetHeight) / 2;

    // 防止因取整误差导致越界
    if (x < 0)
    {
        x = 0;
    }
    if (y < 0)
    {
        y = 0;
    }

    cv::Rect roi(x, y, m_targetWidth, m_targetHeight);

    // clone() 确保返回连续内存的图像，与原始 Mat 解耦
    return src(roi).clone();
}

} // namespace mosaicraft
