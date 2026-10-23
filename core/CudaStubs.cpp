#include "compute/CompatibleScoring.h"
// CudaStubs.cpp — 无 CUDA 时的空实现（在 MOSAICRAFT_CUDA=OFF 时编译）
#include "compute/CudaBackend.h"
#include "compute/CompatibleFeatures.h"
#include <cfloat>
#include <cstdio>
#include <stdexcept>

namespace mosaicraft
{
namespace cuda
{

struct CompatibleScoreWorkspace::Impl
{
};
CompatibleScoreWorkspace::CompatibleScoreWorkspace() = default;
CompatibleScoreWorkspace::~CompatibleScoreWorkspace() = default;
bool CompatibleScoreWorkspace::prepare(int, int, int, size_t, size_t, CompatibleScoreFault)
{
    return false;
}
int CompatibleScoreWorkspace::capacity() const
{
    return 0;
}
size_t CompatibleScoreWorkspace::hostBytes() const
{
    return 0;
}
size_t CompatibleScoreWorkspace::deviceBytes() const
{
    return 0;
}
size_t CompatibleScoreWorkspace::libraryBytes() const
{
    return 0;
}
const char *CompatibleScoreWorkspace::preparationFailure() const
{
    return "CUDA is not available";
}
void CompatibleScoreWorkspace::uploadLibrary(const std::vector<ImageRecord> &, FeaturePack::Reader &, GpuTimings *)
{
    throw std::runtime_error("CUDA is not available");
}
void CompatibleScoreWorkspace::score(const MatchingBuffers &, int, double, double *, GpuTimings *)
{
    throw std::runtime_error("CUDA is not available");
}

struct CompatibleFeatureWorkspace::Impl
{
};
CompatibleFeatureWorkspace::CompatibleFeatureWorkspace() = default;
CompatibleFeatureWorkspace::~CompatibleFeatureWorkspace() = default;
bool CompatibleFeatureWorkspace::prepare(int, int, int, size_t, bool, size_t, PipelineFault)
{
    return false;
}
int CompatibleFeatureWorkspace::capacity() const
{
    return 0;
}
size_t CompatibleFeatureWorkspace::hostBytes() const
{
    return 0;
}
size_t CompatibleFeatureWorkspace::deviceBytes() const
{
    return 0;
}
void CompatibleFeatureWorkspace::extract(const std::vector<cv::Mat> &, std::vector<CompatibleFeature> &,
                                         WorkerPool &, const std::vector<ImageRecord *> &, CompatibleTimings *)
{
    throw std::runtime_error("CUDA is not available");
}

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

bool ScoreWorkspace::preparePipeline(int, int, std::size_t, std::size_t, std::size_t, PipelineFault)
{
    return false;
}
void ScoreWorkspace::resetPipeline()
{
}
std::size_t ScoreWorkspace::pinnedBytes() const
{
    return 0;
}
bool scoreDeviceBatch(const DeviceFeatureView &, const double *, const double *, const double *, const double *,
                      const int *, int, const GpuLibrary &, const double *, const double *, const double *,
                      const double *, const double *, double, double *, ScoreWorkspace &, GpuTimings *, std::size_t)
{
    return false;
}

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
