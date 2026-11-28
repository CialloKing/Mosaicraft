#include "SmartCrop.h"

#if MOSAICRAFT_SMART_CROP
#include "ExecutablePath.h"
#include "SmartCropModels.h"
#include "third_party/picosha2/picosha2.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <utility>
#include "json.hpp"
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>
#if MOSAICRAFT_SMART_CROP_VULKAN
#include "SmartCropVulkan.h"
#endif
#if MOSAICRAFT_SMART_CROP_CUDA
#include <opencv2/core/cuda.hpp>
#endif
#if MOSAICRAFT_SMART_CROP_CUTLASS && defined(CV_DNN_MOSAICRAFT_CUDA_INPUT)
#include "compute/DeviceBuffer.cuh"
#include "compute/SmartCropCudaInput.h"
#endif
#endif

namespace mosaicraft
{
#if MOSAICRAFT_SMART_CROP_CUDA
namespace
{
SmartCropCudaFault cudaFault(const cv::Exception &error)
{
    // OpenCV wraps the NVIDIA status in a cv::Exception and does not retain the numeric code.
    const std::string_view message(error.what());
    for (const auto *fatal : {"an illegal memory access", "device-side assert", "unspecified launch failure",
                              "launch timed out", "context is destroyed", "context has been destroyed",
                              "CUDA_ERROR_ILLEGAL_ADDRESS", "CUDA_ERROR_ASSERT", "CUDA_ERROR_LAUNCH_FAILED",
                              "CUDA_ERROR_LAUNCH_TIMEOUT", "CUDA_ERROR_CONTEXT_IS_DESTROYED",
                              "misaligned address", "hardware stack error", "illegal instruction",
                              "invalid address space", "invalid program counter"})
    {
        if (message.find(fatal) != std::string_view::npos)
        {
            return SmartCropCudaFault::RestartRequired;
        }
    }
    return SmartCropCudaFault::Retryable;
}
}
#endif

bool smartCropAvailable() noexcept
{
    return MOSAICRAFT_SMART_CROP;
}

bool smartCropCudaAvailable() noexcept
{
    return MOSAICRAFT_SMART_CROP_CUDA;
}

void requireSmartCropDevice(CropDevice device)
{
    if (device != CropDevice::Cuda)
    {
        return;
    }
#if MOSAICRAFT_SMART_CROP_CUDA
    try
    {
        if (cv::cuda::getCudaEnabledDeviceCount() <= 0 || !cv::cuda::DeviceInfo().isCompatible())
        {
            throw SmartCropError("smart_crop_cuda_device", "no CUDA device with a compatible driver and OpenCV architecture",
                                 SmartCropCudaFault::Retryable);
        }
        const auto targets = cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_CUDA);
        if (std::find(targets.begin(), targets.end(), cv::dnn::DNN_TARGET_CUDA) == targets.end())
        {
            throw SmartCropError("smart_crop_cuda_device", "OpenCV CUDA DNN FP32 is unavailable", SmartCropCudaFault::Retryable);
        }
    }
    catch (const cv::Exception &error)
    {
        throw SmartCropError("smart_crop_cuda_device", std::string("CUDA smart crop device check failed: ") + error.what(),
                             cudaFault(error));
    }
#else
    throw SmartCropError("smart_crop_cuda_unavailable", "this build does not support CUDA smart crop");
#endif
}

#if MOSAICRAFT_SMART_CROP
namespace
{
constexpr int kForegroundSize = 320;
// Non-square and not stride-aligned so the preflight exercises detector padding.
const cv::Size kFaceProbeSize(352, 239);
// The locked anime model concatenates one prediction per cell at these three strides.
constexpr int kAnimeStrides[] = {8, 16, 32};
constexpr int kAnimeAlignment = kAnimeStrides[2];
constexpr float kAnimeMinFaceScore = 0.25f;
constexpr float kAnimeConfidentFaceScore = 0.50f;
constexpr int kAnimeRecoveryLongEdge = 1024;

struct AnimeDetections
{
    std::vector<cv::Rect2d> faces;
    float strongest = 0.0f;
};

int alignAnimeInput(int length)
{
    return ((length + kAnimeAlignment - 1) / kAnimeAlignment) * kAnimeAlignment;
}

int animePredictionCount(cv::Size input)
{
    int count = 0;
    for (const int stride : kAnimeStrides)
    {
        count += (input.width / stride) * (input.height / stride);
    }
    return count;
}

#if MOSAICRAFT_SMART_CROP_VULKAN
cv::Rect2d decodeAnimeGrid(int index, cv::Size input, cv::Point padding, float l, float t, float r, float b)
{
    for (const int stride : kAnimeStrides)
    {
        const int columns = input.width / stride;
        const int cells = columns * (input.height / stride);
        if (index >= cells)
        {
            index -= cells;
            continue;
        }
        // Recreate the locked export's grid and float operation order, only for hits.
        const float ax = index % columns + 0.5f, ay = index / columns + 0.5f;
        const float left = ax - l, top = ay - t, right = ax + r, bottom = ay + b;
        const float w = (right - left) * stride, h = (bottom - top) * stride;
        return cv::Rect2d((left + right) / 2 * stride - w / 2 - padding.x,
                          (top + bottom) / 2 * stride - h / 2 - padding.y, w, h);
    }
    return {};
}
#endif

std::vector<unsigned char> readModel(const nlohmann::json &manifest, const char *id)
{
    for (const auto &model : manifest.at("models"))
    {
        if (model.at("id") != id)
        {
            continue;
        }
        const auto name = model.at("file").get<std::string>();
        const auto size = model.at("size").get<size_t>();
        std::ifstream input(executableDirectory() / "models" / name, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() != static_cast<std::streamoff>(size))
        {
            throw SmartCropError("smart_crop_model", "missing or wrong-sized smart crop model: " + name);
        }
        std::vector<unsigned char> bytes(size);
        input.seekg(0);
        // Validate the very bytes used to initialize DNN, not a second read of the model.
        if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)) ||
            picosha2::hash256_hex_string(bytes) != model.at("sha256").get<std::string>())
        {
            throw SmartCropError("smart_crop_model", "SHA-256 verification failed for smart crop model: " + name);
        }
        return bytes;
    }
    throw SmartCropError("smart_crop_model", std::string("model missing from the built-in manifest: ") + id);
}

cv::dnn::Net modelNet(const std::vector<unsigned char> &bytes, CropDevice device)
{
    // The OpenCV 5 graph engine preserves the original ONNX input dimensions.
    auto net = cv::dnn::readNetFromONNX(bytes, cv::dnn::ENGINE_OPENCV);
#if MOSAICRAFT_SMART_CROP_CUDA
    if (device == CropDevice::Cuda)
    {
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
        net.enableTF32(false);
    }
#else
    (void)device;
#endif
    return net;
}

#if MOSAICRAFT_SMART_CROP_CUDA
void requireCudaConvolutions(const cv::Ptr<const cv::dnn::Graph> &graph, const char *model)
{
    size_t convolutionCount = 0;
    size_t cudaConvolutionCount = 0;
    std::string firstUnsupported;
    if (graph)
    {
        const auto &ops = graph->prog();
        for (size_t i = 0; i < ops.size(); ++i)
        {
            if (ops[i] && ops[i]->type == "Conv2")
            {
                ++convolutionCount;
                if (graph->opBackend(static_cast<int>(i)) == cv::dnn::DNN_BACKEND_CUDA)
                {
                    ++cudaConvolutionCount;
                }
                else if (firstUnsupported.empty())
                {
                    firstUnsupported = "op=" + std::to_string(i) + ", backend=" +
                        std::to_string(graph->opBackend(static_cast<int>(i)));
                }
            }
        }
    }
    if (convolutionCount == 0 || cudaConvolutionCount != convolutionCount)
    {
        // OpenCV can execute a whole unsupported graph on the CPU even after setPreferableTarget().
        const auto detail = " (CUDA Conv2 nodes: " + std::to_string(cudaConvolutionCount) + "/" +
            std::to_string(convolutionCount) + (firstUnsupported.empty() ? "" : "; first unsupported " + firstUnsupported) + ")";
        throw SmartCropError("smart_crop_cuda_fallback", std::string(model) + " did not execute all convolutions on CUDA" + detail,
                             SmartCropCudaFault::Retryable);
    }
}
#endif

template<class Forward>
auto runDnn(CropDevice device, Forward &&forward)
{
#if MOSAICRAFT_SMART_CROP_CUDA
    if (device == CropDevice::Cuda)
    {
        try
        {
            return forward();
        }
        catch (const cv::Exception &error)
        {
            // The locked OpenCV backend reports CUDA, cuBLAS and cuDNN failures with GPU status codes.
            // The same forward pass can also throw graph/shape errors or fail a host allocation.
            if (error.code != cv::Error::GpuApiCallError && error.code != cv::Error::GpuNotSupported)
            {
                throw;
            }
            throw SmartCropError("smart_crop_cuda_inference", error.what(), cudaFault(error));
        }
    }
#else
    (void)device;
#endif
    return forward();
}

#if MOSAICRAFT_SMART_CROP_CUTLASS && defined(CV_DNN_MOSAICRAFT_CUDA_INPUT)
enum class AnimeCudaInputMode
{
    Legacy, Pinned, Fused
};

AnimeCudaInputMode animeCudaInputMode()
{
    const auto *value = std::getenv("MOSAICRAFT_SMART_CROP_CUDA_INPUT");
    const std::string_view mode = value ? value : "legacy";
    if (mode == "pinned")
    {
        return AnimeCudaInputMode::Pinned;
    }
    if (mode == "fused")
    {
        return AnimeCudaInputMode::Fused;
    }
    return AnimeCudaInputMode::Legacy;
}

void requireCropInputCuda(cudaError_t status)
{
    if (status != cudaSuccess)
    {
        CV_Error(cv::Error::GpuApiCallError, cudaGetErrorString(status));
    }
}

class AnimeCudaInput
{
public:
    template <class Pad>
    std::optional<cv::Mat> forward(cv::dnn::Net &net, const cv::Mat &image, cv::Size size, cv::Point padding, Pad &&pad)
    {
        const auto pixels = static_cast<std::uint64_t>(size.width) * size.height;
        if (m_mode == AnimeCudaInputMode::Legacy || m_disabled || pixels > 4'000'000 || image.type() != CV_8UC3)
        {
            return std::nullopt;
        }
        // The host buffer is reused only after synchronous forward has consumed it.
        const bool fused = m_mode == AnimeCudaInputMode::Fused;
        const std::size_t bytes = fused ? image.total() * image.elemSize() : pixels * 3 * sizeof(float);
        if (!reserve(m_pinned.allocate(bytes)) || (fused && !reserve(m_raw.allocate(bytes))))
        {
            m_disabled = true;
            m_pinned.reset();
            m_raw.reset();
            return std::nullopt;
        }

        Transfer input{m_pinned.get(), bytes, nullptr, {}, nullptr};
        if (fused)
        {
            const auto rowBytes = static_cast<std::size_t>(image.cols) * image.elemSize();
            for (int y = 0; y < image.rows; ++y)
            {
                std::memcpy(m_pinned.get() + y * rowBytes, image.ptr(y), rowBytes);
            }
            input.rawDevice = m_raw.get();
            input.layout = {image.cols, image.rows, size.width, size.height, padding.x, padding.y};
            input.lookup = &pixelValues();
        }
        else
        {
            const int shape[] = {1, 3, size.height, size.width};
            cv::Mat blob(4, shape, CV_32F, m_pinned.get());
            cv::dnn::blobFromImage(pad(), blob, 1.0 / 255, size, cv::Scalar(), true);
            if (blob.data != m_pinned.get())
            {
                std::memcpy(m_pinned.get(), blob.data, bytes);
            }
        }
        return net.forwardMosaicraftCudaInput(size, &upload, &input, "output0");
    }

private:
    struct Transfer
    {
        const std::uint8_t *source;
        std::size_t bytes;
        std::uint8_t *rawDevice;
        cuda::SmartCropInputLayout layout;
        const cuda::SmartCropPixelValues *lookup;
    };

    static bool reserve(cudaError_t status)
    {
        if (status == cudaErrorMemoryAllocation)
        {
            cudaGetLastError();
            return false;
        }
        requireCropInputCuda(status);
        return true;
    }

    static const cuda::SmartCropPixelValues &pixelValues()
    {
        static const auto values = []()
        {
            // Use OpenCV's exact FP32 values, including the normalization of the padding byte.
            cv::Mat ramp(1, 256, CV_8UC3);
            for (int value = 0; value < ramp.cols; ++value)
            {
                const auto byte = static_cast<std::uint8_t>(value);
                ramp.at<cv::Vec3b>(0, value) = cv::Vec3b(byte, byte, byte);
            }
            const auto blob = cv::dnn::blobFromImage(ramp, 1.0 / 255, ramp.size(), cv::Scalar(), true);
            cuda::SmartCropPixelValues result;
            std::copy_n(blob.ptr<float>(), 256, result.values);
            return result;
        }();
        return values;
    }

    static void upload(void *opaque, float *destination, void *nativeStream)
    {
        const auto &input = *static_cast<const Transfer *>(opaque);
        const auto stream = static_cast<cudaStream_t>(nativeStream);
        void *uploadTarget = input.rawDevice ? static_cast<void *>(input.rawDevice) : destination;
        requireCropInputCuda(cudaMemcpyAsync(uploadTarget, input.source, input.bytes, cudaMemcpyHostToDevice, stream));
        if (input.rawDevice)
        {
            requireCropInputCuda(cuda::launchSmartCropCudaInput(input.rawDevice, destination, input.layout, *input.lookup, stream));
        }
    }

    const AnimeCudaInputMode m_mode = animeCudaInputMode();
    bool m_disabled = false;
    cuda::PinnedBuffer m_pinned;
    cuda::DeviceBuffer<std::uint8_t> m_raw;
};
#endif

cv::Rect2d normalizedBox(cv::Rect2d box, cv::Size input)
{
    box &= cv::Rect2d(0, 0, input.width, input.height);
    if (box.width < 12 || box.height < 12)
    {
        return {};
    }
    return cv::Rect2d(box.x / input.width, box.y / input.height, box.width / input.width, box.height / input.height);
}
}

struct SmartCropInference::Impl
{
    size_t optimizedCudaInputs = 0;
    CropDevice device = CropDevice::Cpu;
    CropMode mode = CropMode::Center;
#if MOSAICRAFT_SMART_CROP_CUTLASS && defined(CV_DNN_MOSAICRAFT_CUDA_INPUT)
    // Keep the staging buffers alive until the DNN streams have been destroyed.
    AnimeCudaInput cudaInput;
#endif
    cv::dnn::Net saliency;
    cv::Ptr<cv::FaceDetectorYN> real;
    cv::dnn::Net anime;
#if MOSAICRAFT_SMART_CROP_VULKAN
    std::unique_ptr<SmartCropVulkan> vulkan;
#endif

    cv::Mat foreground(const cv::Mat &image)
    {
        cv::Mat rgb;
        cv::resize(image, rgb, cv::Size(kForegroundSize, kForegroundSize), 0, 0, cv::INTER_LANCZOS4);
        cv::cvtColor(rgb, rgb, cv::COLOR_BGR2RGB);
        double peak = 0;
        cv::minMaxLoc(rgb.reshape(1), nullptr, &peak);
        rgb.convertTo(rgb, CV_32F, 1.0 / std::max(peak, 1.0));
        // Match the rembg export's per-image maximum and per-channel normalization.
        std::vector<cv::Mat> channels;
        cv::split(rgb, channels);
        const float mean[] = {0.485f, 0.456f, 0.406f};
        const float deviation[] = {0.229f, 0.224f, 0.225f};
        for (int i = 0; i < 3; ++i)
        {
            channels[i].convertTo(channels[i], CV_32F, 1.0 / deviation[i], -mean[i] / deviation[i]);
        }
        cv::merge(channels, rgb);
        const auto blob = cv::dnn::blobFromImage(rgb);
        const auto out = [&]()
        {
#if MOSAICRAFT_SMART_CROP_VULKAN
            if (vulkan)
            {
                return vulkan->foreground(blob);
            }
#endif
            saliency.setInput(blob);
            // Fused d1 output of this exact ONNX export.
            return runDnn(device, [&]()
            {
                return saliency.forward("1959");
            });
        }();
        if (out.type() != CV_32F || out.dims != 4 || out.size[0] != 1 || out.size[1] != 1 ||
            out.size[2] != kForegroundSize || out.size[3] != kForegroundSize || !cv::checkRange(out, true, nullptr, 0, 1.001))
        {
            throw SmartCropError("smart_crop_inference", "unexpected U2-NetP foreground tensor");
        }
#if MOSAICRAFT_SMART_CROP_CUDA
        if (device == CropDevice::Cuda)
        {
            requireCudaConvolutions(saliency.getMainGraph(), "U2-NetP");
        }
#endif
        return out.reshape(1, kForegroundSize).clone();
    }

    std::vector<cv::Rect2d> realFaces(const cv::Mat &image)
    {
        const cv::Size size = image.size();
#if MOSAICRAFT_SMART_CROP_VULKAN
        if (vulkan)
        {
            std::vector<cv::Rect2d> faces;
            for (const auto &raw : vulkan->realBoxes(image))
            {
                const auto box = normalizedBox(raw, size);
                if (!box.empty())
                {
                    faces.push_back(box);
                }
            }
            return faces;
        }
#endif
        // YuNet pads internally on the right and bottom, so its boxes stay in the original coordinates.
        cv::Mat detections;
        real->setInputSize(size);
        runDnn(device, [&]()
        {
            return real->detect(image, detections);
        });
        if (!detections.empty() && (detections.type() != CV_32F || detections.cols != 15 || !cv::checkRange(detections)))
        {
            throw SmartCropError("smart_crop_inference", "unexpected YuNet detection tensor");
        }
#if MOSAICRAFT_SMART_CROP_CUDA
        if (device == CropDevice::Cuda)
        {
            requireCudaConvolutions(real->getDnnGraph(), "YuNet");
        }
#endif
        std::vector<cv::Rect2d> faces;
        for (int i = 0; i < detections.rows; ++i)
        {
            const auto *row = detections.ptr<float>(i);
            const auto box = normalizedBox(cv::Rect2d(row[0], row[1], row[2], row[3]), size);
            if (!box.empty())
            {
                faces.push_back(box);
            }
        }
        return faces;
    }

    AnimeDetections detectAnimeFaces(const cv::Mat &image, float minScore)
    {
        const cv::Size content = image.size();
        const cv::Size input(alignAnimeInput(content.width), alignAnimeInput(content.height));
        const int extraWidth = input.width - content.width;
        const int extraHeight = input.height - content.height;
        const cv::Point padding(extraWidth / 2, extraHeight / 2);
        const auto pad = [&]()
        {
            cv::Mat padded = image;
            if (extraWidth || extraHeight)
            {
                // Keep the original pixels; only the model tensor needs stride-aligned dimensions.
                cv::copyMakeBorder(image, padded, padding.y, extraHeight - padding.y,
                                   padding.x, extraWidth - padding.x, cv::BORDER_CONSTANT | cv::BORDER_ISOLATED,
                                   cv::Scalar::all(114));
            }
            return padded;
        };
        const int anchors = animePredictionCount(input);
        const auto out = [&]()
        {
#if MOSAICRAFT_SMART_CROP_CUTLASS && defined(CV_DNN_MOSAICRAFT_CUDA_INPUT)
            if (device == CropDevice::Cuda)
            {
                auto fast = runDnn(device, [&]()
                {
                    return cudaInput.forward(anime, image, input, padding, pad);
                });
                if (fast)
                {
                    ++optimizedCudaInputs;
                    return std::move(*fast);
                }
            }
#endif
            const auto blob = cv::dnn::blobFromImage(pad(), 1.0 / 255, input, cv::Scalar(), true);
#if MOSAICRAFT_SMART_CROP_VULKAN
            if (vulkan)
            {
                return vulkan->animeDistances(blob, anchors);
            }
#endif
            anime.setInput(blob);
            return runDnn(device, [&]()
            {
                return anime.forward("output0");
            });
        }();
        if (out.type() != CV_32F || out.dims != 3 || out.size[0] != 1 || out.size[1] != 5 ||
            out.size[2] != anchors || !cv::checkRange(out))
        {
            throw SmartCropError("smart_crop_inference", "unexpected Anime Face Detection tensor for " +
                                 std::to_string(input.width) + "x" + std::to_string(input.height));
        }
#if MOSAICRAFT_SMART_CROP_CUDA
        if (device == CropDevice::Cuda)
        {
            requireCudaConvolutions(anime.getMainGraph(), "DeepGHS");
        }
#endif
        const auto rows = out.reshape(1, 5);
        const auto *x = rows.ptr<float>(0), *y = rows.ptr<float>(1);
        const auto *w = rows.ptr<float>(2), *h = rows.ptr<float>(3), *score = rows.ptr<float>(4);
        std::vector<cv::Rect2d> boxes;
        std::vector<float> scores;
        for (int i = 0; i < rows.cols; ++i)
        {
            if (score[i] > minScore)
            {
                // Return fractions of the picture, not of the padded detector input.
                const auto box = normalizedBox(
#if MOSAICRAFT_SMART_CROP_VULKAN
                    vulkan ? decodeAnimeGrid(i, input, padding, x[i], y[i], w[i], h[i]) :
#endif
                    cv::Rect2d(x[i] - w[i] / 2 - padding.x, y[i] - h[i] / 2 - padding.y, w[i], h[i]), content);
                if (!box.empty())
                {
                    boxes.push_back(box);
                    scores.push_back(score[i]);
                }
            }
        }
        std::vector<int> keep;
        cv::dnn::NMSBoxes(boxes, scores, minScore, 0.7f, keep, 1.f, 300);
        AnimeDetections detections;
        detections.faces.reserve(keep.size());
        for (const int i : keep)
        {
            detections.faces.push_back(boxes[i]);
            detections.strongest = std::max(detections.strongest, scores[i]);
        }
        return detections;
    }

    std::vector<cv::Rect2d> animeFaces(const cv::Mat &image)
    {
        auto native = detectAnimeFaces(image, kAnimeMinFaceScore);
        const int longEdge = std::max(image.cols, image.rows);
        if (native.strongest < kAnimeConfidentFaceScore && longEdge >= 2 * kAnimeRecoveryLongEdge)
        {
            // A close-up drawn across hundreds of pixels can miss the native detector's scale range.
            // Keep native small-face recall; let a second scale replace weak hits only with confident ones.
            const double scale = static_cast<double>(kAnimeRecoveryLongEdge) / longEdge;
            const cv::Size size(std::max(1, cvRound(image.cols * scale)),
                                std::max(1, cvRound(image.rows * scale)));
            cv::Mat overview;
            cv::resize(image, overview, size, 0, 0, cv::INTER_AREA);
            auto recovered = detectAnimeFaces(overview, kAnimeConfidentFaceScore);
            if (!recovered.faces.empty())
            {
                return std::move(recovered.faces);
            }
        }
        return std::move(native.faces);
    }
};

SmartCropModelPair loadSmartCropModelPair(CropMode mode)
{
    const auto manifest = nlohmann::json::parse(kSmartCropModelManifest);
    return SmartCropModelPair{mode, readModel(manifest, "saliency"),
                              readModel(manifest, mode == CropMode::SmartReal ? "real" : "anime")};
}

SmartCropInference::SmartCropInference(const SmartCropModelPair &models, CropDevice device, std::optional<int> vulkan) try
    : m_impl(std::make_unique<Impl>())
{
    m_impl->device = device;
    m_impl->mode = models.mode;
    if (vulkan)
    {
#if MOSAICRAFT_SMART_CROP_VULKAN
        m_impl->vulkan = std::make_unique<SmartCropVulkan>(*vulkan, models.mode);
        return;
#else
        throw SmartCropError("smart_crop_vulkan_unavailable", "this build does not support the Vulkan smart crop experiment");
#endif
    }
    m_impl->saliency = modelNet(models.saliency, device);
    if (models.mode == CropMode::SmartReal)
    {
        const bool cuda = device == CropDevice::Cuda;
        m_impl->real = cv::FaceDetectorYN::create("onnx", models.face, {}, kFaceProbeSize, 0.9f, 0.3f, 300,
                                                cuda ? cv::dnn::DNN_BACKEND_CUDA : cv::dnn::DNN_BACKEND_OPENCV,
                                                cuda ? cv::dnn::DNN_TARGET_CUDA : cv::dnn::DNN_TARGET_CPU);
#if MOSAICRAFT_SMART_CROP_CUDA
        if (cuda)
        {
            m_impl->real->enableDnnTF32(false);
        }
#endif
    }
    else
    {
        m_impl->anime = modelNet(models.face, device);
    }
}
catch (const cv::Exception &error)
{
    throw SmartCropError("smart_crop_model", std::string("OpenCV could not load the smart crop models: ") + error.what());
}

SmartCropAnalysis SmartCropInference::analyze(const cv::Mat &image) try
{
    using Clock = std::chrono::steady_clock;
    const auto previousOptimizedInputs = m_impl->optimizedCudaInputs;
    const auto start = Clock::now();
    auto mask = m_impl->foreground(image);
    const auto afterMask = Clock::now();
    auto faces = m_impl->mode == CropMode::SmartReal ? m_impl->realFaces(image) : m_impl->animeFaces(image);
    return SmartCropAnalysis{std::move(mask), std::move(faces),
                             std::chrono::duration<double, std::milli>(afterMask - start).count(),
                             std::chrono::duration<double, std::milli>(Clock::now() - afterMask).count(),
                             m_impl->mode, m_impl->optimizedCudaInputs - previousOptimizedInputs};
}
catch (const cv::Exception &error)
{
    throw SmartCropError("smart_crop_inference", std::string("OpenCV smart crop inference failed for ") +
                             std::to_string(image.cols) + "x" + std::to_string(image.rows) + ": " + error.what());
}
catch (const std::bad_alloc &)
{
    throw SmartCropError("smart_crop_inference", "not enough memory for smart crop inference");
}

void SmartCropInference::probe()
{
    // Force a forward pass as ONNX import alone cannot verify output compatibility.
    try
    {
        analyze(cv::Mat::zeros(kFaceProbeSize, CV_8UC3));
    }
    catch (const SmartCropError &error)
    {
        if (error.cudaFault() != SmartCropCudaFault::None)
        {
            throw;
        }
        throw SmartCropError("smart_crop_model", error.what());
    }
}
#else
struct SmartCropInference::Impl
{
};

SmartCropModelPair loadSmartCropModelPair(CropMode)
{
    throw SmartCropError("smart_crop_unavailable", "this build does not support smart crop");
}

SmartCropInference::SmartCropInference(const SmartCropModelPair &, CropDevice, std::optional<int>)
{
    throw SmartCropError("smart_crop_unavailable", "this build does not support smart crop");
}

SmartCropAnalysis SmartCropInference::analyze(const cv::Mat &)
{
    throw SmartCropError("smart_crop_unavailable", "this build does not support smart crop");
}

void SmartCropInference::probe()
{
    throw SmartCropError("smart_crop_unavailable", "this build does not support smart crop");
}
#endif

SmartCropInference::~SmartCropInference() = default;
}
