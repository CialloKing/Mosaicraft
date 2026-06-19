#pragma once

#include <opencv2/core.hpp>
#include <string>

namespace mosaicraft
{

// ImageNormalizer — 将任意尺寸/格式的图片归一化为统一规格（默认 180×320）。
// 算法：等比缩放至覆盖目标尺寸 → 中心裁剪，保证无拉伸、无黑边。
class ImageNormalizer
{
public:
    // 默认目标尺寸与 Mosaicraft 图库规格一致
    explicit ImageNormalizer(int targetWidth = 180, int targetHeight = 320);

    // 处理单张图片：读取 → 缩放 → 裁剪 → 写入
    // 成功返回 true，失败返回 false（并输出错误信息到 stderr）
    bool process(const std::string& inputPath, const std::string& outputPath);

private:
    int m_targetWidth;
    int m_targetHeight;

    // 等比缩放：使图像刚好覆盖目标尺寸（宽和高都 ≥ 目标值）
    cv::Mat resizeToCover(const cv::Mat& src);

    // 中心裁剪到精确的目标尺寸
    cv::Mat centerCrop(const cv::Mat& src);
};

} // namespace mosaicraft
