#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mosaicraft
{

class WorkerPool;

struct ParallelPngPlan
{
    size_t workers = 0;
    size_t blockRows = 0;
    uint64_t bufferBytes = 0;
};

ParallelPngPlan planParallelPng(int width, int height, unsigned workers, uint64_t memoryBudget);

// 以有界缓冲写 RGB8；调用方选择与 libpng 路径一致的滤波策略。
class ParallelPngWriter
{
  public:
    ParallelPngWriter(const std::string &path, int width, int height, int level, bool fixedSub,
                      const ParallelPngPlan &plan, WorkerPool &pool);
    ~ParallelPngWriter();

    void writeRows(const uint8_t *rgb, size_t stride, int count);
    bool close();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mosaicraft
