#include "TaskResources.h"
#include "RuntimeContext.h"

#include <chrono>
#include <fstream>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#if defined(MOSAICRAFT_CUDA) && !defined(__APPLE__)
#if __has_include(<nvml.h>)
#include <nvml.h>
#else
// 独立 CUDA 组件可能不带 NVML 头文件，以下只声明动态加载所用的固定 ABI。
using nvmlReturn_t = int;
using nvmlDevice_t = struct nvmlDevice_st *;
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr unsigned long long NVML_VALUE_NOT_AVAILABLE = ~0ULL;
struct nvmlUtilization_t
{
    unsigned int gpu, memory;
};
struct nvmlMemory_t
{
    unsigned long long total, free, used;
};
struct nvmlProcessInfo_t
{
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId, computeInstanceId;
};
extern "C"
{
nvmlReturn_t nvmlInit_v2();
nvmlReturn_t nvmlShutdown();
nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(const char *, nvmlDevice_t *);
nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t, nvmlUtilization_t *);
nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t, nvmlMemory_t *);
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *);
}
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif
#endif

namespace mosaicraft
{
namespace
{
double cpuSeconds()
{
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
    {
        return -1;
    }
    const auto ticks = [](FILETIME time) { return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) / 10000000.0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
    {
        return -1;
    }
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#endif
}

double residentBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
        ? static_cast<double>(counters.WorkingSetSize) : -1;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS
        ? static_cast<double>(info.resident_size) : -1;
#else
    std::ifstream stat("/proc/self/statm");
    uint64_t total = 0, resident = 0;
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 && (stat >> total >> resident) ? static_cast<double>(resident) * page : -1;
#endif
}

#if defined(MOSAICRAFT_CUDA) && !defined(__APPLE__)
class GpuResources
{
    decltype(&nvmlShutdown) shutdown = nullptr;
    decltype(&nvmlDeviceGetHandleByPciBusId_v2) findDevice = nullptr;
    decltype(&nvmlDeviceGetUtilizationRates) utilization = nullptr;
    decltype(&nvmlDeviceGetMemoryInfo) memory = nullptr;
    decltype(&nvmlDeviceGetComputeRunningProcesses_v3) processes = nullptr;
#ifdef _WIN32
    HMODULE library = nullptr;
#else
    void *library = nullptr;
#endif
    bool tried = false;
    std::string previousBus;
    nvmlDevice_t device = nullptr;

    template <class Function> Function symbol(const char *name)
    {
#ifdef _WIN32
        return reinterpret_cast<Function>(GetProcAddress(library, name));
#else
        return reinterpret_cast<Function>(dlsym(library, name));
#endif
    }

    void load()
    {
        tried = true;
#ifdef _WIN32
        library = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
        library = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
#endif
        if (!library)
        {
            return;
        }
        const auto init = symbol<decltype(&nvmlInit_v2)>("nvmlInit_v2");
        if (!init || init() != NVML_SUCCESS)
        {
            return;
        }
        shutdown = symbol<decltype(shutdown)>("nvmlShutdown");
        findDevice = symbol<decltype(findDevice)>("nvmlDeviceGetHandleByPciBusId_v2");
        utilization = symbol<decltype(utilization)>("nvmlDeviceGetUtilizationRates");
        memory = symbol<decltype(memory)>("nvmlDeviceGetMemoryInfo");
        processes = symbol<decltype(processes)>("nvmlDeviceGetComputeRunningProcesses_v3");
    }

public:
    ~GpuResources()
    {
        if (shutdown)
        {
            shutdown();
        }
        if (library)
        {
#ifdef _WIN32
            FreeLibrary(library);
#else
            dlclose(library);
#endif
        }
    }

    void sample(const std::string &bus, ResourceSample &sample)
    {
        if (bus.empty())
        {
            return;
        }
        if (!tried)
        {
            load();
        }
        if (findDevice && previousBus != bus)
        {
            previousBus = bus;
            device = nullptr;
            findDevice(bus.c_str(), &device);
        }
        if (!device)
        {
            return;
        }
        nvmlUtilization_t util{};
        nvmlMemory_t usage{};
        if (utilization && utilization(device, &util) == NVML_SUCCESS)
        {
            sample.values[2] = util.gpu;
        }
        if (memory && memory(device, &usage) == NVML_SUCCESS)
        {
            sample.values[3] = static_cast<double>(usage.used);
        }
        std::array<nvmlProcessInfo_t, 64> list{};
        unsigned int count = static_cast<unsigned int>(list.size());
        if (processes && processes(device, &count, list.data()) == NVML_SUCCESS)
        {
#ifdef _WIN32
            const auto pid = GetCurrentProcessId();
#else
            const auto pid = static_cast<unsigned int>(getpid());
#endif
            for (unsigned int i = 0; i < count; ++i)
            {
                if (list[i].pid == pid && list[i].usedGpuMemory != NVML_VALUE_NOT_AVAILABLE)
                {
                    sample.values[4] = static_cast<double>(list[i].usedGpuMemory);
                }
            }
        }
    }
};
#endif
} // namespace

TaskResourceMonitor::TaskResourceMonitor(RuntimeContext &runtime, bool enabled) noexcept : m_runtime(runtime)
{
    if (!enabled)
    {
        return;
    }
    try
    {
        m_worker = std::thread([this]
        {
            try
            {
                run();
            }
            catch (...)
            {
                // 观测失败不能中断正式任务。
            }
        });
    }
    catch (...)
    {
    }
}

TaskResourceMonitor::~TaskResourceMonitor()
{
    stop();
}

void TaskResourceMonitor::stop() noexcept
{
    if (!m_worker.joinable())
    {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_one();
    m_worker.join();
}

void TaskResourceMonitor::run()
{
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    auto previous = started;
    double previousCpu = cpuSeconds();
#if defined(MOSAICRAFT_CUDA) && !defined(__APPLE__)
    GpuResources gpu;
#endif
    bool stopped = false;
    do
    {
        const auto now = Clock::now();
        const auto cpu = cpuSeconds();
        ResourceSample sample;
        sample.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        const auto elapsed = std::chrono::duration<double>(now - previous).count();
        if (cpu >= previousCpu && previousCpu >= 0 && elapsed >= 0.001)
        {
            sample.values[0] = 100 * (cpu - previousCpu) / elapsed;
        }
        sample.values[1] = residentBytes();
#if defined(MOSAICRAFT_CUDA) && !defined(__APPLE__)
        gpu.sample(m_runtime.gpuDevice(), sample);
#endif
        m_runtime.resourceSample(sample);
        previous = now;
        previousCpu = cpu;
        if (stopped)
        {
            break;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        stopped = m_cv.wait_until(lock, now + std::chrono::seconds(1), [this] { return m_stop; });
    } while (true);
}
} // namespace mosaicraft
