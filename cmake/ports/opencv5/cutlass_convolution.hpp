#pragma once

#include <cstddef>
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

struct CropConvScratch
{
    int outputRows = 0;
    std::size_t inputElements = 0, outputElements = 0;
};

CropConvScratch crop_conv_scratch(const CropConv2d &shape);
bool cutlass_crop_conv(const CropConv2d &shape, const CropConvScratch &scratch, const float *inputNCHW,
                       const float *filterKRSC, float *outputNCHW, float *inputTile, float *outputTile,
                       cudaStream_t stream);
bool pointwise_crop_conv(const CropConv2d &shape, const float *inputNCHW, const float *filterKC,
                         float *outputNCHW, cudaStream_t stream);
void direct_crop_conv(const CropConv2d &shape, const float *inputNCHW, const float *filterKRSC,
                      float *outputNCHW, cudaStream_t stream);
}
