#pragma once
#include <chrono>

namespace mosaicraft
{
// Disabled instrumentation never reads the clock.
class BenchmarkTimer
{
    using Clock = std::chrono::steady_clock;
    bool m_enabled;
    double &m_total;
    Clock::time_point m_start;

  public:
    BenchmarkTimer(bool enabled, double &total)
        : m_enabled(enabled), m_total(total), m_start(enabled ? Clock::now() : Clock::time_point{})
    {
    }
    ~BenchmarkTimer()
    {
        if (m_enabled)
        {
            m_total += std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
        }
    }
};
} // namespace mosaicraft
