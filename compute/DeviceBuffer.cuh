#pragma once

#include <chrono>
#include <cstddef>
#include <cuda_runtime.h>

namespace mosaicraft
{
namespace cuda
{
// 容量只在需要增长时分配；任务持有工作区，避免批次之间反复申请显存。
template <class T> class DeviceBuffer
{
  public:
    DeviceBuffer() = default;
    ~DeviceBuffer()
    {
        reset();
    }
    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;
    cudaError_t allocate(std::size_t bytes)
    {
        if (bytes <= m_capacity)
        {
            return cudaSuccess;
        }
        reset();
        auto result = cudaMalloc(reinterpret_cast<void **>(&m_ptr), bytes);
        if (result == cudaSuccess)
        {
            m_capacity = bytes;
        }
        return result;
    }
    void reset()
    {
        if (m_ptr)
        {
            cudaFree(m_ptr);
            m_ptr = nullptr;
        }
        m_capacity = 0;
    }
    T *get() const
    {
        return m_ptr;
    }

  private:
    T *m_ptr = nullptr;
    std::size_t m_capacity = 0;
};

class PhaseTimer
{
  public:
    explicit PhaseTimer(double *result) : m_result(result)
    {
        if (m_result)
        {
            m_start = Clock::now();
        }
    }
    ~PhaseTimer()
    {
        if (m_result)
        {
            *m_result += std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
        }
    }

  private:
    using Clock = std::chrono::steady_clock;
    double *m_result;
    Clock::time_point m_start;
};
} // namespace cuda
} // namespace mosaicraft
