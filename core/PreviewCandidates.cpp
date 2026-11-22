#include "PreviewCandidates.h"

#include "compute/PortableCompute.h"
#include "ExecutionScope.h"
#include "FeatureIndex.h"
#include "LibraryAccess.h"
#include "MatchInspection.h"
#include "MatchingScore.h"
#include "MosaicPlanning.h"
#include "MosaicSelection.h"
#include "MosaicService.h"
#include "NeighborhoodState.h"
#include "TileFeatures.h"
#include "UnicodeIO.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <tuple>

namespace mosaicraft
{
namespace detail
{
BaseScoreRow cachedBaseScoreRow(const PreviewScores &scores, size_t position)
{
    if (scores.candidates <= 0)
    {
        return {};
    }
    const auto count = static_cast<size_t>(scores.candidates);
    if (position >= std::min(scores.indices.size(), scores.values.size()) / count)
    {
        return {};
    }
    const auto offset = position * count;
    return {scores.indices.data() + offset, scores.values.data() + offset, count};
}

std::vector<PreviewCandidate> rankPreviewCandidates(BaseScoreRow scores, const std::vector<ImageRecord> &records,
    const MosaicSelection &selection, size_t position, int window, double penalty, size_t limit)
{
    if (!scores.count || !limit)
    {
        return {};
    }
    // 排序分只使用此快照的前序选择；基础分不因编辑而失效。
    NeighborhoodState neighborhood(records, selection.columns, window, penalty);
    const int tile = static_cast<int>(position);
    neighborhood.replay(selection.indices, tile);
    struct Ranked
    {
        double rankScore, baseScore;
        int index;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(scores.count);
    for (size_t i = 0; i < scores.count; ++i)
    {
        const int index = scores.indices[i];
        if (index >= 0)
        {
            double rankScore = scores.values[i];
            neighborhood.applyPenalty(rankScore, index, tile);
            ranked.push_back({rankScore, scores.values[i], index});
        }
    }
    const auto less = [](const Ranked &a, const Ranked &b)
    {
        return std::tie(a.rankScore, a.index) < std::tie(b.rankScore, b.index);
    };
    const size_t count = std::min(limit, ranked.size());
    if (count < ranked.size())
    {
        std::partial_sort(ranked.begin(), ranked.begin() + count, ranked.end(), less);
    }
    else
    {
        std::sort(ranked.begin(), ranked.end(), less);
    }
    std::vector<PreviewCandidate> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const auto &candidate = ranked[i];
        result.push_back({records[candidate.index].id, candidate.baseScore, candidate.rankScore});
    }
    return result;
}
} // namespace detail

namespace
{
void loadFeatures(const cv::Mat &tile, cv::Size size, bool fast, MatchingBuffers &buffers)
{
    cv::Mat resized, lab, gray;
    cv::resize(tile, resized, size, 0, 0, cv::INTER_LINEAR);
    cv::cvtColor(resized, lab, cv::COLOR_BGR2Lab);
    cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);
    const bool trial = MOSAICRAFT_TRIAL_CPU_TILES && fast;
    const auto mean = trial ? detail::gridAndMean(lab, buffers.grid[0].data()) : cv::mean(lab);
    buffers.l[0] = mean[0];
    buffers.a[0] = mean[1];
    buffers.b[0] = mean[2];
    buffers.edge[0] = computeEdgeDensityFromGray(gray);
    if (trial)
    {
        cv::Mat tiny(16, 16, CV_8UC1, buffers.tiny[0].data());
        cv::resize(gray, tiny, tiny.size(), 0, 0, cv::INTER_AREA);
        detail::lbpInto(gray, buffers.lbp[0].data(), MOSAICRAFT_TRIAL_LBP_SIMD);
    }
    else
    {
        buffers.grid[0] = computeGrid8x8FromLab(lab);
        buffers.tiny[0] = computeTinyImageFromGray(gray);
        buffers.lbp[0] = computeLBPHistogramFromGray(gray);
    }
}

bool prepareFeatures(const MosaicRequest &request, const MosaicSelection &selection,
                     const PreviewWorkspace &workspace, size_t position, MatchingBuffers &buffers)
{
    if (workspace.tileFeatures && position < workspace.tileFeatures->data.size())
    {
        const auto &features = workspace.tileFeatures->data[position];
        buffers.l[0] = features.l;
        buffers.a[0] = features.a;
        buffers.b[0] = features.b;
        buffers.edge[0] = features.edge;
        std::copy(features.grid.begin(), features.grid.end(), buffers.grid[0].data());
        std::copy(features.tiny.begin(), features.tiny.end(), buffers.tiny[0].data());
        std::copy(features.lbp.begin(), features.lbp.end(), buffers.lbp[0].data());
        return true;
    }

    cv::Mat target = workspace.target;
    const auto &config = request.config;
    if (target.empty())
    {
        const auto bytes = readBytesUnicode(request.inputPath);
        if (bytes.empty() || previewFileVersion(bytes) != selection.targetVersion)
        {
            return false;
        }
        target = cv::imdecode(bytes, cv::IMREAD_COLOR);
        TargetGeometry geometry;
        std::string error;
        if (!resolveTargetGeometry(target.cols, target.rows, config, geometry, error) ||
            geometry.columns != selection.columns || geometry.rows != selection.rows)
        {
            return false;
        }
        resizeTarget(target, geometry);
    }
    padTarget(target, config, selection.columns, selection.rows);
    const cv::Rect region(static_cast<int>(position % selection.columns) * config.tileW,
                          static_cast<int>(position / selection.columns) * config.tileH, config.tileW, config.tileH);
    loadFeatures(target(region), {selection.featureWidth, selection.featureHeight},
                 config.scoreMode == ScoreMode::Fast, buffers);
    return true;
}

void setWeights(MatchingBuffers &buffers, const MosaicEngine::Config &config)
{
    auto weights = normalizedMatchWeights(config);
    if (config.adaptiveWeights)
    {
        weights = adaptiveTileWeights(classifyTile(buffers.edge[0], buffers.grid[0], buffers.lbp[0]), weights);
    }
    buffers.labW[0] = weights[0];
    buffers.gridW[0] = weights[1];
    buffers.tinyW[0] = weights[2];
    buffers.edgeW[0] = weights[3];
    buffers.lbpW[0] = weights[4];
}
} // namespace

ServiceResult previewCandidates(const MosaicRequest &request, const MosaicSelection &selection,
                                const PreviewWorkspace &workspace, size_t position, size_t limit,
                                PreviewCandidateRow &result)
try
{
    const auto &config = request.config;
    result = {};
    if (selection.columns <= 0 || position >= selection.indices.size() ||
        position > std::numeric_limits<int>::max() || config.candidates <= 0)
    {
        return ServiceResult::failure(1, "invalid preview tile or candidate count", "invalid_candidate_query");
    }
    if (workspace.catalog && workspace.catalog != selection.catalog)
    {
        return ServiceResult::failure(1, "the candidate workspace belongs to another preview", "preview_stale");
    }
    if (!limit)
    {
        return ServiceResult::success();
    }
    const auto rank = [&](detail::BaseScoreRow row, const std::vector<ImageRecord> &records)
    {
        const int window = detail::resolveNeighborWindow(config.neighborWindow, selection.columns, records.size());
        result.candidates = detail::rankPreviewCandidates(row, records, selection, position, window,
                                                           config.neighborPenalty, limit);
    };
    if (workspace.scores && workspace.scores->candidates == config.candidates)
    {
        const auto row = detail::cachedBaseScoreRow(*workspace.scores, position);
        if (row.count)
        {
            rank(row, workspace.scores->records);
            return ServiceResult::success();
        }
    }

    std::lock_guard<std::mutex> lock(pipelineMutex());
    ExecutionScope scope;
    const LibraryAccess library(selection.dbPath, false);
    if (!library.acquired())
    {
        return ServiceResult::failure(1, "the preview library is busy", "library_busy");
    }
    Database database(selection.dbPath, Database::OpenMode::ReadOnly);
    if (!database.isOpen())
    {
        return ServiceResult::failure(1, database.error(), database.errorCode());
    }
    int featureWidth = 0, featureHeight = 0;
    const auto records = database.allRecords(config.usePenalty > 0);
    if (!readLibraryDimensions(database, featureWidth, featureHeight) ||
        featureWidth != selection.featureWidth || featureHeight != selection.featureHeight ||
        records.empty() || previewLibraryVersion(database.assetKey(), records, featureWidth, featureHeight, config.usePenalty > 0) != selection.libraryVersion)
    {
        return ServiceResult::failure(1, "the preview library changed", "preview_stale");
    }
    MatchingBuffers buffers;
    buffers.resize(1, 0, config.candidates, false, false);
    if (!prepareFeatures(request, selection, workspace, position, buffers))
    {
        return ServiceResult::failure(1, "the preview target changed", "preview_stale");
    }

    const auto featureDirectory = u8path(database.path()).parent_path() / "index";
    auto &indices = buffers.indices;
    const auto count = indices.size();
    if (workspace.candidates && position < workspace.candidates->data.size() / count)
    {
        std::copy_n(workspace.candidates->data.begin() + position * count, count, indices.begin());
    }
    else
    {
        auto index = workspace.index;
        if (!index)
        {
            index = std::make_shared<FeatureIndex>();
            if (!index->load(pathToUtf8(featureDirectory / "lib.ann196"), FeatureIndex::kDimension, records) &&
                !index->build(records))
            {
                return ServiceResult::failure(1, "cannot open the library search index", "library_unavailable");
            }
        }
        writeTileVector(buffers.l[0], buffers.a[0], buffers.b[0], buffers.grid[0], buffers.edge[0], buffers.queries.data());
        WorkerPool workers(1);
        index->queryBatch(buffers.queries, config.candidates, indices, workers, MOSAICRAFT_TRIAL_ANN_ROWS);
    }
    for (auto &index : indices)
    {
        filterCandidateLightness(index, records, buffers.l[0], config.lRange);
    }
    setWeights(buffers, config);
    LibraryFeatureStore::Reader reader(database, records, 32ULL * 1024 * 1024);
    reader.prepare(indices);
    std::optional<RowHistory> history;
    if (config.scoreMode == ScoreMode::Fast)
    {
        history.emplace(indices.data(), config.candidates, records, config.usePenalty);
    }
    std::vector<double> values(count);
    for (size_t i = 0; i < count; ++i)
    {
        const int index = indices[i];
        if (index >= 0)
        {
            const auto &asset = records[index];
            const auto &features = reader.get(index);
            values[i] = history
                ? history->combine(weightedFastScore(buffers, 0, compute::fp32Distances(buffers, 0, asset, features)),
                                   asset.useCount, config.usePenalty)
                : cpuBaseScore(buffers, 0, asset, features, config.usePenalty);
        }
    }
    // 补算使用 CPU，不能将它标成原 GPU 预览的分数。
    result.source = PreviewScoreSource::CpuRecomputed;
    rank({indices.data(), values.data(), values.size()}, records);
    return ServiceResult::success();
}
catch (const InvalidLibraryFeatures &error)
{
    return ServiceResult::failure(2, error.what(), "library_incompatible", "dbPath");
}
} // namespace mosaicraft
