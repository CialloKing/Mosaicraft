#include "ComputeBackend.h"
#include "CudaBackend.h"
#include <stdexcept>

namespace mosaicraft::compute
{
namespace
{
Device cpuDevice()
{
    return {BackendPolicy::Cpu, "cpu", "CPU", "", true, false, true};
}
Device cudaDevice()
{
    const auto cuda = cuda::queryCudaAvailability();
    return {BackendPolicy::Cuda, "cuda:" + std::to_string(cuda.device), cuda.deviceName, cuda.reason,
            cuda.available, false, false};
}
} // namespace
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
    std::vector<Device> result{cpuDevice(), cudaDevice()};
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
    // 未建立性能准入表前，新后端只接受显式选择，不能冒称 auto 已验收。
    if (backend == BackendPolicy::Auto && id == "auto")
    {
        const auto cuda = cudaDevice();
        return cuda.available ? cuda : cpuDevice();
    }
    // 选择设备不等于列出全部设备；无关驱动初始化会拖慢原有 CUDA/CPU 的启动。
    const auto requested = backend == BackendPolicy::Auto ? parseBackend(id.substr(0, id.find(':'))) : backend;
    std::vector<Device> all;
    switch (requested)
    {
    case BackendPolicy::Cpu:
        all.push_back(cpuDevice());
        break;
    case BackendPolicy::Cuda:
        all.push_back(cudaDevice());
        break;
    case BackendPolicy::Vulkan:
        all = vulkanDevices();
        break;
    case BackendPolicy::Metal:
        all = metalDevices();
        break;
    default:
        break;
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
} // namespace mosaicraft::compute
