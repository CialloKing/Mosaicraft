#pragma once

#include <opencv2/core.hpp>

namespace mosaicraft::detail
{
// 两种实现使用相同的像素、分母和 LBP 位序；实验只改变遍历和暂存方式。
cv::Scalar gridAndMean(const cv::Mat &lab, float *grid);
void lbpInto(const cv::Mat &gray, float *hist, bool vectorize);
}
