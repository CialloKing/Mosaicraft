#define VK_NO_PROTOTYPES
#include "ComputeBackend.h"
#include "PortableSpirv.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mosaicraft::compute
{
namespace
{
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void check(VkResult result, const char *operation)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(std::string("Vulkan ") + operation + ": " + std::to_string(result));
    }
}

#define VULKAN_FUNCTIONS(X)                                                                                            \
    X(DestroyInstance)                                                                                                 \
    X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties)               \
        X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle)      \
            X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory)            \
                X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges)                             \
                    X(InvalidateMappedMemoryRanges) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)         \
                        X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule)  \
                            X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool)                       \
                                X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets)             \
                                    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)               \
                                        X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer)                  \
                                            X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants)            \
                                                X(CmdDispatch) X(CmdPipelineBarrier) X(QueueSubmit) X(QueueWaitIdle)

struct Runtime
{
#ifdef _WIN32
    HMODULE module = nullptr;
#else
    void *module = nullptr;
#endif
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr get = nullptr;
#define DECLARE(NAME) PFN_vk##NAME NAME = nullptr;
    VULKAN_FUNCTIONS(DECLARE)
#undef DECLARE

    Runtime()
    {
#ifdef _WIN32
        module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        get = module ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"))
                     : nullptr;
#else
        module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        get = module ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(module, "vkGetInstanceProcAddr")) : nullptr;
#endif
        if (!get)
        {
            release();
            throw std::runtime_error("Vulkan loader is unavailable");
        }
        try
        {
            const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.pApplicationName = "Mosaicraft";
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
            check(create(&info, nullptr, &instance), "create instance");
#define LOAD(NAME)                                                                                                     \
    NAME = reinterpret_cast<PFN_vk##NAME>(get(instance, "vk" #NAME));                                                  \
    if (!NAME)                                                                                                         \
    {                                                                                                                  \
        throw std::runtime_error("missing Vulkan entry point: " #NAME);                                                \
    }
            VULKAN_FUNCTIONS(LOAD)
#undef LOAD
        }
        catch (...)
        {
            release();
            throw;
        }
    }
    void release()
    {
        if (instance && DestroyInstance)
        {
            DestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        if (module)
        {
#ifdef _WIN32
            FreeLibrary(module);
#else
            dlclose(module);
#endif
            module = nullptr;
        }
    }
    ~Runtime()
    {
        release();
    }
    std::vector<VkPhysicalDevice> physical()
    {
        uint32_t count = 0;
        check(EnumeratePhysicalDevices(instance, &count, nullptr), "enumerate count");
        std::vector<VkPhysicalDevice> result(count);
        check(EnumeratePhysicalDevices(instance, &count, result.data()), "enumerate devices");
        result.resize(count);
        return result;
    }
    uint32_t family(VkPhysicalDevice physical)
    {
        uint32_t count = 0;
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
            {
                return i;
            }
        }
        throw std::runtime_error("no Vulkan compute queue");
    }
};

class VulkanSession final : public Session
{
    Runtime api;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceProperties properties{};
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::array<VkPipeline, 2> pipelines{};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    struct Buffer
    {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory allocation = VK_NULL_HANDLE;
        void *mapped = nullptr;
        size_t capacity = 0;
        bool coherent = false;
    };
    std::array<Buffer, 4> buffers;

    void free(Buffer &b)
    {
        if (b.mapped)
        {
            api.UnmapMemory(device, b.allocation);
        }
        if (b.handle)
        {
            api.DestroyBuffer(device, b.handle, nullptr);
        }
        if (b.allocation)
        {
            api.FreeMemory(device, b.allocation, nullptr);
        }
        b = {};
    }
    void release()
    {
        if (!device)
        {
            return;
        }
        api.DeviceWaitIdle(device);
        for (auto &b : buffers)
        {
            free(b);
        }
        for (auto pipeline : pipelines)
        {
            if (pipeline)
            {
                api.DestroyPipeline(device, pipeline, nullptr);
            }
        }
        if (commands)
        {
            api.DestroyCommandPool(device, commands, nullptr);
        }
        if (pool)
        {
            api.DestroyDescriptorPool(device, pool, nullptr);
        }
        if (layout)
        {
            api.DestroyPipelineLayout(device, layout, nullptr);
        }
        if (descriptors)
        {
            api.DestroyDescriptorSetLayout(device, descriptors, nullptr);
        }
        api.DestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }

  public:
    explicit VulkanSession(const Device &description)
    {
        try
        {
            const auto physicals = api.physical();
            const size_t index = std::stoul(description.id.substr(description.id.find(':') + 1));
            if (index >= physicals.size())
            {
                throw std::runtime_error("Vulkan device disappeared");
            }
            const auto physical = physicals[index];
            api.GetPhysicalDeviceMemoryProperties(physical, &memory);
            api.GetPhysicalDeviceProperties(physical, &properties);
            const uint32_t family = api.family(physical);
            float priority = 1;
            VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            q.queueFamilyIndex = family;
            q.queueCount = 1;
            q.pQueuePriorities = &priority;
            VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            d.queueCreateInfoCount = 1;
            d.pQueueCreateInfos = &q;
            check(api.CreateDevice(physical, &d, nullptr, &device), "create device");
            api.GetDeviceQueue(device, family, 0, &queue);
            std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
            for (uint32_t i = 0; i < 4; ++i)
            {
                bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            }
            VkDescriptorSetLayoutCreateInfo desc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            desc.bindingCount = 4;
            desc.pBindings = bindings.data();
            check(api.CreateDescriptorSetLayout(device, &desc, nullptr, &descriptors), "descriptor layout");
            VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
            VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            li.setLayoutCount = 1;
            li.pSetLayouts = &descriptors;
            li.pushConstantRangeCount = 1;
            li.pPushConstantRanges = &push;
            check(api.CreatePipelineLayout(device, &li, nullptr, &layout), "pipeline layout");
            for (size_t i = 0; i < 2; ++i)
            {
                VkShaderModule module = VK_NULL_HANDLE;
                VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                si.pCode = i ? scoreSpirv : featureSpirv;
                si.codeSize = i ? sizeof(scoreSpirv) : sizeof(featureSpirv);
                check(api.CreateShaderModule(device, &si, nullptr, &module), "shader module");
                VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                pi.stage.module = module;
                pi.stage.pName = "main";
                pi.layout = layout;
                const auto result = api.CreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipelines[i]);
                api.DestroyShaderModule(device, module, nullptr);
                check(result, "compute pipeline");
            }
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.maxSets = 1;
            dpi.poolSizeCount = 1;
            dpi.pPoolSizes = &size;
            check(api.CreateDescriptorPool(device, &dpi, nullptr, &pool), "descriptor pool");
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &descriptors;
            check(api.AllocateDescriptorSets(device, &ai, &set), "descriptor set");
            VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            ci.queueFamilyIndex = family;
            check(api.CreateCommandPool(device, &ci, nullptr, &commands), "command pool");
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = commands;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            check(api.AllocateCommandBuffers(device, &cai, &command), "command buffer");
            for (unsigned i = 0; i < 4; ++i)
            {
                buffer(i, 4);
            }
        }
        catch (...)
        {
            release();
            throw;
        }
    }
    ~VulkanSession() override
    {
        release();
    }
    void buffer(unsigned slot, size_t bytes, const void *input = nullptr) override
    {
        auto &b = buffers.at(slot);
        if (!bytes || bytes > properties.limits.maxStorageBufferRange)
        {
            throw std::runtime_error("Vulkan storage buffer exceeds device limit");
        }
        if (b.capacity < bytes)
        {
            const auto start = Clock::now();
            timings.residentBytes -= b.capacity;
            free(b);
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = bytes;
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(api.CreateBuffer(device, &bi, nullptr, &b.handle), "create buffer");
            VkMemoryRequirements requirements;
            api.GetBufferMemoryRequirements(device, b.handle, &requirements);
            uint32_t selected = UINT32_MAX;
            int best = -1;
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            {
                const auto flags = memory.memoryTypes[i].propertyFlags;
                if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                {
                    const int rank = ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 2 : 0) +
                                     ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 1 : 0);
                    if (rank > best)
                    {
                        selected = i;
                        best = rank;
                    }
                }
            }
            if (selected == UINT32_MAX)
            {
                throw std::runtime_error("Vulkan host-visible memory unavailable");
            }
            VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mi.allocationSize = requirements.size;
            mi.memoryTypeIndex = selected;
            check(api.AllocateMemory(device, &mi, nullptr, &b.allocation), "allocate buffer");
            check(api.BindBufferMemory(device, b.handle, b.allocation, 0), "bind buffer");
            check(api.MapMemory(device, b.allocation, 0, VK_WHOLE_SIZE, 0, &b.mapped), "map buffer");
            b.capacity = bytes;
            b.coherent = (memory.memoryTypes[selected].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            timings.residentBytes += bytes;
            timings.allocationMs += elapsed(start);
        }
        if (input)
        {
            const auto start = Clock::now();
            std::memcpy(b.mapped, input, bytes);
            if (!b.coherent)
            {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = b.allocation;
                range.size = VK_WHOLE_SIZE;
                check(api.FlushMappedMemoryRanges(device, 1, &range), "flush buffer");
            }
            timings.uploadMs += elapsed(start);
            timings.uploadBytes += bytes;
        }
    }
    void read(unsigned slot, size_t bytes, void *output) override
    {
        auto &b = buffers.at(slot);
        if (bytes > b.capacity)
        {
            throw std::out_of_range("Vulkan download exceeds buffer");
        }
        const auto start = Clock::now();
        if (!b.coherent)
        {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = b.allocation;
            range.size = VK_WHOLE_SIZE;
            check(api.InvalidateMappedMemoryRanges(device, 1, &range), "invalidate buffer");
        }
        std::memcpy(output, b.mapped, bytes);
        timings.downloadMs += elapsed(start);
        timings.downloadBytes += bytes;
    }
    void dispatch(bool scoring, const std::array<uint32_t, 4> &parameters, uint32_t x, uint32_t y) override
    {
        if (!x || !y || x > properties.limits.maxComputeWorkGroupCount[0] ||
            y > properties.limits.maxComputeWorkGroupCount[1])
        {
            throw std::runtime_error("Vulkan dispatch exceeds device limits");
        }
        std::array<VkDescriptorBufferInfo, 4> infos{};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            infos[i] = {buffers[i].handle, 0, buffers[i].capacity};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        api.UpdateDescriptorSets(device, 4, writes.data(), 0, nullptr);
        const auto start = Clock::now();
        check(api.ResetCommandPool(device, commands, 0), "reset command pool");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(api.BeginCommandBuffer(command, &begin), "begin command");
        api.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[scoring ? 1 : 0]);
        api.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        api.CmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, parameters.data());
        api.CmdDispatch(command, x, y, 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        api.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                               &barrier, 0, nullptr, 0, nullptr);
        check(api.EndCommandBuffer(command), "end command");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(api.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit");
        check(api.QueueWaitIdle(queue), "wait");
        timings.computeMs += elapsed(start);
    }
};
} // namespace

std::vector<Device> vulkanDevices()
{
    try
    {
        Runtime api;
        const auto physicals = api.physical();
        std::vector<Device> result;
        for (size_t i = 0; i < physicals.size(); ++i)
        {
            VkPhysicalDeviceProperties p;
            api.GetPhysicalDeviceProperties(physicals[i], &p);
            Device device{BackendPolicy::Vulkan, "vulkan:" + std::to_string(i), p.deviceName};
            device.integrated = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
            device.maxBufferBytes = p.limits.maxStorageBufferRange;
            device.available = p.apiVersion >= VK_API_VERSION_1_1 && p.limits.maxComputeWorkGroupInvocations >= 128 &&
                               p.limits.maxComputeSharedMemorySize >= 451 * sizeof(uint32_t);
            try
            {
                api.family(physicals[i]);
            }
            catch (const std::exception &e)
            {
                device.available = false;
                device.reason = e.what();
            }
            if (!device.available && device.reason.empty())
            {
                device.reason = "Vulkan compute limits are insufficient";
            }
            result.push_back(std::move(device));
        }
        return result;
    }
    catch (const std::exception &e)
    {
        return {{BackendPolicy::Vulkan, "vulkan", "Vulkan", e.what()}};
    }
}
std::unique_ptr<Session> openVulkan(const Device &device)
{
    return std::make_unique<VulkanSession>(device);
}
} // namespace mosaicraft::compute
