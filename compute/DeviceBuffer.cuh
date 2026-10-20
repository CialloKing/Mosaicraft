#pragma once

#include "CudaBackend.h"
#include <chrono>
#include <cstddef>
#include <cuda_runtime.h>
#include <array>
#include <stdexcept>
#include <string>

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
    std::size_t capacityBytes() const
    {
        return m_capacity;
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

inline void requireCuda(cudaError_t error)
{
    if (error != cudaSuccess)
    {
        throw std::runtime_error(std::string("CUDA pipeline: ") + cudaGetErrorString(error));
    }
}

inline bool optionalCudaAllocation(cudaError_t error)
{
    if (error == cudaErrorMemoryAllocation)
    {
        cudaGetLastError();
        return false;
    }
    requireCuda(error);
    return true;
}

class PinnedBuffer
{
  public:
    ~PinnedBuffer()
    {
        reset();
    }
    PinnedBuffer() = default;
    PinnedBuffer(const PinnedBuffer &) = delete;
    PinnedBuffer &operator=(const PinnedBuffer &) = delete;
    cudaError_t allocate(std::size_t bytes)
    {
        if (bytes <= m_capacity)
        {
            return cudaSuccess;
        }
        reset();
        const auto error = cudaMallocHost(reinterpret_cast<void **>(&m_ptr), bytes);
        if (error == cudaSuccess)
        {
            m_capacity = bytes;
        }
        return error;
    }
    void reset()
    {
        if (m_ptr)
        {
            cudaFreeHost(m_ptr);
            m_ptr = nullptr;
        }
        m_capacity = 0;
    }
    std::uint8_t *get() const
    {
        return m_ptr;
    }
    std::size_t capacityBytes() const
    {
        return m_capacity;
    }

  private:
    std::uint8_t *m_ptr = nullptr;
    std::size_t m_capacity = 0;
};

// 在所引用缓冲之后声明，析构先收束 stream，再释放锁页内存和显存。
class TransferStream
{
  public:
    ~TransferStream()
    {
        if (stream)
        {
            cudaStreamSynchronize(stream);
        }
        for (auto event : events)
        {
            if (event)
            {
                cudaEventDestroy(event);
            }
        }
        if (stream)
        {
            cudaStreamDestroy(stream);
        }
    }
    bool create()
    {
        if (!optionalCudaAllocation(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking)))
        {
            return false;
        }
        for (auto &event : events)
        {
            if (!optionalCudaAllocation(cudaEventCreateWithFlags(&event, cudaEventBlockingSync)))
            {
                return false;
            }
        }
        return true;
    }
    void mark(int index)
    {
        requireCuda(cudaEventRecord(events[index], stream));
    }
    void finish(GpuTimings *timings)
    {
        {
            PhaseTimer timer(timings ? &timings->waitMs : nullptr);
            requireCuda(cudaEventSynchronize(events[3]));
        }
        if (timings)
        {
            float upload = 0, compute = 0, download = 0;
            requireCuda(cudaEventElapsedTime(&upload, events[0], events[1]));
            requireCuda(cudaEventElapsedTime(&compute, events[1], events[2]));
            requireCuda(cudaEventElapsedTime(&download, events[2], events[3]));
            timings->eventUploadMs += upload;
            timings->eventComputeMs += compute;
            timings->eventDownloadMs += download;
        }
    }
    cudaStream_t stream = nullptr;

  private:
    std::array<cudaEvent_t, 4> events{};
};

// 异常也必须先等待自己提交的操作，避免释放/重写仍被 DMA 读取的主机缓冲。
class StreamDrain
{
  public:
    explicit StreamDrain(cudaStream_t stream) : m_stream(stream)
    {
    }
    ~StreamDrain()
    {
        if (m_stream)
        {
            cudaStreamSynchronize(m_stream);
        }
    }
    void complete()
    {
        m_stream = nullptr;
    }

  private:
    cudaStream_t m_stream;
};

struct DeviceFeatureState
{
    const float *grid = nullptr, *lbp = nullptr;
    const std::uint8_t *tiny = nullptr;
    std::uint64_t generation = 0;
    int count = 0, ready = 0;
    std::size_t bytes = 0;
};
} // namespace cuda
} // namespace mosaicraft
