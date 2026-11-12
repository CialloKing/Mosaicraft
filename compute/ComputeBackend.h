#pragma once

#include "core/ExecutionOptions.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mosaicraft::compute
{
struct Device
{
    BackendPolicy backend = BackendPolicy::Cpu;
    std::string id, name, reason;
    bool available = false, integrated = false, validated = false;
    uint64_t maxBufferBytes = 0;
};

struct Timings
{
    double allocationMs = 0, uploadMs = 0, computeMs = 0, downloadMs = 0;
    uint64_t uploadBytes = 0, downloadBytes = 0, residentBytes = 0, peakResidentBytes = 0;
};

// 工作区只拥有四个有类型契约的存储缓冲；原生句柄不泄漏到业务层。
class Session
{
  public:
    virtual ~Session() = default;
    virtual void buffer(unsigned slot, size_t bytes, const void *input = nullptr) = 0;
    virtual void write(unsigned slot, size_t offset, size_t bytes, const void *input) = 0;
    virtual void read(unsigned slot, size_t bytes, void *output) = 0;
    virtual void dispatch(bool scoring, const std::array<uint32_t, 4> &parameters, uint32_t x, uint32_t y = 1) = 0;
    Timings timings;
};

std::vector<Device> devices();
Device select(BackendPolicy backend, const std::string &id);
std::unique_ptr<Session> open(const Device &device);
std::vector<Device> vulkanDevices();
std::unique_ptr<Session> openVulkan(const Device &device);
std::vector<Device> metalDevices();
std::unique_ptr<Session> openMetal(const Device &device);
} // namespace mosaicraft::compute
