#include "compute/DeviceBuffer.cuh"
#include "compute/SmartCropCudaInput.h"
#include "doctest.h"

#include <cstring>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

TEST_CASE("CUDA anime input matches the OpenCV FP32 tensor for all byte values")
{
    using namespace mosaicraft::cuda;
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0)
    {
        MESSAGE("CUDA tensor parity requires a visible GPU");
        return;
    }

    const SmartCropInputLayout layout{257, 3, 288, 32, 15, 14};
    cv::Mat storage(layout.height, layout.width + 3, CV_8UC3);
    auto image = storage(cv::Rect(1, 0, layout.width, layout.height));
    auto ramp = image.row(0).colRange(0, 256);
    for (int x = 0; x < layout.width; ++x)
    {
        const auto byte = static_cast<std::uint8_t>(x);
        const auto inverse = static_cast<std::uint8_t>(255 - byte);
        image.ptr<cv::Vec3b>(0)[x] = {byte, byte, byte};
        image.ptr<cv::Vec3b>(1)[x] = {byte, inverse, 17};
        image.ptr<cv::Vec3b>(2)[x] = {89, byte, inverse};
    }
    const auto referenceRamp = cv::dnn::blobFromImage(ramp, 1.0 / 255, ramp.size(), cv::Scalar(), true);
    SmartCropPixelValues lookup;
    std::memcpy(lookup.values, referenceRamp.data, sizeof(lookup.values));

    cv::Mat padded;
    cv::copyMakeBorder(image, padded, layout.top, layout.paddedHeight - image.rows - layout.top,
                       layout.left, layout.paddedWidth - image.cols - layout.left,
                       cv::BORDER_CONSTANT | cv::BORDER_ISOLATED, cv::Scalar::all(114));
    const auto reference = cv::dnn::blobFromImage(padded, 1.0 / 255, padded.size(), cv::Scalar(), true);
    const auto packed = image.clone();
    const auto inputBytes = packed.total() * packed.elemSize();
    const auto outputBytes = reference.total() * reference.elemSize();
    DeviceBuffer<std::uint8_t> bgr;
    DeviceBuffer<float> rgb;
    REQUIRE(bgr.allocate(inputBytes) == cudaSuccess);
    REQUIRE(rgb.allocate(outputBytes) == cudaSuccess);
    REQUIRE(cudaMemcpy(bgr.get(), packed.data, inputBytes, cudaMemcpyHostToDevice) == cudaSuccess);
    REQUIRE(launchSmartCropCudaInput(bgr.get(), rgb.get(), layout, lookup, nullptr) == cudaSuccess);
    auto actual = reference.clone();
    REQUIRE(cudaMemcpy(actual.data, rgb.get(), outputBytes, cudaMemcpyDeviceToHost) == cudaSuccess);
    CHECK(std::memcmp(actual.data, reference.data, outputBytes) == 0);
}
