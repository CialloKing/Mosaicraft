#include "EditorCandidates.h"

#include "compute/PortableCompute.h"
#include "core/ExecutionScope.h"
#include "core/FeatureIndex.h"
#include "core/MatchInspection.h"
#include "core/MatchingScore.h"
#include "core/MosaicPlanning.h"
#include "core/MosaicSelection.h"
#include "core/NeighborhoodState.h"
#include "core/TileFeatures.h"
#include "core/UnicodeIO.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>

using namespace mosaicraft;

namespace
{
using Scores = std::vector<std::pair<double, int>>;

void rankCandidates(Scores &scores, const std::vector<ImageRecord> &records,
                    const MosaicEngine::Config &config, const MosaicSelection &selection,
                    size_t position, std::vector<EditorCandidate> &result)
{
    int window = config.neighborWindow;
    if (window <= 0)
    {
        const int dynamic = static_cast<int>(std::sqrt(static_cast<double>(records.size())) * 1.5);
        window = std::max({300, selection.columns * 2, std::min(dynamic, 400)});
    }
    detail::NeighborhoodState neighborhood(records, selection.columns, window, config.neighborPenalty);
    const int tile = static_cast<int>(position);
    neighborhood.replay(selection.indices, tile);
    for (auto &[score, index] : scores)
    {
        neighborhood.applyPenalty(score, index, tile);
    }
    std::sort(scores.begin(), scores.end());
    result.reserve(scores.size());
    for (const auto &[score, index] : scores)
    {
        result.push_back({records[index].id, score});
    }
}

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
    MatchWeights weights{config.labWeight, config.gridWeight, config.tinyWeight, config.edgeWeight, config.lbpWeight};
    const auto total = std::accumulate(weights.begin(), weights.end(), 0.0);
    for (auto &weight : weights)
    {
        weight /= total;
    }
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

float fastScore(const MatchingBuffers &buffers, const ImageRecord &record, const FeaturePack::Reader::Row &features)
{
    const auto d = compute::fp32Distances(buffers, 0, record, features);
    return float(buffers.labW[0]) * d[0] + float(buffers.gridW[0]) * d[1] + float(buffers.tinyW[0]) * d[2] +
           float(buffers.edgeW[0]) * d[3] + float(buffers.lbpW[0]) * d[4];
}
}

ServiceResult editorCandidates(const MosaicRequest &request, const MosaicSelection &selection,
                               const PreviewWorkspace &workspace, size_t position, std::vector<EditorCandidate> &result)
{
    const auto &config = request.config;
    Scores scores;
    scores.reserve(config.candidates);
    const auto count = static_cast<size_t>(config.candidates);
    const auto offset = position * count;
    if (workspace.scores && workspace.scores->candidates == config.candidates &&
        workspace.scores->indices.size() >= offset + count && workspace.scores->values.size() >= offset + count)
    {
        const auto &cached = *workspace.scores;
        for (size_t candidate = offset; candidate < offset + count; ++candidate)
        {
            if (cached.indices[candidate] >= 0)
            {
                scores.emplace_back(cached.values[candidate], cached.indices[candidate]);
            }
        }
        rankCandidates(scores, cached.records, config, selection, position, result);
        return ServiceResult::success();
    }

    ExecutionScope scope;
    Database database(selection.dbPath, Database::OpenMode::ReadOnly);
    if (!database.isOpen())
    {
        return ServiceResult::failure(1, database.error(), database.errorCode());
    }
    int featureWidth = 0, featureHeight = 0;
    const auto records = database.allRecords(config.usePenalty > 0);
    if (!readLibraryDimensions(database, featureWidth, featureHeight) ||
        featureWidth != selection.featureWidth || featureHeight != selection.featureHeight ||
        records.empty() || previewLibraryVersion(records, featureWidth, featureHeight, config.usePenalty > 0) != selection.libraryVersion)
    {
        return ServiceResult::failure(1, "the preview library changed", "preview_stale");
    }
    MatchingBuffers buffers;
    buffers.resize(1, 0, config.candidates, false, false);
    if (!prepareFeatures(request, selection, workspace, position, buffers))
    {
        return ServiceResult::failure(1, "the preview target changed", "preview_stale");
    }

    const auto featureDirectory = u8path(records.front().tinyPath).parent_path();
    auto &indices = buffers.indices;
    if (workspace.candidates && workspace.candidates->data.size() >= offset + count)
    {
        std::copy_n(workspace.candidates->data.begin() + offset, count, indices.begin());
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
        index->queryBatch(buffers.queries, config.candidates, indices, workers);
    }
    for (auto &index : indices)
    {
        if (index >= 0 && std::abs(records[index].avgL - buffers.l[0]) > config.lRange)
        {
            index = -1;
        }
    }
    setWeights(buffers, config);
    FeaturePack::Reader reader(pathToUtf8(featureDirectory), records, 32ULL * 1024 * 1024);
    reader.prepare(indices);
    const RowHistory history(indices.data(), config.candidates, records, config.usePenalty);
    for (int index : indices)
    {
        if (index >= 0)
        {
            const auto &asset = records[index];
            const auto &features = reader.get(index);
            const double score = config.scoreMode == ScoreMode::Fast
                ? history.combine(fastScore(buffers, asset, features), asset.useCount, config.usePenalty)
                : cpuBaseScore(buffers, 0, asset, features, config.usePenalty);
            scores.emplace_back(score, index);
        }
    }
    rankCandidates(scores, records, config, selection, position, result);
    return ServiceResult::success();
}
