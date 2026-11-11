#pragma once

// 尺寸判定、内核分派和启动检查共用名单，避免选中未实例化的内核。
#define MOSAICRAFT_CUDA_TEMPLATE_SIZES(X) \
    X(180, 320)                          \
    X(320, 180)                          \
    X(360, 640)                          \
    X(640, 360)                          \
    X(256, 256)                          \
    X(512, 512)                          \
    X(270, 480)                          \
    X(480, 270)                          \
    X(540, 960)                          \
    X(960, 540)

namespace mosaicraft
{
namespace cuda
{
struct TemplateFeatureSize
{
    int width;
    int height;
};

inline constexpr TemplateFeatureSize templateFeatureSizes[] =
{
#define FEATURE_SIZE(W, H) {W, H},
    MOSAICRAFT_CUDA_TEMPLATE_SIZES(FEATURE_SIZE)
#undef FEATURE_SIZE
};

inline constexpr bool hasTemplateFeatures(int width, int height)
{
    for (const auto &size : templateFeatureSizes)
    {
        if (width == size.width && height == size.height)
        {
            return true;
        }
    }
    return false;
}
} // namespace cuda
} // namespace mosaicraft
