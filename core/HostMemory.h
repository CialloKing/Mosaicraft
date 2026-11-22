#pragma once

#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#ifdef __linux__
#include <fstream>
#include <limits>
#include <string>
#endif
#endif

namespace mosaicraft
{
// 调用方需要按机器容量分档时，同时取得总物理内存。
inline uint64_t availableMemory(uint64_t *total = nullptr)
{
    if (total)
    {
        *total = 0;
    }
#ifdef _WIN32
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (!GlobalMemoryStatusEx(&memory))
    {
        return 0;
    }
    if (total)
    {
        *total = memory.ullTotalPhys;
    }
    return memory.ullAvailPhys;
#elif defined(__APPLE__)
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t statistics{};
    const auto host = mach_host_self();
    vm_size_t pageSize = 0;
    const bool valid = host_page_size(host, &pageSize) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (valid && total)
    {
        size_t bytes = sizeof(*total);
        sysctlbyname("hw.memsize", total, &bytes, nullptr, 0);
    }
    // Inactive pages are reclaimable; do not count compressed or wired memory.
    return valid ? (static_cast<uint64_t>(statistics.free_count) + statistics.inactive_count) * pageSize : 0;
#else
#ifdef __linux__
    if (total)
    {
        // Linux 空闲页不包括可回收的文件缓存；桌面预算优先使用内核的可用量估计。
        std::ifstream info("/proc/meminfo");
        std::string name;
        uint64_t kib = 0;
        while (info >> name >> kib)
        {
            if (name == "MemTotal:")
            {
                *total = kib * 1024;
            }
            else if (name == "MemAvailable:" && *total)
            {
                return kib * 1024;
            }
            info.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
    }
#endif
    const long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    if (total && pages >= 0 && size > 0)
    {
        const long count = sysconf(_SC_PHYS_PAGES);
        *total = count > 0 ? static_cast<uint64_t>(count) * size : 0;
    }
    return pages > 0 && size > 0 ? static_cast<uint64_t>(pages) * size : 0;
#endif
}
} // namespace mosaicraft
