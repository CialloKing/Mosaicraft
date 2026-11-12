#include "PreviewEvaluation.h"
#include "LibraryAccess.h"
#include "MosaicPlanning.h"
#include "MosaicSelection.h"
#include "UnicodeIO.h"
#include <opencv2/imgcodecs.hpp>
#include <numeric>
#include <unordered_set>

namespace mosaicraft
{
namespace
{
struct SourceStamp
{
    uintmax_t bytes = 0;
    std::filesystem::file_time_type modified{};
    bool operator==(const SourceStamp &other) const { return bytes == other.bytes && modified == other.modified; }
};

SourceStamp stamp(const std::string &file)
{
    std::error_code error;
    const auto path = u8path(file);
    const auto bytes = std::filesystem::file_size(path, error);
    if (error)
    {
        return {};
    }
    const auto modified = std::filesystem::last_write_time(path, error);
    return error ? SourceStamp{} : SourceStamp{bytes, modified};
}

struct TargetFeatures
{
    cv::Scalar lab;
    std::vector<float> grid;
    FeaturePack::Reader::Row texture;
    double edge = 0;
};

struct EvaluationCache
{
    std::string key;
    SourceStamp targetStamp, dbStamp, walStamp;
    cv::Mat target;
    std::vector<ImageRecord> records;
    std::unordered_map<int, int> index;
    std::unique_ptr<FeaturePack::Reader> reader;

    TargetFeatures features(size_t position, int columns, cv::Size size, const MosaicEngine::Config &config) const
    {
        const cv::Rect region(static_cast<int>(position % columns) * config.tileW,
                             static_cast<int>(position / columns) * config.tileH, config.tileW, config.tileH);
        cv::Mat tile, lab, gray;
        cv::resize(target(region), tile, size, 0, 0, cv::INTER_LINEAR);
        cv::cvtColor(tile, lab, cv::COLOR_BGR2Lab);
        cv::cvtColor(tile, gray, cv::COLOR_BGR2GRAY);
        TargetFeatures result;
        result.lab = cv::mean(lab);
        result.grid = computeGrid8x8FromLab(lab);
        result.edge = computeEdgeDensityFromGray(gray);
        const auto tiny = computeTinyImageFromGray(gray);
        const auto lbp = computeLBPHistogramFromGray(gray);
        std::copy_n(tiny.begin(), result.texture.tiny.size(), result.texture.tiny.begin());
        std::copy_n(lbp.begin(), result.texture.lbp.size(), result.texture.lbp.begin());
        result.texture.tinyValid = result.texture.lbpValid = true;
        return result;
    }

    std::optional<PreviewCandidateScore> score(const TargetFeatures &tile, int id, const MosaicEngine::Config &config)
    {
        const auto found = index.find(id);
        if (found == index.end())
        {
            return {};
        }
        const auto &record = records[found->second];
        const auto distances = featureDistances(tile.lab[0], tile.lab[1], tile.lab[2], tile.grid, tile.edge,
                                                tile.texture, record, reader->get(found->second));
        double value = inspectionScore(distances, config);
        if (config.adaptiveWeights)
        {
            const double total = config.labWeight + config.gridWeight + config.tinyWeight + config.edgeWeight + config.lbpWeight;
            const MatchWeights defaults{config.labWeight / total, config.gridWeight / total, config.tinyWeight / total,
                                        config.edgeWeight / total, config.lbpWeight / total};
            const auto weights = adaptiveTileWeights(classifyTile(tile.edge, tile.grid, tile.texture.lbp), defaults);
            value = std::inner_product(distances.begin(), distances.end(), weights.begin(), 0.0);
        }
        return PreviewCandidateScore{id, value, distances, record.useCount};
    }
};

std::mutex evaluationMutex;
std::unique_ptr<EvaluationCache> evaluation;

ServiceResult prepareEvaluation(const MosaicRequest &request, const MosaicSelection &selection)
{
    const auto input = stamp(request.inputPath);
    const auto dbStamp = stamp(selection.dbPath);
    const auto wal = stamp(selection.dbPath + "-wal");
    if (evaluation && evaluation->key == selection.editKey && evaluation->targetStamp == input &&
        evaluation->dbStamp == dbStamp && evaluation->walStamp == wal)
    {
        return ServiceResult::success();
    }
    evaluation.reset();
    auto cache = std::make_unique<EvaluationCache>();
    const auto bytes = readBytesUnicode(request.inputPath);
    if (bytes.empty() || previewFileVersion(bytes) != selection.targetVersion)
    {
        return ServiceResult::failure(1, "target changed", "preview_stale");
    }
    cache->target = cv::imdecode(bytes, cv::IMREAD_COLOR);
    TargetGeometry geometry;
    std::string error;
    if (!resolveTargetGeometry(cache->target.cols, cache->target.rows, request.config, geometry, error) ||
        geometry.columns != selection.columns || geometry.rows != selection.rows)
    {
        return ServiceResult::failure(1, "target grid changed", "preview_stale");
    }
    resizeTarget(cache->target, geometry, request.config);
    padTarget(cache->target, request.config, selection.columns, selection.rows);
    Database database(selection.dbPath, Database::OpenMode::ReadOnly);
    if (!database.isOpen())
    {
        return ServiceResult::failure(1, database.error(), database.errorCode());
    }
    cache->records = database.allRecords();
    if (cache->records.empty() ||
        previewLibraryVersion(cache->records, selection.featureWidth, selection.featureHeight) != selection.libraryVersion)
    {
        return ServiceResult::failure(1, "library changed", "preview_stale");
    }
    for (size_t i = 0; i < cache->records.size(); ++i)
    {
        cache->index.emplace(cache->records[i].id, static_cast<int>(i));
    }
    const auto directory = pathToUtf8(u8path(cache->records.front().tinyPath).parent_path());
    cache->reader = std::make_unique<FeaturePack::Reader>(directory, cache->records, 32ULL * 1024 * 1024);
    cache->key = selection.editKey;
    cache->targetStamp = input;
    cache->dbStamp = dbStamp;
    cache->walStamp = wal;
    evaluation = std::move(cache);
    return ServiceResult::success();
}
} // namespace

ServiceResult evaluatePreview(const MosaicRequest &request, const MosaicSelection &selection,
                              const std::vector<PreviewProbe> &probes, bool explain,
                              std::vector<PreviewTileScore> &scores)
{
    const std::unique_lock<std::mutex> lock(evaluationMutex, std::try_to_lock);
    const LibraryAccess library(selection.dbPath, false);
    if (!lock.owns_lock() || !library.acquired())
    {
        return ServiceResult::failure(1, "preview evaluation is busy", "library_busy");
    }
    if (auto status = prepareEvaluation(request, selection); !status.ok)
    {
        return status;
    }
    const auto &config = request.config;
    const cv::Size size(selection.featureWidth, selection.featureHeight);
    for (const auto &probe : probes)
    {
        const int id = probe.replacement ? probe.replacement : selection.imageIds[probe.position];
        PreviewTileScore result{probe.position, id, {}, {}, {}};
        if (id > 0 || explain)
        {
            const auto features = evaluation->features(probe.position, selection.columns, size, config);
            if (auto selected = evaluation->score(features, id, config))
            {
                result.score = selected->score;
                if (explain)
                {
                    result.candidates.push_back(*selected);
                }
            }
            if (explain)
            {
                for (int candidate : selection.recommendationsAt(probe.position))
                {
                    if (candidate != id)
                    {
                        if (auto item = evaluation->score(features, candidate, config))
                        {
                            result.candidates.push_back(*item);
                        }
                    }
                    if (result.candidates.size() >= 12)
                    {
                        break;
                    }
                }
                const cv::Rect rect(static_cast<int>(probe.position % selection.columns) * config.tileW,
                                    static_cast<int>(probe.position / selection.columns) * config.tileH,
                                    config.tileW, config.tileH);
                cv::Mat thumbnail;
                const double scale = std::min(1.0, 128.0 / std::max(rect.width, rect.height));
                cv::resize(evaluation->target(rect), thumbnail, cv::Size(std::max(1, cvRound(rect.width * scale)),
                           std::max(1, cvRound(rect.height * scale))), 0, 0, cv::INTER_AREA);
                cv::imencode(".jpg", thumbnail, result.targetJpeg);
            }
        }
        scores.push_back(std::move(result));
    }
    return ServiceResult::success();
}
} // namespace mosaicraft
