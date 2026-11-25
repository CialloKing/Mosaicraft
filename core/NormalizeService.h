#pragma once
#include "BuildService.h"
#include "ImageNormalizer.h"
#include <utility>
namespace mosaicraft
{
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
void runDualSmartCropPipeline(ImageNormalizer &cuda, ImageNormalizer &vulkan, std::size_t count,
                              const std::function<NormalizePaths(std::size_t)> &paths,
                              const std::function<void(bool)> &complete, ImageNormalizer::StandaloneTimings &timings,
                              std::size_t queueBytes = 256ull * 1024 * 1024,
                              std::size_t vulkanMaxPixels = kDualVulkanMaxPixels);
}
}
