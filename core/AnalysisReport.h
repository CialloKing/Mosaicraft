#pragma once
#include "AnalysisTiles.h"
#include "Database.h"
#include "MosaicEngine.h"
#include <opencv2/core.hpp>
#include <unordered_map>

namespace mosaicraft::detail
{
// 报告只借用任务数据，不持有数据库或复制整图及特征载荷。
struct AnalysisReportContext
{
    const MosaicEngine::Config &cfg;
    const std::string &outputPath;
    const cv::Mat &target;
    int totalTiles = 0;
    int tilesX = 0;
    int tilesY = 0;
    int outTileW = 0;
    int outTileH = 0;
    int featW = 0;
    int featH = 0;
    int dbCount = 0;
    int matchedTiles = 0;
    const std::vector<double> &analyzeScores;
    const std::vector<int> &analyzeImageIds;
    const std::vector<double> &analyzeLabD;
    const std::vector<double> &analyzeGridD;
    const std::vector<double> &analyzeEdgeD;
    const std::vector<double> &analyzeGaps;
    const std::vector<int> &analyzeRanks;
    const std::vector<int> &analyzeAnnRanks;
    const std::vector<int> &analyzeCat;
    const double *analyzeGridCellSum = nullptr;
    const WorstTiles &worstTiles;
    const std::vector<ImageRecord> &allRecords;
    const std::vector<int> &selected;
};

// 返回需要提交的使用计数；禁用分析或目录创建失败时返回空计数。
std::unordered_map<int, int> writeAnalysisReport(const AnalysisReportContext &context,
                                                 RuntimeContext *runtime = nullptr);
} // namespace mosaicraft::detail
