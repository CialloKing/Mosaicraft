#pragma once

#include "MosaicPlanning.h"
#include "MosaicSelection.h"

namespace mosaicraft
{
struct MosaicCoverage
{
    ColorDistribution target, library;
    int columns = 0, rows = 0, mapColumns = 0, mapRows = 0, threshold = 0;
    uint64_t noCandidates = 0, fewCandidates = 0;
    std::vector<int> minimumCandidates;
};

ServiceResult measureMosaicCoverage(const MosaicRequest &request, MosaicCoverage &coverage);

struct LibraryBrowser
{
    std::string id;
    std::vector<PreviewCatalogEntry> ordered;
    std::unordered_map<int, size_t> positions;
    std::unordered_map<int, int> tiles;
    std::unordered_map<int, double> distances;
    uint64_t version = 0;
};

ServiceResult browseLibrary(const std::string &dbPath, const std::string &sort, int anchor, LibraryBrowser &browser);
} // namespace mosaicraft
