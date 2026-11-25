#include "SmartCropVulkan.h"
#include "ExecutablePath.h"
#include "SmartCrop.h"
#include "third_party/picosha2/picosha2.h"

#include <gpu.h>
#include <layer.h>
#include <net.h>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace mosaicraft
{
namespace
{
struct VulkanInstance
{
    VulkanInstance()
    {
        if (ncnn::create_gpu_instance() != 0)
        {
            throw SmartCropError("smart_crop_vulkan_device", "cannot initialize ncnn Vulkan");
        }
    }

    ~VulkanInstance()
    {
        ncnn::destroy_gpu_instance();
    }
};

const ncnn::GpuInfo &gpuInfo(int device)
{
    static const VulkanInstance instance;
    if (device < 0 || device >= ncnn::get_gpu_count())
    {
        throw SmartCropError("smart_crop_vulkan_device", "Vulkan device index was not enumerated");
    }
    return ncnn::get_gpu_info(device);
}

void require(bool ok, const std::string &message)
{
    if (!ok)
    {
        throw SmartCropError("smart_crop_vulkan_inference", message);
    }
}

std::filesystem::path verifiedAsset(const char *name, const char *hash)
{
    const auto file = executableDirectory() / "models" / "vulkan" / name;
    std::ifstream input(file, std::ios::binary);
    if (!input || picosha2::hash256_hex_string(std::istreambuf_iterator<char>(input), {}) != hash)
    {
        throw SmartCropError("smart_crop_model", std::string("missing or mismatched Vulkan model asset: ") + name);
    }
    return file;
}

void initNet(ncnn::Net &net, int device, const char *param, const char *paramHash, const char *bin, const char *binHash)
{
    net.set_vulkan_device(device);
    auto &opt = net.opt;
    opt.num_threads = 1;
    opt.use_vulkan_compute = true;
    opt.use_fp16_arithmetic = opt.use_fp16_storage = opt.use_fp16_packed = opt.use_fp16_uniform = false;
    opt.use_bf16_storage = opt.use_bf16_packed = false;
    opt.use_int8_inference = opt.use_int8_arithmetic = opt.use_int8_storage = opt.use_int8_packed = opt.use_int8_uniform = false;
    opt.use_cooperative_matrix = false;
    const auto graph = verifiedAsset(param, paramHash);
    const auto weights = verifiedAsset(bin, binHash);
    if (net.load_param(graph.c_str()) != 0 || net.load_model(weights.c_str()) != 0)
    {
        throw SmartCropError("smart_crop_model", std::string("cannot load Vulkan model: ") + param);
    }
    for (const auto *layer : net.layers())
    {
        // Do not let ncnn invisibly run an unsupported neural-network layer on the CPU.
        if (!layer || (layer->type != "Input" && !layer->support_vulkan))
        {
            throw SmartCropError("smart_crop_model", "Vulkan graph would use a host layer: " +
                                 (layer ? layer->type + " " + layer->name : std::string("missing layer")));
        }
    }
}

ncnn::Mat inputTensor(const cv::Mat &blob)
{
    require(blob.dims == 4 && blob.type() == CV_32FC1 && blob.size[0] == 1 && blob.size[1] == 3 && blob.isContinuous(),
            "invalid FP32 NCHW input for Vulkan smart crop");
    const int width = blob.size[3], height = blob.size[2];
    ncnn::Mat input(width, height, 3);
    require(!input.empty(), "cannot allocate Vulkan input staging tensor");
    // ncnn can pad its channel stride, so wrapping a packed OpenCV blob is unsafe.
    const std::size_t bytes = static_cast<std::size_t>(width) * height * sizeof(float);
    for (int i = 0; i < 3; ++i)
    {
        std::memcpy(input.channel(i).data, blob.ptr(0, i), bytes);
    }
    return input;
}

ncnn::Mat outputTensor(ncnn::Extractor &extractor, const char *name, int width, int height)
{
    ncnn::Mat output;
    require(extractor.extract(name, output) == 0 && !output.empty() && output.elemsize == sizeof(float) &&
            output.elempack == 1 && output.w == width && output.h == height && output.c == 1 && output.d == 1,
            "unexpected Vulkan output: " + std::string(name));
    const cv::Mat view(height, width, CV_32FC1, output.data);
    require(cv::checkRange(view), "non-finite Vulkan output: " + std::string(name));
    return output;
}
}

struct SmartCropVulkan::Impl
{
    ncnn::Net saliency, real;
};

SmartCropVulkan::SmartCropVulkan(int device) : m_impl(std::make_unique<Impl>())
{
    (void)gpuInfo(device);
    initNet(m_impl->saliency, device, "saliency.param", "315dbc3956dce0cf14f2821c130f1084c62ef9f4f77f3ba9a66d99c258f8e2d1",
            "saliency.bin", "7e18d54080bc4560dff8f9b39f4a5b0392f6a104cf4d59b4c5d86bd24402c908");
    initNet(m_impl->real, device, "real.param", "1ffaf4819c98d60fa811520546c4e710237c25c7e704692a082c9a3a2caa371d",
            "real.bin", "8faa696d61ad6bf5c13ed5d6c8ae35e376c660968db53d723a329020856ca5c9");
}

SmartCropVulkan::~SmartCropVulkan() = default;

unsigned SmartCropVulkan::deviceVendor(int device)
{
    return gpuInfo(device).vendor_id();
}

cv::Mat SmartCropVulkan::foreground(const cv::Mat &blob) const
{
    auto extractor = m_impl->saliency.create_extractor();
    require(extractor.input("in0", inputTensor(blob)) == 0, "cannot set Vulkan foreground input");
    auto out = outputTensor(extractor, "out0", blob.size[3], blob.size[2]);
    const int sizes[] = {1, 1, out.h, out.w};
    return cv::Mat(4, sizes, CV_32FC1, out.data).clone();
}

std::vector<cv::Rect2d> SmartCropVulkan::realBoxes(const cv::Mat &image) const
{
    constexpr int alignment = 32;
    const int width = (image.cols + alignment - 1) / alignment * alignment;
    const int height = (image.rows + alignment - 1) / alignment * alignment;
    cv::Mat padded;
    cv::copyMakeBorder(image, padded, 0, height - image.rows, 0, width - image.cols, cv::BORDER_CONSTANT, cv::Scalar());
    auto extractor = m_impl->real.create_extractor();
    require(extractor.input("in0", inputTensor(cv::dnn::blobFromImage(padded))) == 0, "cannot set Vulkan YuNet input");

    // Mirror the locked OpenCV FaceDetectorYN's stride traversal, float box decoding and integer NMS.
    std::vector<cv::Rect2d> boxes;
    std::vector<float> scores;
    constexpr float scoreThreshold = 0.9f;
    for (int i = 0; i < 3; ++i)
    {
        const int stride = 8 << i, cols = width / stride, rows = height / stride;
        const auto cls = outputTensor(extractor, ("out" + std::to_string(i)).c_str(), 1, rows * cols);
        const auto obj = outputTensor(extractor, ("out" + std::to_string(3 + i)).c_str(), 1, rows * cols);
        const auto reg = outputTensor(extractor, ("out" + std::to_string(6 + i)).c_str(), 4, rows * cols);
        const float *clsValues = cls, *objValues = obj, *regValues = reg;
        for (int r = 0; r < rows; ++r)
        {
            for (int c = 0; c < cols; ++c)
            {
                const std::size_t index = static_cast<std::size_t>(r) * cols + c;
                const float score = std::sqrt(std::clamp(clsValues[index], 0.0f, 1.0f) * std::clamp(objValues[index], 0.0f, 1.0f));
                if (score < scoreThreshold)
                {
                    continue;
                }
                const float *v = regValues + index * 4;
                const float w = static_cast<float>(std::exp(static_cast<double>(v[2])) * stride);
                const float h = static_cast<float>(std::exp(static_cast<double>(v[3])) * stride);
                const float x = (c + v[0]) * stride - w / 2.0f;
                const float y = (r + v[1]) * stride - h / 2.0f;
                require(std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h), "invalid Vulkan YuNet box");
                boxes.emplace_back(x, y, w, h);
                scores.push_back(score);
            }
        }
    }
    if (boxes.size() <= 1)
    {
        return boxes;
    }
    std::vector<cv::Rect> integerBoxes;
    for (const auto &box : boxes)
    {
        integerBoxes.emplace_back(static_cast<int>(box.x), static_cast<int>(box.y),
                                  static_cast<int>(box.width), static_cast<int>(box.height));
    }
    std::vector<int> keep;
    cv::dnn::NMSBoxes(integerBoxes, scores, scoreThreshold, 0.3f, keep, 1.0f, 300);
    std::vector<cv::Rect2d> result;
    for (const int index : keep)
    {
        result.push_back(boxes[index]);
    }
    return result;
}
}
