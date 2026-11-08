#pragma once
#include "CudaBackend.h"
#include <cuda_runtime.h>
#include <stdexcept>

namespace mosaicraft
{
namespace cuda
{
// Only preparation/compatibility failures may select CPU. A previous asynchronous
// execution failure must remain an error, even when discovered by a query API.
inline bool checkPreflight(cudaError_t error, const char *stage, CudaAvailability &status)
{
    if (error == cudaSuccess)
    {
        return true;
    }
    // Error-name/last-error APIs can themselves initialize the Windows runtime.
    // In particular, do not call them after enumeration reports no driver.
    // Preserve the numeric code without clearing any earlier execution failure.
    const char *detail = nullptr;
    switch (error)
    {
    case cudaErrorNoDevice:
        detail = "no CUDA device";
        break;
    case cudaErrorInsufficientDriver:
    case cudaErrorInitializationError:
    case cudaErrorSystemDriverMismatch:
    case cudaErrorSystemNotReady:
    case cudaErrorCompatNotSupportedOnDevice:
    case cudaErrorCallRequiresNewerDriver:
        detail = "CUDA driver unavailable or incompatible";
        break;
    case cudaErrorInvalidDeviceFunction:
    case cudaErrorNoKernelImageForDevice:
    case cudaErrorUnsupportedPtxVersion:
    case cudaErrorInvalidPtx:
    case cudaErrorJitCompilerNotFound:
        detail = "no compatible CUDA kernel or PTX image";
        break;
    case cudaErrorMemoryAllocation:
        detail = "CUDA preflight allocation failed";
        break;
    default:
        throw std::runtime_error(std::string("CUDA preflight: ") + stage + ": execution/unexpected error " +
                                 std::to_string(static_cast<int>(error)));
    }
    status.reason = std::string(stage) + ": " + detail + " (CUDA error " +
                    std::to_string(static_cast<int>(error)) + ")";
    return false;
}

template <class Kernel>
bool checkKernel(Kernel kernel, int threads, const cudaDeviceProp &device,
                 CudaAvailability &status, const char *name)
{
    cudaFuncAttributes attributes{};
    if (!checkPreflight(cudaFuncGetAttributes(&attributes, kernel), name, status))
    {
        return false;
    }
    if (threads > attributes.maxThreadsPerBlock || threads > device.maxThreadsPerBlock ||
        threads > device.maxThreadsDim[0] || attributes.sharedSizeBytes > device.sharedMemPerBlock)
    {
        status.reason = std::string(name) + ": launch configuration exceeds device/kernel limits";
        return false;
    }
    return true;
}

bool probeTemplateFeatures(const cudaDeviceProp &device, CudaAvailability &status);
bool probeCompatibleFeatures(const cudaDeviceProp &device, CudaAvailability &status);
bool probeCompatibleScoring(const cudaDeviceProp &device, CudaAvailability &status);
} // namespace cuda
} // namespace mosaicraft
