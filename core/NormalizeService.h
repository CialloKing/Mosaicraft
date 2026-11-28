#pragma once
#include "BuildService.h"
#include "ImageNormalizer.h"
#include <cstdint>
#include <utility>
namespace mosaicraft
{
class SmartCropError;
int normalizationWorkerCount(int requestedThreads);
ServiceResult normalizeImages(const BuildRequest &request, RuntimeContext &runtime);
// Keep environment-based experiment selection confined to the legacy CLI benchmark path.
std::optional<ServiceResult> runCliSmartCropExperiment(const BuildRequest &request, RuntimeContext &runtime);

namespace detail
{
using NormalizePaths = std::pair<std::string, std::string>;
inline constexpr std::size_t kDualVulkanMaxPixels = 4ull * 1024 * 1024;
void runSmartCropPipeline(ImageNormalizer &normalizer, std::size_t count,
                          const std::function<NormalizePaths(std::size_t)> &paths,
                          const std::function<void(bool)> &complete, ImageNormalizer::StandaloneTimings &timings,
                          std::size_t queueBytes = 256ull * 1024 * 1024);
struct SameCudaCropPolicy
{
    std::function<bool()> primaryUsesCuda;
    std::function<bool()> canOverlap;
    std::function<bool(std::uint64_t)> keepSecondary;
    std::function<void()> releaseSecondary;
    std::function<void(const SmartCropError &)> stopCuda;
};
void runDualSmartCropPipeline(ImageNormalizer &cuda, ImageNormalizer &secondary, std::size_t count,
                              const std::function<NormalizePaths(std::size_t)> &paths,
                              const std::function<void(bool)> &complete, ImageNormalizer::StandaloneTimings &timings,
                              std::size_t queueBytes = 256ull * 1024 * 1024,
                              std::size_t secondaryMaxPixels = kDualVulkanMaxPixels,
                              const SameCudaCropPolicy *sameCuda = nullptr);
}
}
