// CudaStubs.cpp — 无 CUDA 时的空实现（在 MOSAICRAFT_CUDA=OFF 时编译）
#include "compute/CudaBackend.h"
#include <cfloat>
#include <cstdio>
#include <stdexcept>

namespace mosaicraft
{
namespace cuda
{

struct FeatureWorkspace::Impl
{
};
FeatureWorkspace::FeatureWorkspace() = default;
FeatureWorkspace::~FeatureWorkspace() = default;

bool FeatureWorkspace::preparePipeline(const FeaturePipelineConfig &, GpuTimings *)
{
    return false;
}
void FeatureWorkspace::resetPipeline()
{
}
std::uint8_t *FeatureWorkspace::pipelineInput()
{
    return nullptr;
}
std::size_t FeatureWorkspace::pinnedBytes() const
{
    return 0;
}
std::size_t FeatureWorkspace::residentBytes() const
{
    return 0;
}
void FeatureWorkspace::beginBatch(int)
{
    throw std::runtime_error("CUDA is not available");
}
void FeatureWorkspace::extract(int, int, double *, float *, std::uint8_t *, double *, float *, GpuTimings *)
{
    throw std::runtime_error("CUDA is not available");
}
DeviceFeatureView FeatureWorkspace::deviceView(int, int) const
{
    throw std::runtime_error("CUDA is not available");
}

struct ScoreWorkspace::Impl
{
};
ScoreWorkspace::ScoreWorkspace() = default;
ScoreWorkspace::~ScoreWorkspace() = default;

bool isCudaAvailable()
{
    return false;
}

bool uploadLibrary(GpuLibrary &, const double *, const float *, const std::uint8_t *, const double *, const float *,
                   const int *, int, GpuTimings *, const std::uint8_t *, const std::uint8_t *)
{
    return false;
}

void freeLibrary(GpuLibrary &)
{
}

int matchAgainstLibrary(double, double, double, const float *, const std::uint8_t *, double, const float *,
                        const GpuLibrary &, double, double, double, double, double, double)
{
    return -1;
}

void scoreIndices(double, double, double, const float *, const std::uint8_t *, double, const float *,
                  const GpuLibrary &, const int *, int count, double, double, double, double, double, double, double* scores)
{
    if (scores)
    {
        for (int i = 0; i < count; ++i)
        {
            scores[i] = DBL_MAX;
        }
    }
}

int matchWithIndices(double, double, double, const float *, const std::uint8_t *, double, const float *,
                     const GpuLibrary &, const int *, int, double, double, double, double, double, double)
{
    return -1;
}

bool scoreBatch(int, const double *, const double *, const double *, const float *, const std::uint8_t *,
                const double *, const float *, const int *, int, const GpuLibrary &, const double *, const double *,
                const double *, const double *, const double *, double, double *, GpuTimings *, std::size_t,
                ScoreWorkspace *)
{
    return false;
}

int extractTileFeatures(const std::uint8_t *, int, int, int, double *, float *, std::uint8_t *, double *, float *)
{
    return 0;
}

int extractTileFeatures(const std::uint8_t *, int, double *, float *, std::uint8_t *, double *, float *)
{
    return 0;
}

int extractFeaturesRaw(const std::uint8_t *, int, double *, float *, std::uint8_t *, double *, float *)
{
    return 0;
}

int extractFeaturesRaw(const std::uint8_t *, int, int, int, double *, float *, std::uint8_t *, double *, float *,
                       FeatureWorkspace *, GpuTimings *)
{
    return 0;
}

int matchOnGpu(double, double, double, const float *, const std::uint8_t *, double, const float *,
               const CandidateData &, double, double, double, double, double, double)
{
    return -1;
}

} // namespace cuda
} // namespace mosaicraft
