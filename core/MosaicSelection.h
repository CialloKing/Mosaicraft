#pragma once

#include "AnalysisTiles.h"
#include <opencv2/core/mat.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mosaicraft
{
class FeatureIndex;
struct ImageRecord;
class PreviewRecommendationCache;

struct PreviewScores
{
    static std::shared_ptr<PreviewScores> create(size_t tiles, int candidates,
                                                 const std::vector<ImageRecord> &records);
    PreviewScores();
    ~PreviewScores();
    PreviewScores(const PreviewScores &) = delete;
    PreviewScores &operator=(const PreviewScores &) = delete;

    int candidates = 0;
    std::vector<int> indices;
    std::vector<double> values;
    std::vector<ImageRecord> records;

  private:
    size_t reservedBytes = 0;
};

struct PreviewTileFeatures
{
    double l, a, b, edge;
    std::array<float, 192> grid;
    std::array<uint8_t, 256> tiny;
    std::array<float, 256> lbp;
};

struct SelectionAnalysis
{
    std::vector<double> scores, lab, grid, edge, gaps;
    std::vector<int> imageIds, ranks, annRanks, categories;
    double gridCells[64]{};
    WorstTiles worstTiles;
};

struct PreviewAsset
{
    std::string path, hash;
    uintmax_t bytes = 0;
    std::filesystem::file_time_type modified{};
    bool available = false;

    bool current() const;
    std::string version() const;
};

struct PreviewCatalogEntry
{
    int id, index;
    std::string name, path, hash;
};

struct PreviewCatalog
{
    std::vector<PreviewCatalogEntry> entries;
    const PreviewCatalogEntry *find(int id) const;
};

struct MosaicSelection
{
    static constexpr int maxTiles = 160000;
    static constexpr int recommendationLimit = 60;
    std::string matchingKey, dbPath, featureBackend, scoreBackend;
    uint64_t targetVersion = 0, libraryVersion = 0;
    std::string editKey;
    size_t manualEdits = 0;
    int columns = 0, rows = 0, featureWidth = 0, featureHeight = 0;
    std::vector<int> imageIds, indices;
    std::unordered_map<int, PreviewAsset> assets;
    std::shared_ptr<const PreviewCatalog> catalog;
    std::vector<std::array<int, recommendationLimit>> recommendations;
    std::vector<unsigned char> referenceJpeg;
    SelectionAnalysis analysis;

    void deferRecommendations(std::shared_ptr<const PreviewScores> scores, int window, double penalty);
    std::array<int, recommendationLimit> recommendationsAt(size_t position) const;

  private:
    std::shared_ptr<PreviewRecommendationCache> m_recommendations;
};

struct PreviewWorkspace
{
    std::string targetKey, libraryPath, candidateKey;
    cv::Mat target;
    std::shared_ptr<FeatureIndex> index;
    std::shared_ptr<const PreviewCatalog> catalog;
    uint64_t libraryVersion = 0;
    std::vector<int> candidates;
    std::string tileFeatureKey, scoreKey;
    std::vector<PreviewTileFeatures> tileFeatures;
    std::shared_ptr<const PreviewScores> scores;
};

uint64_t previewFileVersion(const std::vector<unsigned char> &bytes);
uint64_t previewLibraryVersion(const std::vector<ImageRecord> &records, int width, int height);
PreviewAsset previewAsset(const ImageRecord &record);
PreviewAsset previewAsset(const PreviewCatalogEntry &entry);
std::shared_ptr<const PreviewCatalog> makePreviewCatalog(const std::vector<ImageRecord> &records);
bool previewAssetsCurrent(const MosaicSelection &selection);
} // namespace mosaicraft
