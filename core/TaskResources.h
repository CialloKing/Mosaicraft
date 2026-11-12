#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace mosaicraft
{
class RuntimeContext;

inline constexpr std::array<const char *, 5> resourceNames
{
    "cpuPct", "rssBytes", "gpuPct", "deviceVramBytes", "processVramBytes"
};

struct ResourceSample
{
    uint64_t elapsedMs = 0;
    std::array<double, resourceNames.size()> values{-1, -1, -1, -1, -1};
};

struct ResourceAggregate
{
    double sum = 0, peak = 0;
    uint64_t count = 0;
};

class TaskResourceMonitor
{
public:
    TaskResourceMonitor(RuntimeContext &runtime, bool enabled) noexcept;
    ~TaskResourceMonitor();
    void stop() noexcept;

private:
    void run();
    RuntimeContext &m_runtime;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::thread m_worker;
};
} // namespace mosaicraft
