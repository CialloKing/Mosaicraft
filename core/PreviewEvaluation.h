#pragma once

#include "MatchInspection.h"
#include "MosaicService.h"

namespace mosaicraft
{
struct PreviewProbe
{
    size_t position;
    int replacement = 0;
};

struct PreviewCandidateScore
{
    int imageId;
    double score;
    FeatureDistances features;
    int useCount = 0;
};

struct PreviewTileScore
{
    size_t position;
    int imageId;
    std::optional<double> score;
    std::vector<unsigned char> targetJpeg;
    std::vector<PreviewCandidateScore> candidates;
};

ServiceResult evaluatePreview(const MosaicRequest &request, const MosaicSelection &selection,
                              const std::vector<PreviewProbe> &probes, bool explain,
                              std::vector<PreviewTileScore> &scores);

struct PreviewComparison
{
    uint64_t compared = 0, changed = 0, matchedA = 0, matchedB = 0, uniqueA = 0, uniqueB = 0;
    double meanA = 0, meanB = 0, p90A = 0, p90B = 0;
    int mapColumns = 0, mapRows = 0;
    std::vector<unsigned char> imageA, imageB, changedMap;
};

ServiceResult comparePreview(const MosaicRequest &request, const MosaicSelection &a, const MosaicSelection &b,
                             const std::atomic_bool &canceled, PreviewComparison &result);
} // namespace mosaicraft
