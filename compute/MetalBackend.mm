#include "ComputeBackend.h"
#include "PortableMetal.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace mosaicraft::compute
{
namespace
{
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
class MetalSession final : public Session
{
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> featurePipeline, scorePipeline;
    id<MTLBuffer> buffers[4];

  public:
    explicit MetalSession(const Device &description)
    {
        @autoreleasepool
        {
            NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
            size_t index = std::stoul(description.id.substr(description.id.find(':') + 1));
            if (index >= all.count)
            {
                throw std::runtime_error("Metal device disappeared");
            }
            device = all[index];
            queue = [device newCommandQueue];
            NSError *error = nil;
            dispatch_data_t data =
                dispatch_data_create(metalLibrary, sizeof(metalLibrary),
                                     dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
            if (!queue || !library)
            {
                throw std::runtime_error(error ? error.localizedDescription.UTF8String
                                               : "cannot prepare Metal queue/library");
            }
            featurePipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"features"]
                                                                    error:&error];
            scorePipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"scoring"]
                                                                  error:&error];
            if (!featurePipeline || !scorePipeline || featurePipeline.maxTotalThreadsPerThreadgroup < 128 ||
                scorePipeline.maxTotalThreadsPerThreadgroup < 128)
            {
                throw std::runtime_error("Metal compute pipeline preparation failed");
            }
            for (unsigned i = 0; i < 4; ++i)
            {
                buffer(i, 4);
            }
        }
    }
    void buffer(unsigned slot, size_t bytes, const void *input = nullptr) override
    {
        if (slot >= 4 || !bytes || bytes > device.maxBufferLength)
        {
            throw std::runtime_error("Metal buffer exceeds device limit");
        }
        if (!buffers[slot] || buffers[slot].length < bytes)
        {
            const auto start = Clock::now();
            timings.residentBytes -= buffers[slot].length;
            buffers[slot] = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            if (!buffers[slot])
            {
                throw std::runtime_error("Metal buffer allocation failed");
            }
            timings.residentBytes += bytes;
            timings.peakResidentBytes = std::max(timings.peakResidentBytes, timings.residentBytes);
            timings.allocationMs += elapsed(start);
        }
        if (input)
        {
            write(slot, 0, bytes, input);
        }
    }
    void write(unsigned slot, size_t offset, size_t bytes, const void *input) override
    {
        if (slot >= 4 || offset > buffers[slot].length || bytes > buffers[slot].length - offset)
        {
            throw std::out_of_range("Metal upload exceeds buffer");
        }
        const auto start = Clock::now();
        std::memcpy(static_cast<uint8_t *>(buffers[slot].contents) + offset, input, bytes);
        timings.uploadMs += elapsed(start);
        timings.uploadBytes += bytes;
    }
    void read(unsigned slot, size_t bytes, void *output) override
    {
        if (slot >= 4 || bytes > buffers[slot].length)
        {
            throw std::out_of_range("Metal download exceeds buffer");
        }
        const auto start = Clock::now();
        std::memcpy(output, buffers[slot].contents, bytes);
        timings.downloadMs += elapsed(start);
        timings.downloadBytes += bytes;
    }
    void dispatch(bool scoring, const std::array<uint32_t, 4> &p, uint32_t x, uint32_t y) override
    {
        @autoreleasepool
        {
            const auto start = Clock::now();
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (!command || !encoder)
            {
                throw std::runtime_error("cannot prepare Metal command");
            }
            [encoder setComputePipelineState:scoring ? scorePipeline : featurePipeline];
            for (unsigned i = 0; i < 4; ++i)
            {
                [encoder setBuffer:buffers[i] offset:0 atIndex:i];
            }
            [encoder setBytes:p.data() length:16 atIndex:4];
            [encoder dispatchThreadgroups:MTLSizeMake(x, y, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted)
            {
                throw std::runtime_error(command.error ? command.error.localizedDescription.UTF8String
                                                       : "Metal execution failed");
            }
            timings.computeMs += elapsed(start);
        }
    }
};
} // namespace
std::vector<Device> metalDevices()
{
    @autoreleasepool
    {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        std::vector<Device> result;
        for (NSUInteger i = 0; i < all.count; ++i)
        {
            id<MTLDevice> native = all[i];
            Device description{BackendPolicy::Metal, "metal:" + std::to_string(i), native.name.UTF8String};
            description.available = true;
            description.integrated = native.hasUnifiedMemory;
            description.maxBufferBytes = native.maxBufferLength;
            description.reason = "candidate: hardware validation not-run";
            result.push_back(std::move(description));
        }
        if (result.empty())
        {
            result.push_back({BackendPolicy::Metal, "metal", "Metal", "no Metal devices"});
        }
        return result;
    }
}
std::unique_ptr<Session> openMetal(const Device &device)
{
    return std::make_unique<MetalSession>(device);
}
} // namespace mosaicraft::compute
