#pragma once

#include <opencv2/core.hpp>
#include <cstdint>

namespace mosaicraft
{

// 浏览顺序独立于匹配特征；修改算法时仅重算此键，不升级特征版本。
constexpr int kColorOrderVersion = 1;
std::int64_t imageColorOrderKey(const cv::Mat& image);

} // namespace mosaicraft
