#pragma once
#include "ComputeBackend.h"
#include "ComputeFeatures.h"
#include "core/MatchingBatch.h"
#include "core/WorkerPool.h"
#include <opencv2/core.hpp>

namespace mosaicraft::compute
{
class FeatureWorkspace
{
    Device device;
    std::unique_ptr<Session> session;
    int width = 0, height = 0, rows = 0, blocks = 0;
    size_t host = 0, storage = 0;

  public:
    explicit FeatureWorkspace(Device description) : device(std::move(description))
    {
    }
    bool prepare(int w, int h, int maximum, size_t hostBudget, size_t deviceBudget);
    int capacity() const
    {
        return rows;
    }
    size_t hostBytes() const
    {
        return host;
    }
    size_t deviceBytes() const
    {
        return storage;
    }
    void extract(const std::vector<cv::Mat> &images, std::vector<Feature> &output, WorkerPool &workers,
                 const std::vector<ImageRecord *> &statistics = {});
    Timings timings() const;
};

class ScoreWorkspace
{
    Device device;
    std::unique_ptr<Session> session;
    const std::vector<ImageRecord> *records = nullptr;
    int rows = 0, candidates = 0;
    size_t library = 0, temporary = 0;

  public:
    explicit ScoreWorkspace(Device description) : device(std::move(description))
    {
    }
    bool prepare(int maximum, int count, size_t libraryCount, size_t hostBudget, size_t deviceBudget);
    int capacity() const
    {
        return rows;
    }
    size_t libraryBytes() const
    {
        return library;
    }
    void uploadLibrary(const std::vector<ImageRecord> &images, FeaturePack::Reader &reader);
    void score(const MatchingBuffers &tiles, int count, double penalty, double *scores);
    Timings timings() const;
};
} // namespace mosaicraft::compute
