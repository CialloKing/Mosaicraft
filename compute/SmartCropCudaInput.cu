#include "SmartCropCudaInput.h"

namespace mosaicraft::cuda
{
namespace
{
__global__ void packInput(const std::uint8_t *bgr, float *rgb, SmartCropInputLayout layout, SmartCropPixelValues lookup)
{
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    const int plane = layout.paddedWidth * layout.paddedHeight;
    if (index >= plane)
    {
        return;
    }
    const int x = index % layout.paddedWidth - layout.left;
    const int y = index / layout.paddedWidth - layout.top;
    if (x < 0 || y < 0 || x >= layout.width || y >= layout.height)
    {
        // Match the detector's padding before the BGR-to-RGB conversion.
        rgb[index] = rgb[plane + index] = rgb[2 * plane + index] = lookup.values[114];
        return;
    }
    const auto *pixel = bgr + 3 * (y * layout.width + x);
    rgb[index] = lookup.values[pixel[2]];
    rgb[plane + index] = lookup.values[pixel[1]];
    rgb[2 * plane + index] = lookup.values[pixel[0]];
}
}

cudaError_t launchSmartCropCudaInput(const std::uint8_t *bgr, float *rgb, SmartCropInputLayout layout,
                                     const SmartCropPixelValues &lookup, cudaStream_t stream)
{
    constexpr int blockSize = 256;
    const int pixels = layout.paddedWidth * layout.paddedHeight;
    packInput<<<(pixels + blockSize - 1) / blockSize, blockSize, 0, stream>>>(bgr, rgb, layout, lookup);
    return cudaGetLastError();
}
}
