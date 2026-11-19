#pragma once

#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

namespace mosaicraft
{
inline uint64_t availableMemory()
{
#ifdef _WIN32
    MEMORYSTATUSEX memory{sizeof(memory)};
    return GlobalMemoryStatusEx(&memory) ? memory.ullAvailPhys : 0;
#elif defined(__APPLE__)
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t statistics{};
    const auto host = mach_host_self();
    vm_size_t pageSize = 0;
    const bool valid = host_page_size(host, &pageSize) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    // Inactive pages are reclaimable; do not count compressed or wired memory.
    return valid ? (static_cast<uint64_t>(statistics.free_count) + statistics.inactive_count) * pageSize : 0;
#else
    const long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? static_cast<uint64_t>(pages) * size : 0;
#endif
}
} // namespace mosaicraft
