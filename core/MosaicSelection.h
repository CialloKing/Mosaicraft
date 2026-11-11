#pragma once

#include "AnalysisTiles.h"
#include <opencv2/core/mat.hpp>

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
};

struct MosaicSelection
{
    static constexpr int maxTiles = 160000;
    std::string matchingKey, dbPath, featureBackend, scoreBackend;
    uint64_t targetVersion = 0, libraryVersion = 0;
    int columns = 0, rows = 0, featureWidth = 0, featureHeight = 0;
    std::vector<int> imageIds, indices;
    std::unordered_map<int, PreviewAsset> assets;
    std::vector<unsigned char> referenceJpeg;
    SelectionAnalysis analysis;
};

struct PreviewWorkspace
{
    std::string targetKey, libraryPath, candidateKey;
    cv::Mat target;
    std::shared_ptr<FeatureIndex> index;
    uint64_t libraryVersion = 0;
    std::vector<int> candidates;
};

uint64_t previewFileVersion(const std::vector<unsigned char> &bytes);
uint64_t previewLibraryVersion(const std::vector<ImageRecord> &records, int width, int height);
PreviewAsset previewAsset(const ImageRecord &record);
bool previewAssetsCurrent(const MosaicSelection &selection);
} // namespace mosaicraft
