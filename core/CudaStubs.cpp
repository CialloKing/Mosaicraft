// CudaStubs.cpp — 无 CUDA 时的空实现（在 MOSAICRAFT_CUDA=OFF 时编译）
#include "compute/CudaBackend.h"
#include <cstdio>

namespace mosaicraft {
namespace cuda {

bool isCudaAvailable() { return false; }

bool uploadLibrary(GpuLibrary&,
    const double*, const float*, const std::uint8_t*,
    const double*, const float*, const int*, int) { return false; }

void freeLibrary(GpuLibrary&) {}

int matchAgainstLibrary(double,double,double,const float*,const std::uint8_t*,
    double,const float*,const GpuLibrary&,double,double,double,double,double,double) { return -1; }

void scoreIndices(double,double,double,const float*,const std::uint8_t*,
    double,const float*,const GpuLibrary&,const int*,int,
    double,double,double,double,double,double,double*) {}

int matchWithIndices(double,double,double,const float*,const std::uint8_t*,
    double,const float*,const GpuLibrary&,const int*,int,
    double,double,double,double,double,double) { return -1; }

void scoreBatch(int,const double*,const double*,const double*,
    const float*,const std::uint8_t*,const double*,const float*,
    const int*,int,const GpuLibrary&,
    const double*,const double*,const double*,const double*,const double*,
    double,double*) {}

int extractTileFeatures(const std::uint8_t*, int, int, int,
    double*, float*, std::uint8_t*, double*, float*) { return 0; }

int extractTileFeatures(const std::uint8_t*, int,
    double*, float*, std::uint8_t*, double*, float*) { return 0; }

int extractFeaturesRaw(const std::uint8_t*, int,
    double*, float*, std::uint8_t*, double*, float*) { return 0; }

int extractFeaturesRaw(const std::uint8_t*, int, int, int,
    double*, float*, std::uint8_t*, double*, float*) { return 0; }

int matchOnGpu(double,double,double,const float*,const std::uint8_t*,
    double,const float*,const CandidateData&,
    double,double,double,double,double,double) { return -1; }

} // namespace cuda
} // namespace mosaicraft
