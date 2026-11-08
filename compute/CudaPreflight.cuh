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
    status.reason = std::string(stage) + ": " + cudaGetErrorName(error) + " (" + cudaGetErrorString(error) + ")";
    switch (error)
    {
    case cudaErrorNoDevice:
    case cudaErrorInsufficientDriver:
    case cudaErrorInitializationError:
    case cudaErrorSystemDriverMismatch:
    case cudaErrorSystemNotReady:
    case cudaErrorCompatNotSupportedOnDevice:
    case cudaErrorInvalidDeviceFunction:
    case cudaErrorNoKernelImageForDevice:
    case cudaErrorUnsupportedPtxVersion:
    case cudaErrorInvalidPtx:
    case cudaErrorJitCompilerNotFound:
    case cudaErrorCallRequiresNewerDriver:
    case cudaErrorMemoryAllocation:
        cudaGetLastError();
        return false;
    default:
        throw std::runtime_error("CUDA preflight: " + status.reason);
    }
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
