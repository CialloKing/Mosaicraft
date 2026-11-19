#pragma once
#include <chrono>
#include <cstddef>

namespace mosaicraft
{
struct BenchmarkSample
{
    double ms = 0;
    std::size_t calls = 0;

    void merge(const BenchmarkSample &other)
    {
        ms += other.ms;
        calls += other.calls;
    }
};

// Disabled instrumentation never reads the clock.
class BenchmarkTimer
{
    using Clock = std::chrono::steady_clock;
    double *m_total;
    BenchmarkSample *m_sample;
    Clock::time_point m_start;

    BenchmarkTimer(double *total, BenchmarkSample *sample)
        : m_total(total), m_sample(sample), m_start(total ? Clock::now() : Clock::time_point{})
    {
    }

  public:
    BenchmarkTimer(bool enabled, double &total)
        : BenchmarkTimer(enabled ? &total : nullptr, nullptr)
    {
    }
    explicit BenchmarkTimer(BenchmarkSample *sample)
        : BenchmarkTimer(sample ? &sample->ms : nullptr, sample)
    {
    }
    ~BenchmarkTimer()
    {
        if (m_total)
        {
            *m_total += std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
            if (m_sample)
            {
                ++m_sample->calls;
            }
        }
    }
};
} // namespace mosaicraft
