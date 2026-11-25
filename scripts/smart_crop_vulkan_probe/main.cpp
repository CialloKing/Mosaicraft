#include <gpu.h>
#include <layer.h>
#include <net.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
struct VulkanInstance
{
    VulkanInstance()
    {
        if (ncnn::create_gpu_instance() != 0)
        {
            throw std::runtime_error("ncnn Vulkan initialization failed");
        }
    }

    ~VulkanInstance()
    {
        ncnn::destroy_gpu_instance();
    }
};

void require(bool valid, const std::string &message)
{
    if (!valid)
    {
        throw std::runtime_error(message);
    }
}

void configure(ncnn::Net &net, int device)
{
    net.set_vulkan_device(device);
    auto &opt = net.opt;
    opt.num_threads = 1;
    opt.use_vulkan_compute = true;
    opt.use_fp16_arithmetic = opt.use_fp16_storage = opt.use_fp16_packed = opt.use_fp16_uniform = false;
    opt.use_bf16_storage = opt.use_bf16_packed = false;
    opt.use_int8_inference = opt.use_int8_arithmetic = opt.use_int8_storage = opt.use_int8_packed = opt.use_int8_uniform = false;
    opt.use_cooperative_matrix = false;
}

void run(ncnn::Net &net, const std::string &model, int width, int height, const std::string &prefix)
{
    ncnn::Mat input(width, height, 3);
    require(!input.empty(), "input allocation failed");
    for (int c = 0; c < 3; ++c)
    {
        float *channel = input.channel(c);
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i)
        {
            // Use a reproducible FP32 input with both positive and negative values.
            channel[i] = static_cast<float>(static_cast<int>((i + 17 * c) % 251) - 125) / 128.0f;
        }
    }
    auto extractor = net.create_extractor();
    require(extractor.input("in0", input) == 0, "cannot set model input");
    const int outputs = model == "real" ? 12 : 1;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < outputs; ++i)
    {
        const auto name = "out" + std::to_string(i);
        ncnn::Mat value;
        require(extractor.extract(name.c_str(), value) == 0 && !value.empty(), "extract failed: " + name);
        require(value.elemsize == sizeof(float) && value.elempack == 1 && value.d == 1, "non-FP32 output: " + name);
        int w = 320, h = 320, c = 1;
        if (model == "real")
        {
            const int stride = 8 << (i % 3);
            w = i < 6 ? 1 : (i < 9 ? 4 : 10);
            h = (width / stride) * (height / stride);
        }
        else if (model == "anime")
        {
            w = (width / 8) * (height / 8) + (width / 16) * (height / 16) + (width / 32) * (height / 32);
            h = 5;
        }
        std::cout << name << " shape=" << value.w << 'x' << value.h << 'x' << value.c << '\n';
        require(value.w == w && value.h == h && value.c == c, "unexpected output shape: " + name);
        std::ofstream out(prefix + "-" + std::to_string(width) + "x" + std::to_string(height) + "-" + name + ".f32",
                          std::ios::binary);
        for (int ch = 0; ch < c; ++ch)
        {
            const float *plane = value.channel(ch);
            for (std::size_t k = 0; k < static_cast<std::size_t>(w) * h; ++k)
            {
                require(std::isfinite(plane[k]), "non-finite output: " + name);
            }
            out.write(reinterpret_cast<const char *>(plane), static_cast<std::streamsize>(w) * h * sizeof(float));
        }
        require(out.good(), "cannot write probe output: " + name);
    }
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    std::cout << "size=" << width << 'x' << height << " probe_ms=" << elapsed.count() << std::endl;
}
}

int main(int argc, char **argv) try
{
    VulkanInstance vulkan;
    const int count = ncnn::get_gpu_count();
    if (argc == 1)
    {
        for (int i = 0; i < count; ++i)
        {
            const auto &info = ncnn::get_gpu_info(i);
            std::cout << i << ' ' << std::hex << info.vendor_id() << ':' << info.device_id() << std::dec
                      << ' ' << info.device_name();
            if (ncnn::vkGetPhysicalDeviceProperties2KHR)
            {
                VkPhysicalDeviceIDProperties id{};
                id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                VkPhysicalDeviceProperties2 properties{};
                properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                properties.pNext = &id;
                ncnn::vkGetPhysicalDeviceProperties2KHR(info.physicalDevice(), &properties);
                if (id.deviceLUIDValid)
                {
                    // Windows performance counters identify adapters by high/low LUID, not Vulkan index.
                    std::uint32_t low = 0, high = 0;
                    std::memcpy(&low, id.deviceLUID, sizeof(low));
                    std::memcpy(&high, id.deviceLUID + sizeof(low), sizeof(high));
                    std::cout << " luid_0x" << std::hex << std::setfill('0') << std::setw(8) << high
                              << "_0x" << std::setw(8) << low << std::dec;
                }
            }
            std::cout << '\n';
        }
        return count == 0 ? 1 : 0;
    }
    require(argc >= 8 && argc % 2 == 0, "usage: probe gpu model param bin output-prefix width height [width height ...]");
    const int device = std::stoi(argv[1]);
    const std::string model = argv[2];
    require(device >= 0 && device < count, "invalid Vulkan device index");
    require(model == "saliency" || model == "real" || model == "anime", "invalid probe model");
    std::cout << "device=" << ncnn::get_gpu_info(device).device_name() << std::endl;
    ncnn::Net net;
    configure(net, device);
    require(net.load_param(argv[3]) == 0, "cannot load ncnn graph");
    require(net.load_model(argv[4]) == 0, "cannot load ncnn weights");
    // ncnn can silently execute unsupported layers on the host even with Vulkan enabled.
    for (const auto *layer : net.layers())
    {
        require(layer != nullptr, "missing ncnn layer");
        require(layer->type == "Input" || layer->support_vulkan, "non-Vulkan layer: " + layer->type + " " + layer->name);
    }
    for (int i = 6; i + 1 < argc; i += 2)
    {
        const int width = std::stoi(argv[i]);
        const int height = std::stoi(argv[i + 1]);
        require(width > 0 && height > 0 && width % 32 == 0 && height % 32 == 0, "invalid model dimensions");
        run(net, model, width, height, argv[5]);
    }
    return 0;
}
catch (const std::exception &error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
