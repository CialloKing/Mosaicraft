#include "ComputeBackend.h"
#include "CudaBackend.h"
#include <stdexcept>

namespace mosaicraft::compute
{
#ifndef MOSAICRAFT_VULKAN
std::vector<Device> vulkanDevices()
{
    return {{BackendPolicy::Vulkan, "vulkan", "Vulkan", "not compiled"}};
}
std::unique_ptr<Session> openVulkan(const Device &)
{
    throw std::runtime_error("Vulkan support is not compiled");
}
#endif
#ifndef MOSAICRAFT_METAL
std::vector<Device> metalDevices()
{
    return {{BackendPolicy::Metal, "metal", "Metal", "not compiled"}};
}
std::unique_ptr<Session> openMetal(const Device &)
{
    throw std::runtime_error("Metal support is not compiled");
}
#endif

std::vector<Device> devices()
{
    std::vector<Device> result{{BackendPolicy::Cpu, "cpu", "CPU", "", true, false, true}};
    const auto cuda = cuda::queryCudaAvailability();
    result.push_back({BackendPolicy::Cuda, "cuda:" + std::to_string(cuda.device), cuda.deviceName,
                      cuda.reason, cuda.available, false, cuda.available});
    for (auto &device : vulkanDevices())
    {
        result.push_back(std::move(device));
    }
    for (auto &device : metalDevices())
    {
        result.push_back(std::move(device));
    }
    return result;
}

Device select(BackendPolicy backend, const std::string &id)
{
    const auto all = devices();
    // 未建立性能准入表前，新后端只接受显式选择，不能冒称 auto 已验收。
    if (backend == BackendPolicy::Auto && id == "auto")
    {
        for (const auto &device : all)
        {
            if (device.backend == BackendPolicy::Cuda && device.available)
            {
                return device;
            }
        }
        return all.front();
    }
    for (const auto &device : all)
    {
        if ((backend == BackendPolicy::Auto || backend == device.backend) && device.available &&
            (id == "auto" || id == device.id))
        {
            return device;
        }
    }
    throw std::runtime_error("requested GPU backend/device is unavailable: " + backendName(backend) + "/" + id);
}

std::unique_ptr<Session> open(const Device &device)
{
    if (device.backend == BackendPolicy::Vulkan)
    {
        return openVulkan(device);
    }
    if (device.backend == BackendPolicy::Metal)
    {
        return openMetal(device);
    }
    throw std::invalid_argument("portable compute session requires Vulkan or Metal");
}
}
