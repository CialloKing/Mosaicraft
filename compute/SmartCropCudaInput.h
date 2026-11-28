#pragma once

#include <cuda_runtime_api.h>
#include <cstdint>

namespace mosaicraft::cuda
{
struct SmartCropInputLayout
{
    int width, height, paddedWidth, paddedHeight, left, top;
};

struct SmartCropPixelValues
{
    float values[256];
};

cudaError_t launchSmartCropCudaInput(const std::uint8_t *bgr, float *rgb, SmartCropInputLayout layout,
                                     const SmartCropPixelValues &lookup, cudaStream_t stream);
}
