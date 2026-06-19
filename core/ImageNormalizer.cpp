#include "ImageNormalizer.h"
#include "UnicodeIO.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>

namespace mosaicraft
{

ImageNormalizer::ImageNormalizer(int targetWidth, int targetHeight)
    : m_targetWidth(targetWidth)
    , m_targetHeight(targetHeight)
{
}

bool ImageNormalizer::process(const std::string& inputPath, const std::string& outputPath)
{
    // 1. 读取图片
    cv::Mat src = imreadUnicode(inputPath, cv::IMREAD_COLOR);
    if (src.empty())
    {
        std::cerr << "ERROR: Cannot read image: " << inputPath << std::endl;
        return false;
    }

    // 2. 等比缩放至覆盖目标尺寸
    cv::Mat covered = resizeToCover(src);

    // 3. 中心裁剪到精确尺寸
    cv::Mat cropped = centerCrop(covered);

    // 4. 写入输出文件
    if (!imwriteUnicode(outputPath, cropped))
    {
        std::cerr << "ERROR: Cannot write image: " << outputPath << std::endl;
        return false;
    }

    return true;
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

    int newW = static_cast<int>(srcW * scale);
    int newH = static_cast<int>(srcH * scale);

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
    // INTER_AREA 对缩小图像质量最好；对放大则退化为 INTER_LINEAR 行为
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
