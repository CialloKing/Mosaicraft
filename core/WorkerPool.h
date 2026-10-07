#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mosaicraft
{

// 一个任务内复用工作线程；run 返回前所有任务和异常均已收束。
class WorkerPool
{
public:
    explicit WorkerPool(unsigned requested = 0)
    {
        unsigned count = requested ? requested : std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
        try
        {
            for (unsigned i = 0; i < count; ++i)
            {
                m_threads.emplace_back([this]() { worker(); });
            }
        }
        catch (...)
        {
            stop();
            throw;
        }
    }

    ~WorkerPool() { stop(); }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void run(std::size_t count, std::function<void(std::size_t)> job)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_job = std::move(job);
        m_count = count;
        m_next = 0;
        m_error = nullptr;
        m_remaining = m_threads.size();
        ++m_generation;
        m_ready.notify_all();
        m_done.wait(lock, [this]() { return m_remaining == 0; });
        m_job = {};
        if (m_error)
        {
            std::rethrow_exception(m_error);
        }
    }

private:
    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_ready.notify_all();
        for (auto& thread : m_threads)
        {
            thread.join();
        }
    }

    void worker()
    {
        std::size_t generation = 0;
        for (;;)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ready.wait(lock, [&]() { return m_stop || m_generation != generation; });
            if (m_stop)
            {
                return;
            }
            generation = m_generation;
            lock.unlock();
            try
            {
                for (std::size_t i = m_next.fetch_add(1); i < m_count; i = m_next.fetch_add(1))
                {
                    m_job(i);
                }
            }
            catch (...)
            {
                std::lock_guard<std::mutex> errorLock(m_mutex);
                if (!m_error)
                {
                    m_error = std::current_exception();
                }
                m_next = m_count;
            }
            lock.lock();
            if (--m_remaining == 0)
            {
                m_done.notify_one();
            }
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_ready, m_done;
    std::function<void(std::size_t)> m_job;
    std::exception_ptr m_error;
    std::atomic<std::size_t> m_next{0};
    std::size_t m_count = 0, m_remaining = 0, m_generation = 0;
    bool m_stop = false;
};

}
