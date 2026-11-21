#pragma once

#include <cstddef>
#include <vector>

namespace mosaicraft
{
struct ImageRecord;
struct MosaicRequest;
struct MosaicSelection;
struct PreviewScores;
struct PreviewWorkspace;
struct ServiceResult;

enum class PreviewScoreSource
{
    PreviewCache, CpuRecomputed
};

struct PreviewCandidate
{
    int imageId;
    double baseScore, rankScore;
};

struct PreviewCandidateRow
{
    PreviewScoreSource source = PreviewScoreSource::PreviewCache;
    std::vector<PreviewCandidate> candidates;
};

// 请求和工作区来自同一份成功预览；换图只改变该选择的邻域。
ServiceResult previewCandidates(const MosaicRequest &request, const MosaicSelection &selection,
                                const PreviewWorkspace &workspace, size_t position, size_t limit,
                                PreviewCandidateRow &result);

namespace detail
{
struct BaseScoreRow
{
    const int *indices = nullptr;
    const double *values = nullptr;
    size_t count = 0;
};

BaseScoreRow cachedBaseScoreRow(const PreviewScores &scores, size_t position);
std::vector<PreviewCandidate> rankPreviewCandidates(BaseScoreRow scores, const std::vector<ImageRecord> &records,
    const MosaicSelection &selection, size_t position, int window, double penalty, size_t limit);
} // namespace detail
} // namespace mosaicraft
