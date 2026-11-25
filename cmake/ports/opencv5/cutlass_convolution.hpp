#pragma once

#include <cuda_runtime_api.h>

namespace cv::dnn::cuda4dnn::kernels
{
struct CropConv2d
{
    int batches, inH, inW, inC, outH, outW, outC;
    int kernel, groups;
    int top, bottom, left, right;
    int strideY, strideX, dilationY, dilationX;
};

bool cutlass_crop_conv(const CropConv2d &shape, const float *inputNHWC, const float *filterKRSC,
                       float *outputNHWC, cudaStream_t stream);
bool pointwise_crop_conv(const CropConv2d &shape, const float *inputNCHW, const float *filterKC,
                         float *outputNCHW, cudaStream_t stream);
void direct_crop_conv(const CropConv2d &shape, const float *inputNCHW, const float *filterKRSC,
                      float *outputNCHW, cudaStream_t stream);
}
