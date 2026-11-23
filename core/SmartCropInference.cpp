#include "SmartCrop.h"

#if MOSAICRAFT_SMART_CROP
#include "ExecutablePath.h"
#include "SmartCropModels.h"
#include "third_party/picosha2/picosha2.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include "json.hpp"
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>
#endif

namespace mosaicraft
{
bool smartCropAvailable() noexcept
{
    return MOSAICRAFT_SMART_CROP;
}

#if MOSAICRAFT_SMART_CROP
namespace
{
constexpr int kForegroundSize = 320;
constexpr int kFaceSize = 640;

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

cv::dnn::Net cpuNet(const std::vector<unsigned char> &bytes)
{
    auto net = cv::dnn::readNetFromONNX(bytes);
    net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    return net;
}

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
    cv::dnn::Net saliency;
    cv::Ptr<cv::FaceDetectorYN> real;
    cv::dnn::Net anime;

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
        saliency.setInput(cv::dnn::blobFromImage(rgb));
        const auto out = saliency.forward("1959"); // Fused d1 output of this exact ONNX export.
        if (out.type() != CV_32F || out.dims != 4 || out.size[0] != 1 || out.size[1] != 1 ||
            out.size[2] != kForegroundSize || out.size[3] != kForegroundSize || !cv::checkRange(out, true, nullptr, 0, 1.001))
        {
            throw SmartCropError("smart_crop_inference", "unexpected U2-NetP foreground tensor");
        }
        return out.reshape(1, kForegroundSize).clone();
    }

    std::vector<cv::Rect2d> realFaces(const cv::Mat &image)
    {
        const double scale = std::min(1.0, static_cast<double>(kFaceSize) / std::max(image.cols, image.rows));
        const cv::Size size(std::max(1, cvRound(image.cols * scale)), std::max(1, cvRound(image.rows * scale)));
        cv::Mat reduced, detections;
        cv::resize(image, reduced, size, 0, 0, cv::INTER_AREA);
        real->setInputSize(size);
        real->detect(reduced, detections);
        if (!detections.empty() && (detections.type() != CV_32F || detections.cols != 15 || !cv::checkRange(detections)))
        {
            throw SmartCropError("smart_crop_inference", "unexpected YuNet detection tensor");
        }
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

    std::vector<cv::Rect2d> animeFaces(const cv::Mat &image)
    {
        const cv::Size size(kFaceSize, kFaceSize);
        cv::Mat resized;
        // The detector's fixed-size preprocessing is separate from the aspect-preserving output.
        cv::resize(image, resized, size, 0, 0, cv::INTER_CUBIC);
        anime.setInput(cv::dnn::blobFromImage(resized, 1.0 / 255, size, cv::Scalar(), true));
        const auto out = anime.forward("output0");
        if (out.type() != CV_32F || out.dims != 3 || out.size[0] != 1 || out.size[1] != 5 ||
            out.size[2] != 8400 || !cv::checkRange(out))
        {
            throw SmartCropError("smart_crop_inference", "unexpected Anime Face Detection tensor");
        }
        const auto rows = out.reshape(1, 5);
        const auto *x = rows.ptr<float>(0), *y = rows.ptr<float>(1);
        const auto *w = rows.ptr<float>(2), *h = rows.ptr<float>(3), *score = rows.ptr<float>(4);
        std::vector<cv::Rect2d> boxes;
        std::vector<float> scores;
        for (int i = 0; i < rows.cols; ++i)
        {
            if (score[i] > 0.25f)
            {
                const auto box = normalizedBox(cv::Rect2d(x[i] - w[i] / 2, y[i] - h[i] / 2, w[i], h[i]), size);
                if (!box.empty())
                {
                    boxes.push_back(box);
                    scores.push_back(score[i]);
                }
            }
        }
        std::vector<int> keep;
        cv::dnn::NMSBoxes(boxes, scores, 0.25f, 0.7f, keep, 1.f, 300);
        std::vector<cv::Rect2d> faces;
        for (const int i : keep)
        {
            faces.push_back(boxes[i]);
        }
        return faces;
    }
};

SmartCropModelPair loadSmartCropModelPair(CropMode mode)
{
    const auto manifest = nlohmann::json::parse(kSmartCropModelManifest);
    return SmartCropModelPair{mode, readModel(manifest, "saliency"),
                              readModel(manifest, mode == CropMode::SmartReal ? "real" : "anime")};
}

SmartCropInference::SmartCropInference(const SmartCropModelPair &models) try
    : m_impl(std::make_unique<Impl>())
{
    m_impl->saliency = cpuNet(models.saliency);
    if (models.mode == CropMode::SmartReal)
    {
        m_impl->real = cv::FaceDetectorYN::create("onnx", models.face, {}, cv::Size(kFaceSize, kFaceSize),
                                                0.9f, 0.3f, 300, cv::dnn::DNN_BACKEND_OPENCV, cv::dnn::DNN_TARGET_CPU);
    }
    else
    {
        m_impl->anime = cpuNet(models.face);
    }
}
catch (const cv::Exception &error)
{
    throw SmartCropError("smart_crop_model", std::string("OpenCV could not load the smart crop models: ") + error.what());
}

SmartCropAnalysis SmartCropInference::analyze(const cv::Mat &image) try
{
    return SmartCropAnalysis{m_impl->foreground(image), m_impl->real ? m_impl->realFaces(image) : m_impl->animeFaces(image)};
}
catch (const cv::Exception &error)
{
    throw SmartCropError("smart_crop_inference", std::string("OpenCV smart crop inference failed: ") + error.what());
}

void SmartCropInference::probe()
{
    // Force a forward pass as ONNX import alone cannot verify output compatibility.
    try
    {
        analyze(cv::Mat::zeros(320, 320, CV_8UC3));
    }
    catch (const SmartCropError &error)
    {
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

SmartCropInference::SmartCropInference(const SmartCropModelPair &)
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
