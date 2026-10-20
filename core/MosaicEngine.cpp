#include "MosaicEngine.h"
#include "AnalysisTiles.h"
#include "BenchmarkTimer.h"
#include "BrightnessAdjustment.h"
#include "BigTiffWriter.h"
#include "CandidateFeatures.h"
#include "Database.h"
#include "DeepZoomWriter.h"
#include "FeatureExtractor.h"
#include "FeatureIndex.h"
#include "FeatureMatrix.h"
#include "FeaturePack.h"
#include "FeatureUtils.h"
#include "GridDuplicateCache.h"
#include "ImageCache.h"
#include "JpgStreamWriter.h"
#include "MosaicOutput.h"
#include "PngBatchWriter.h"
#include "PngStreamWriter.h"
#include "RequestValidation.h"
#include "UnicodeIO.h"
#include "compute/CudaBackend.h"
#include <numeric>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif
#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>

namespace mosaicraft
{

// ============================================================
// 在选图后的输出阶段微调 LAB L 通道，减少重复图片的视觉重复感。
// ============================================================
static void adjustColor(cv::Mat& img, double strength)
{
    // 输出线程各自持有生成器，避免共享随机数状态产生数据竞争。
    thread_local std::mt19937 rng(std::random_device{}());
    detail::adjustBrightness(img, strength, rng);
}

struct AnalysisReportContext
{
    const MosaicEngine::Config& cfg;
    Database& db;
    const std::string& outputPath;
    const std::string& targetPath;
    const std::string& targetHash;
    const cv::Mat& target;
    int totalTiles = 0;
    int tilesX = 0;
    int tilesY = 0;
    int outTileW = 0;
    int outTileH = 0;
    int featW = 0;
    int featH = 0;
    int dbCount = 0;
    int matchedTiles = 0;
    const std::vector<double>& analyzeScores;
    const std::vector<int>& analyzeImageIds;
    const std::vector<double>& analyzeLabD;
    const std::vector<double>& analyzeGridD;
    const std::vector<double>& analyzeEdgeD;
    const std::vector<double>& analyzeGaps;
    const std::vector<int>& analyzeRanks;
    const std::vector<int>& analyzeAnnRanks;
    const std::vector<int>& analyzeCat;
    const double* analyzeGridCellSum = nullptr;
    const WorstTiles &worstTiles;
    const std::vector<ImageRecord>& allRecords;
    const std::vector<int> &selected;
};

static void writeAnalysisReport(const AnalysisReportContext& ctx)
{
    const auto& cfg = ctx.cfg;
    auto& db = ctx.db;
    const auto& outputPath = ctx.outputPath;
    const auto& targetPath = ctx.targetPath;
    const auto& targetHash = ctx.targetHash;
    const auto& target = ctx.target;
    const int totalTiles = ctx.totalTiles;
    const int tilesX = ctx.tilesX;
    const int tilesY = ctx.tilesY;
    const int outTileW = ctx.outTileW;
    const int outTileH = ctx.outTileH;
    const int featW = ctx.featW;
    const int featH = ctx.featH;
    const int dbCount = ctx.dbCount;
    const int matched = ctx.matchedTiles > 0 ? ctx.matchedTiles : totalTiles;
    const auto& analyzeScores = ctx.analyzeScores;
    const auto& analyzeImageIds = ctx.analyzeImageIds;
    const auto& analyzeLabD = ctx.analyzeLabD;
    const auto& analyzeGridD = ctx.analyzeGridD;
    const auto& analyzeEdgeD = ctx.analyzeEdgeD;
    const auto& analyzeGaps = ctx.analyzeGaps;
    const auto& analyzeRanks = ctx.analyzeRanks;
    const auto& analyzeAnnRanks = ctx.analyzeAnnRanks;
    const auto& analyzeCat = ctx.analyzeCat;
    const double* analyzeGridCellSum = ctx.analyzeGridCellSum;
    const auto &worstTiles = ctx.worstTiles;
    const auto& allRecords = ctx.allRecords;
    const auto &selected = ctx.selected;

    if (!cfg.analyze || analyzeScores.empty() || analyzeGridCellSum == nullptr)
        return;

    int n = static_cast<int>(analyzeScores.size());
    std::vector<double> sortedScores = analyzeScores;
    std::sort(sortedScores.begin(), sortedScores.end());
    double scoreMean = 0, scoreMin = 1e30, scoreMax = 0;
    for (double s : analyzeScores) { scoreMean += s; if (s < scoreMin) scoreMin = s; if (s > scoreMax) scoreMax = s; }
    scoreMean /= n;
    double scoreP50 = sortedScores[n/2];
    double scoreP90 = sortedScores[n*9/10];
    double scoreP99 = sortedScores[n*99/100];

    double labSum = 0, gridSum = 0, edgeSum = 0;
    double labW = cfg.labWeight, gridW = cfg.gridWeight, edgeW = cfg.edgeWeight;
    double totalW = labW + gridW + cfg.tinyWeight + edgeW + cfg.lbpWeight;
    labW /= totalW; gridW /= totalW; edgeW /= totalW;
    for (int i = 0; i < n; ++i)
    {
        labSum  += labW  * analyzeLabD[i];
        gridSum += gridW * analyzeGridD[i];
        edgeSum += edgeW * analyzeEdgeD[i];
    }
    double contribTotal = labSum + gridSum + edgeSum;

    std::unordered_map<int, int> useCount;
    for (int id : analyzeImageIds) useCount[id]++;
    std::vector<std::pair<int,int>> topUsed;
    for (auto& [id, cnt] : useCount) topUsed.push_back({cnt, id});
    std::sort(topUsed.rbegin(), topUsed.rend());

    std::cout << "\n=== Match Quality Analysis ===\n";
    std::cout << "  Scores describe selected tiles before brightness adjustment and base-image overlay.\n";
    std::cout << "  Tiles: " << n << "\n";
    std::cout << "  Score: mean=" << std::fixed << std::setprecision(4) << scoreMean
              << " median=" << scoreP50 << " p90=" << scoreP90
              << " p99=" << scoreP99 << " max=" << scoreMax << "\n";
    std::cout << "  Feature contribution (LAB/Grid/Edge only):\n";
    if (contribTotal > 0)
        std::cout << "    LAB="  << std::setprecision(1) << (labSum*100/contribTotal)
                  << "%  Grid=" << (gridSum*100/contribTotal)
                  << "%  Edge=" << (edgeSum*100/contribTotal) << "%\n";
    if (!analyzeGaps.empty())
    {
        auto sortedGaps = analyzeGaps;
        std::sort(sortedGaps.begin(), sortedGaps.end());
        double gapMean = 0;
        for (double g : analyzeGaps) gapMean += g;
        gapMean /= analyzeGaps.size();
        std::cout << "  Winner-RunnerUp gap: mean=" << std::setprecision(4) << gapMean
                  << " median=" << sortedGaps[analyzeGaps.size()/2]
                  << " p90=" << sortedGaps[analyzeGaps.size()*9/10] << "\n";
    }
    if (!analyzeRanks.empty())
    {
        int rankBuckets[4] = {0, 0, 0, 0};
        for (int r : analyzeRanks)
        {
            if (r <= 3) rankBuckets[r-1]++;
            else rankBuckets[3]++;
        }
        double total = static_cast<double>(analyzeRanks.size());
        std::cout << "  Winner rank in TopN: #1=" << std::setprecision(1) << (rankBuckets[0]*100/total)
                  << "% #2=" << (rankBuckets[1]*100/total) << "% #3=" << (rankBuckets[2]*100/total) << "%\n";
    }
    if (!analyzeAnnRanks.empty())
    {
        int annTop1=0, annTop5=0, annTop10=0, annTop20=0, annTop50=0;
        for (int r : analyzeAnnRanks)
        {
            if (r == 0) annTop1++;
            if (r < 5) annTop5++;
            if (r < 10) annTop10++;
            if (r < 20) annTop20++;
            if (r < 50) annTop50++;
        }
        double t = static_cast<double>(analyzeAnnRanks.size());
        std::cout << "  ANN recall: Top1=" << std::setprecision(1) << (annTop1*100/t)
                  << "% Top5=" << (annTop5*100/t) << "% Top10=" << (annTop10*100/t)
                  << "% Top20=" << (annTop20*100/t) << "%\n";
    }
    if (!analyzeCat.empty())
    {
        double catScores[4] = {0, 0, 0, 0};
        int catCounts[4] = {0, 0, 0, 0};
        const char* catNames[] = {"Smooth", "Edge", "Texture", "Normal"};
        for (int i = 0; i < static_cast<int>(analyzeCat.size()); ++i)
        {
            int c = analyzeCat[i];
            if (c >= 0 && c < 4) { catScores[c] += analyzeScores[i]; catCounts[c]++; }
        }
        std::cout << "  Score by category:\n";
        for (int c = 0; c < 4; ++c)
            if (catCounts[c] > 0)
                std::cout << "    " << catNames[c] << "(" << catCounts[c] << "): "
                          << std::setprecision(4) << (catScores[c]/catCounts[c]) << "\n";
    }

    std::cout << "  Worst 5 tiles:\n";
    const auto worstIdx = worstTiles.ranks();
    for (int k = 0; k < std::min(5, n); ++k)
    {
        int sample = worstIdx[k].second;
        int ti = worstTiles.at(sample).tile;
        int tx = ti % tilesX, ty = ti / tilesX;
        std::cout << "    (" << tx << "," << ty << ") score="
                  << std::fixed << std::setprecision(4) << worstIdx[k].first;
        if (sample < static_cast<int>(analyzeCat.size()))
        {
            const char *cn = (analyzeCat[sample] == 0)   ? "Smooth"
                             : (analyzeCat[sample] == 1) ? "Edge"
                             : (analyzeCat[sample] == 2) ? "Texture"
                                                         : "Normal";
            std::cout << " [" << cn << "]";
        }
        std::cout << "\n";
    }
    {
        double cellSum = 0;
        for (int i = 0; i < 64; ++i) cellSum += analyzeGridCellSum[i];
        if (cellSum > 0)
        {
            std::cout << "  Grid 8x8 cell contribution (avg distance, lower=more important):\n";
            for (int r = 0; r < 8; ++r)
            {
                std::cout << "    ";
                for (int c = 0; c < 8; ++c)
                {
                    double val = analyzeGridCellSum[r * 8 + c] / n * 100.0;
                    std::cout << std::fixed << std::setprecision(1) << std::setw(5) << val;
                }
                std::cout << "\n";
            }
            double wTotal = 0;
            for (int i = 0; i < 64; ++i)
            {
                double d = analyzeGridCellSum[i] / n;
                if (d < 0.001) d = 0.001;
                wTotal += 1.0 / d;
            }
            std::cout << "  Grid weights (auto-tuned): {";
            for (int r = 0; r < 8; ++r)
            {
                if (r > 0) std::cout << "   ";
                for (int c = 0; c < 8; ++c)
                {
                    double w = (1.0 / (analyzeGridCellSum[r*8+c] / n)) / wTotal * 64.0;
                    std::cout << std::fixed << std::setprecision(2) << w;
                    if (r*8+c < 63) std::cout << ",";
                }
                std::cout << (r < 7 ? "\n" : "}\n");
            }
        }
    }
    std::cout << "  Reuse: unique=" << useCount.size() << "/" << n
              << " ratio=" << std::setprecision(2) << (static_cast<double>(n)/useCount.size()) << "x\n";
    std::cout << "  Top 10 most used:\n";
    int top10Total = 0;
    for (int i = 0; i < std::min(10, static_cast<int>(topUsed.size())); ++i)
    {
        std::cout << "    id=" << topUsed[i].second << " : " << topUsed[i].first << " times\n";
        top10Total += topUsed[i].first;
    }
    std::cout << "  Top10 share: " << std::fixed << std::setprecision(1)
              << (100.0 * top10Total / n) << "% of all tiles\n";

    int freqDist[6] = {0};
    for (const auto& [id, cnt] : useCount)
    {
        if (cnt == 1) freqDist[0]++;
        else if (cnt == 2) freqDist[1]++;
        else if (cnt == 3) freqDist[2]++;
        else if (cnt <= 5) freqDist[3]++;
        else if (cnt <= 10) freqDist[4]++;
        else freqDist[5]++;
    }
    std::cout << "  Freq dist: 1x=" << freqDist[0] << " 2x=" << freqDist[1]
              << " 3x=" << freqDist[2] << " 4-5x=" << freqDist[3]
              << " 6-10x=" << freqDist[4] << " 10x+=" << freqDist[5] << "\n";

    std::string anaDir = outputPath;
    auto dp = anaDir.rfind('.');
    if (dp != std::string::npos) anaDir = anaDir.substr(0, dp) + "_analysis";
    else anaDir += "_analysis";
    std::error_code ec;
    std::filesystem::create_directories(u8path(anaDir), ec);
    if (ec)
    {
        std::cerr << "WARNING: analysis dir creation failed: " << ec.message() << std::endl;
        return;
    }

    std::string freqDir = anaDir + "/freq_rank";
    std::filesystem::create_directories(u8path(freqDir), ec);
    if (ec)
    {
        std::cerr << "WARNING: freq_rank dir creation failed: " << ec.message() << std::endl;
        return;
    }
    int exported = 0;
    for (const auto& [cnt, id] : topUsed)
    {
        if (cnt < 2) break;
        for (int i = 0; i < dbCount; ++i)
        {
            if (allRecords[i].id == id && !allRecords[i].filePath.empty())
            {
                cv::Mat img = imreadUnicode(allRecords[i].filePath, cv::IMREAD_COLOR);
                if (!img.empty())
                {
                    std::string normFile = std::filesystem::path(allRecords[i].filePath).filename().string();
                    std::string fn = freqDir + "/rank" + (exported + 1 < 10 ? "0" : "")
                                   + std::to_string(exported + 1)
                                   + "_" + std::to_string(cnt)
                                   + "x_id" + std::to_string(id)
                                   + "_" + normFile;
                    imwriteUnicode(fn, img);
                    exported++;
                }
                break;
            }
        }
    }
    if (exported > 0)
        std::cout << "  Frequency ranking: " << freqDir << "/ (x" << exported << ")\n";

    constexpr int kExport = 20;
    for (int k = 0; k < std::min(kExport, n); ++k)
    {
        int sample = worstIdx[k].second;
        int ti = worstTiles.at(sample).tile;
        int tx = ti % tilesX, ty = ti / tilesX;
        char fname[256];
        cv::Mat tileROI = target(cv::Rect(tx*cfg.tileW, ty*cfg.tileH, cfg.tileW, cfg.tileH));
        cv::Mat tileBig;
        cv::resize(tileROI, tileBig, cv::Size(featW, featH), 0, 0, cv::INTER_NEAREST);
        const char *cn = (sample < static_cast<int>(analyzeCat.size()) && analyzeCat[sample] == 0)   ? "S"
                         : (sample < static_cast<int>(analyzeCat.size()) && analyzeCat[sample] == 1) ? "E"
                         : (sample < static_cast<int>(analyzeCat.size()) && analyzeCat[sample] == 2) ? "T"
                                                                                                     : "N";
        snprintf(fname, sizeof(fname), "%s/worst_%02d_s%.4f_%s_tile.png",
                 anaDir.c_str(), k, worstIdx[k].first, cn);
        imwriteUnicode(fname, tileBig);
        if (selected[ti] >= 0 && !allRecords[selected[ti]].filePath.empty())
        {
            cv::Mat match = imreadUnicode(allRecords[selected[ti]].filePath, cv::IMREAD_COLOR);
            if (!match.empty())
            {
                snprintf(fname, sizeof(fname), "%s/worst_%02d_match.png", anaDir.c_str(), k);
                imwriteUnicode(fname, match);
            }
        }
    }
    std::cout << "  Worst tiles exported: " << anaDir << "/ (x" << kExport << ")\n";

    {
        std::string rptPath = anaDir + "/worst_report.txt";
        std::ofstream rpt(u8path(rptPath));
        rpt << "=== Worst Tile Analysis ===\n";
        for (int k = 0; k < std::min(kExport, n); ++k) {
            int sample = worstIdx[k].second;
            int ti = worstTiles.at(sample).tile, tx = ti % tilesX, ty = ti / tilesX;
            const auto &rec = allRecords[selected[ti]];
            double labD = labDistance(worstTiles.at(sample).lab[0], worstTiles.at(sample).lab[1],
                                      worstTiles.at(sample).lab[2], rec.avgL, rec.avgA, rec.avgB);
            double gridD = gridDistance8x8(
                std::vector<float>(worstTiles.at(sample).grid.begin(), worstTiles.at(sample).grid.end()), rec.grid4x4,
                true);
            double edgeD = std::abs(worstTiles.at(sample).edge - rec.edgeDensity);
            rpt << "\n#" << (k+1) << " tile(" << tx << "," << ty << ") score=" << worstIdx[k].first << "\n";
            rpt << "  Tile  LAB=" << worstTiles.at(sample).lab[0] << "," << worstTiles.at(sample).lab[1] << ","
                << worstTiles.at(sample).lab[2] << " Edge=" << worstTiles.at(sample).edge << "\n";
            rpt << "  Match LAB=" << rec.avgL << "," << rec.avgA << "," << rec.avgB
                << " Edge=" << rec.edgeDensity << "\n";
            rpt << "  Dists: LAB=" << labD << " Grid=" << gridD << " Edge=" << edgeD << "\n";
            std::string cause;
            if (labD > 0.3) cause = "color mismatch (LAB dist " + std::to_string(labD).substr(0,4) + ")";
            else if (gridD > 0.5) cause = "spatial mismatch (Grid dist " + std::to_string(gridD).substr(0,4) + ")";
            else if (worstTiles.at(sample).lab[0] < 60)
                cause = "dark region (tile L=" + std::to_string((int)worstTiles.at(sample).lab[0]) + ")";
            else if (rec.useCount > 10) cause = "popular image penalty (used " + std::to_string(rec.useCount) + "x)";
            else cause = "combined mismatch";
            rpt << "  Cause: " << cause << "\n";
        }
        std::cout << "  Diagnosis report: " << rptPath << "\n";
    }

    std::string heatPath = anaDir + "/heatmap.png";
    double sMin = sortedScores.front(), sRange = sortedScores.back() - sMin;
    if (sRange < 0.001) sRange = 0.001;
    int cellW = cfg.tileW;
    int cellH = cfg.tileH;
    cv::Mat heat(tilesY * cellH, tilesX * cellW, CV_8UC3, cv::Scalar(64, 64, 64));
    int sample = 0;
    for (int ty = 0; ty < tilesY; ++ty)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            int ti = ty * tilesX + tx;
            if (selected[ti] < 0)
                continue;
            double s = analyzeScores[sample++];
            double t = (s - sMin) / sRange;
            cv::Vec3b color;
            if (t < 0.5)
                color = cv::Vec3b(0, static_cast<uchar>(255*t*2), static_cast<uchar>(255*(1-t*2)));
            else
                color = cv::Vec3b(0, static_cast<uchar>(255*(1-(t-0.5)*2)), static_cast<uchar>(255));
            cv::rectangle(heat, cv::Rect(tx*cellW, ty*cellH, cellW, cellH), color, cv::FILLED);
        }
    }
    // 缩放到与原图一致的比例
    double srcAspect = (double)target.cols / target.rows;
    double heatAspect = (double)heat.cols / heat.rows;
    if (std::abs(srcAspect - heatAspect) > 0.01) {
        int newW = static_cast<int>(heat.rows * srcAspect);
        int newH = heat.rows;
        if (newW > heat.cols) { newW = heat.cols; newH = static_cast<int>(heat.cols / srcAspect); }
        if (newW > 1 && newH > 1)
            cv::resize(heat, heat, cv::Size(newW, newH), 0, 0, cv::INTER_NEAREST);
    }
    imwriteUnicode(heatPath, heat);
    std::cout << "  Heatmap: " << heatPath << "\n";

    std::string htmlPath = anaDir + "/report.html";
    std::ofstream html(u8path(htmlPath));
    if (html.is_open())
    {
        html << "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">\n";
        html << "<title>Mosaicraft Analysis</title>\n";
        html << "<style>body{font-family:system-ui,sans-serif;max-width:960px;margin:0 auto;padding:20px;"
             << "background:#1a1a2e;color:#e0e0e0}h1{color:#e94560}h2{color:#f0a500;border-bottom:1px solid #333}"
             << "table{border-collapse:collapse;width:100%;margin:10px 0}"
             << "th,td{border:1px solid #444;padding:8px;text-align:right}th{background:#16213e;color:#f0a500}"
             << "tr:nth-child(even){background:#0f3460}.good{color:#4ecca3}.warn{color:#f0a500}.bad{color:#e94560}"
             << ".bar{display:inline-block;height:12px;background:#e94560;border-radius:2px}"
             << "</style></head><body>\n";
        html << "<h1>&#55356;&#57211; Mosaicraft Analysis Report</h1>\n";

        html << "<h2>Overview</h2><table>\n";
        html << "<tr><th>Tiles</th><td>" << totalTiles << "</td></tr>\n";
        html << "<tr><th>Output</th><td>" << (tilesX*outTileW) << " x " << (tilesY*outTileH) << "</td></tr>\n";
        html << "<tr><th>Matched</th><td>" << matched << " / " << totalTiles << "</td></tr>\n";
        html << "</table>\n";

        html << "<h2>Match Quality</h2><p>Scores describe selected tiles before brightness adjustment and "
             << "base-image overlay, not the final composited image.</p><table>\n";
        html << "<tr><th>Score Mean</th><td>" << scoreMean << "</td></tr>\n";
        html << "<tr><th>Score Median</th><td>" << scoreP50 << "</td></tr>\n";
        html << "<tr><th>Score P90</th><td>" << scoreP90 << "</td></tr>\n";
        html << "<tr><th>Score P99</th><td>" << scoreP99 << "</td></tr>\n";
        html << "<tr><th>Score Max</th><td class=\"warn\">" << scoreMax << "</td></tr>\n";
        html << "</table>\n";

        {
            int histBins[10] = {0};
            for (double s : analyzeScores) {
                int bi = static_cast<int>(s * 30);
                if (bi < 0) bi = 0; if (bi > 9) bi = 9;
                histBins[bi]++;
            }
            int histMax = 1; for (int h : histBins) if (h > histMax) histMax = h;
            html << "<h2>Score Distribution</h2>\n";
            html << "<div style=\"font-family:monospace;font-size:12px;line-height:1.4\">\n";
            for (int i = 0; i < 10; ++i) {
                int w = (int)(60.0 * histBins[i] / histMax);
                double lo = i * 0.033;
                html << "<span style=\"color:#888\">" << std::fixed << std::setprecision(2) << lo << "</span> "
                     << "<span style=\"background:#e94560;display:inline-block;width:" << w << "px;height:12px\" "
                     << "title=\"" << histBins[i] << " tiles\"></span> "
                     << histBins[i] << "<br>\n";
            }
            html << "</div>\n";
        }

        html << "<h2>Diversity</h2><table>\n";
        html << "<tr><th>Unique Images</th><td>" << useCount.size() << " / " << n << "</td></tr>\n";
        html << "<tr><th>Reuse Ratio</th><td>" << std::fixed << std::setprecision(2) << (double)n/useCount.size() << "x</td></tr>\n";
        html << "<tr><th>Top10 Share</th><td>" << std::setprecision(1) << (100.0*top10Total/n) << "%</td></tr>\n";
        html << "</table>\n";

        html << "<h2>Top 10 Most Used</h2><table>\n";
        html << "<tr><th>Rank</th><th>Image ID</th><th>Uses</th><th>Bar</th></tr>\n";
        int topMax = topUsed.empty() ? 1 : topUsed[0].first;
        for (int i = 0; i < std::min(10, (int)topUsed.size()); ++i)
        {
            int w = (int)(100.0 * topUsed[i].first / topMax);
            html << "<tr><td>" << (i+1) << "</td><td>" << topUsed[i].second
                 << "</td><td>" << topUsed[i].first
                 << "</td><td><span class=\"bar\" style=\"width:" << w << "px\"></span></td></tr>\n";
        }
        html << "</table>\n";

        html << "<h2>Frequency Distribution</h2><table>\n";
        html << "<tr><th>Category</th><th>Count</th><th>%</th></tr>\n";
        const char* catNames[] = {"1x","2x","3x","4-5x","6-10x","10x+"};
        int htmlFreqDist[6] = {0};
        for (const auto& [id, cnt] : useCount) {
            if (cnt == 1) htmlFreqDist[0]++; else if (cnt == 2) htmlFreqDist[1]++;
            else if (cnt == 3) htmlFreqDist[2]++; else if (cnt <= 5) htmlFreqDist[3]++;
            else if (cnt <= 10) htmlFreqDist[4]++; else htmlFreqDist[5]++;
        }
        for (int i = 0; i < 6; ++i)
            html << "<tr><td>" << catNames[i] << "</td><td>" << htmlFreqDist[i]
                 << "</td><td>" << std::setprecision(1) << (100.0*htmlFreqDist[i]/n) << "%</td></tr>\n";
        html << "</table>\n";

        html << "<h2>Worst Tiles Gallery (Top 10)</h2>\n";
        html << "<div style=\"display:grid;grid-template-columns:repeat(5,1fr);gap:8px\">\n";
        constexpr int kShowWorst = 10;
        for (int k = 0; k < std::min(kShowWorst, n); ++k)
        {
            int sample = worstIdx[k].second;
            int ti = worstTiles.at(sample).tile;
            int tx = ti % tilesX, ty = ti / tilesX;
            const char *cn = (sample < (int)analyzeCat.size() && analyzeCat[sample] == 0)   ? "S"
                             : (sample < (int)analyzeCat.size() && analyzeCat[sample] == 1) ? "E"
                             : (sample < (int)analyzeCat.size() && analyzeCat[sample] == 2) ? "T"
                                                                                            : "N";
            const char* cnFull = cn[0]=='S'?"Smooth":cn[0]=='E'?"Edge":cn[0]=='T'?"Texture":"Normal";
            html << "<div style=\"background:#0f3460;padding:4px;text-align:center\">\n";
            html << "<img src=\"worst_" << std::setfill('0') << std::setw(2) << k
                 << "_s" << std::fixed << std::setprecision(4) << worstIdx[k].first
                 << "_" << cn << "_tile.png\" style=\"width:90px;height:160px\"><br>\n";
            html << "<img src=\"worst_" << std::setfill('0') << std::setw(2) << k
                 << "_match.png\" style=\"width:90px;height:160px;margin-top:2px\"><br>\n";
            html << "<span style=\"font-size:10px;color:#aaa\">(" << tx << "," << ty << ") "
                 << std::setprecision(3) << worstIdx[k].first << " " << cnFull << "</span>\n";
            html << "</div>\n";
        }
        html << "</div>\n";

        html << "<p>Heatmap: <a href=\"heatmap.png\">heatmap.png</a></p>\n";
        html << "</body></html>";
        html.close();
        std::cout << "  HTML report: " << htmlPath << "\n";
    }

    if (db.isOpen() && !useCount.empty())
    {
        if (!db.recordRunUsage(useCount, targetHash, targetPath))
        {
            throw std::runtime_error("mosaic output saved, but usage statistics could not be committed");
        }
    }
}

// ============================================================
// 主马赛克生成流程
// ============================================================
bool MosaicEngine::generate(const std::string& targetPath,
                             const std::string& dbPath,
                             const std::string& outputPath,
                             const Config& config)
{
    // 检查 CUDA 可用性，GPU 不可用时默认回退 CPU
    MosaicRequest validated{targetPath, dbPath, outputPath, config};
    std::string validationError;
    if (!validateMosaicRequest(validated, validationError))
    {
        std::cerr << "ERROR: " << validationError << std::endl;
        return false;
    }
    Config cfg = validated.config;
    if (cfg.useGpu && !cuda::isCudaAvailable())
    {
        cfg.useGpu = false;
    }

    // Benchmark 计时变量
    using Clock = std::chrono::steady_clock;
    using Ms = std::chrono::duration<double, std::milli>;
    auto tStart = Clock::now();
    double msNeighbor = 0, msSort = 0, msAnalysis = 0, msCPUFeatures = 0;
    OutputStats outputStats;
    double msFeat = 0, msANNBuild = 0, msANNLoad = 0, msANNQuery = 0, msGPUScore = 0, msSelect = 0, msPlace = 0, msEncode = 0, msCPUScore = 0;
    double msPrep = 0;  // DB 加载 + GPU library 上传累计耗时，GPU 路径专用

    // 细粒度 profile：记录各算子累计纳秒，精确定位瓶颈
    std::atomic<int64_t> opResizeNs{0};
    std::atomic<int64_t> opLabNs{0};
    std::atomic<int64_t> opGridNs{0};
    std::atomic<int64_t> opTinyNs{0};
    std::atomic<int64_t> opEdgeNs{0};
    std::atomic<int64_t> opLbpNs{0};

    // Placement 阶段 profile
    std::atomic<int64_t> opPlaceDecodeNs{0};
    std::atomic<int64_t> opPlaceResizeNs{0};
    std::atomic<int64_t> opPlaceCopyNs{0};

    std::cout << "GPU: " << (cfg.useGpu ? "CUDA enabled" : "disabled (CPU only)") << std::endl;
    cfg.print();

    cv::Mat target = imreadUnicode(targetPath, cv::IMREAD_COLOR);
    if (target.empty())
    {
        std::cerr << "ERROR: Cannot read target image: " << targetPath << std::endl;
        return false;
    }
    if (!validImageSize(target.cols, target.rows))
    {
        std::cerr << "ERROR: Target exceeds 100 million pixels." << std::endl;
        return false;
    }

    // 计算目标图哈希，用于分析报告关联和数据库使用记录
    std::string targetHash;
    {
        // 每 10000 像素采样 1 个字节，约 10KB 数据量，兼顾速度与碰撞率
        int64_t totalPixels = static_cast<int64_t>(target.rows) * target.cols;
        int step = std::max<int64_t>(int64_t{1}, totalPixels / 10000);
        uint64_t h = 0x9e3779b97f4a7c15ULL;
        const uint8_t* data = target.data;
        int64_t totalBytes = totalPixels * 3;
        for (int64_t i = 0; i < totalBytes; i += step * 3)
        {
            h ^= static_cast<uint64_t>(data[i]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        std::stringstream ss;
        ss << std::hex << h;
        targetHash = ss.str();
    }

    // 用户指定输出尺寸时，先缩放目标图到指定分辨率，后续 tile 划分基于此尺寸
    if (cfg.outW > 0 || cfg.outH > 0)
    {
        // 未指定的一轴按原图比例推导，0 保留 auto 语义。
        int64_t width =
            cfg.outW > 0
                ? cfg.outW
                : std::max<int64_t>(1, std::llround(static_cast<double>(target.cols) * cfg.outH / target.rows));
        int64_t height =
            cfg.outH > 0
                ? cfg.outH
                : std::max<int64_t>(1, std::llround(static_cast<double>(target.rows) * cfg.outW / target.cols));
        if (!validImageSize(width, height))
        {
            std::cerr << "ERROR: Target resize exceeds 100 million pixels." << std::endl;
            return false;
        }
        cfg.outW = static_cast<int>(width);
        cfg.outH = static_cast<int>(height);
        cv::Mat resized;
        cv::resize(target, resized, cv::Size(cfg.outW, cfg.outH), 0, 0, cv::INTER_AREA);
        target = resized;
        std::cout << "Target resized to: " << cfg.outW << "x" << cfg.outH << std::endl;
    }

    // --upscale：放大原图后再分 tile，提高最终输出分辨率
    if (cfg.upscale > 1)
    {
        int64_t width = static_cast<int64_t>(target.cols) * cfg.upscale;
        int64_t height = static_cast<int64_t>(target.rows) * cfg.upscale;
        if (!validImageSize(width, height))
        {
            std::cerr << "ERROR: Upscaled target exceeds 100 million pixels." << std::endl;
            return false;
        }
        cv::Mat up;
        cv::resize(target, up, cv::Size(static_cast<int>(width), static_cast<int>(height)), 0, 0, cv::INTER_LINEAR);
        target = up;
        std::cout << "Target upscaled " << cfg.upscale << "x: "
                  << target.cols << "x" << target.rows << std::endl;
    }

    Database db(dbPath, Database::OpenMode::Existing);
    if (!db.isOpen())
    {
        std::cerr << "ERROR: Cannot open database: " << dbPath << std::endl;
        return false;
    }

    int dbCount = db.totalCount();
    if (dbCount == 0)
    {
        std::cerr << "ERROR: Database is empty." << std::endl;
        return false;
    }
    std::cout << "Database: " << dbCount << " images" << std::endl;

    // Read feature resolution from DB meta (required; old DBs must be rebuilt)，旧库需重建
    std::string fw = db.getMeta("feature_w");
    std::string fh = db.getMeta("feature_h");
    if (fw.empty() || fh.empty())
    {
        std::cerr << "ERROR: Database missing feature resolution metadata." << std::endl;
        std::cerr << "  This database was built with an older version of Mosaicraft." << std::endl;
        std::cerr << "  Please rebuild with: mosaicraft build -i <photos> -d <db>" << std::endl;
        return false;
    }
    int featW = 0, featH = 0;
    auto widthResult = std::from_chars(fw.data(), fw.data() + fw.size(), featW);
    auto heightResult = std::from_chars(fh.data(), fh.data() + fh.size(), featH);
    // 元数据也属于外部输入，必须在像素乘法和 CUDA 分配之前验证。
    if (widthResult.ec != std::errc{} || widthResult.ptr != fw.data() + fw.size() || heightResult.ec != std::errc{} ||
        heightResult.ptr != fh.data() + fh.size() || featW > 8192 || featH > 8192 || !validImageSize(featW, featH))
    {
        std::cerr << "ERROR: Invalid DB feature dimensions. Rebuild the library." << std::endl;
        return false;
    }

    int featPixels = featW * featH;
    int featBytes = featPixels * 3;
    std::cout << "  (feature space: " << featW << "x" << featH << ")" << std::endl;

    // 自动检测输出 tile 方向：横屏 320x180，竖屏 180x320
    if (cfg.nativeTileW == 180 && cfg.nativeTileH == 320)
    {
        if (featW > featH)  // 横屏
        {
            cfg.nativeTileW = 320;
            cfg.nativeTileH = 180;
            std::cout << "  (auto output tile: 320x180, DB is landscape)" << std::endl;
        }
    }


    // 提取特征目录路径，供 FeaturePack / ANN 持久化缓存使用
    std::string featDirCache;
    auto allRecords = db.allRecords();  // 全量记录，GPU 路径需连续索引
    dbCount = static_cast<int>(allRecords.size());
    for (const auto& record : allRecords)
    {
        // 分析读取全部 192 维；拒绝不完整旧记录，避免越界和虚假的匹配报告。
        if (record.featureVersion != FeatureExtractor::featureVersion() || record.grid4x4.size() != 192 ||
            !std::all_of(record.grid4x4.begin(), record.grid4x4.end(),
                         [](float value)
                         {
                             return std::isfinite(value);
                         }))
        {
            std::cerr << "ERROR: Invalid grid features for image " << record.id
                      << ". Rebuild this library before generating a mosaic." << std::endl;
            return false;
        }
    }

    // 从第一条记录的 tinyPath 推断特征目录
    if (!allRecords.empty() && !allRecords[0].tinyPath.empty())
    {
        std::string firstTiny = allRecords[0].tinyPath;
        auto slashPos = firstTiny.rfind('/');
        auto backslashPos = firstTiny.rfind('\\');
        auto dirEnd = (slashPos != std::string::npos && backslashPos != std::string::npos)
            ? std::max(slashPos, backslashPos)
            : (slashPos != std::string::npos ? slashPos : backslashPos);
        if (dirEnd != std::string::npos)
            featDirCache = firstTiny.substr(0, dirEnd);
    }

    // 为 GPU 路径准备连续内存缓冲区，存放 tiny/LBP 特征数据
    cuda::GpuTimings featureTimings, scoreTimings, libraryTimings;
    FeaturePack::Reader::ReadStats libraryReadStats;
    cuda::GpuLibrary gpuLib;
    struct LibraryGuard
    {
        cuda::GpuLibrary& library;
        ~LibraryGuard()
        {
            cuda::freeLibrary(library);
        }
    } libraryGuard{gpuLib};
    auto releaseGpuLib = [&]() {
        if (gpuLib.count > 0) cuda::freeLibrary(gpuLib);
    };
    if (cfg.useGpu && !((featW == 180 && featH == 320) || (featW == 320 && featH == 180)
        || (featW == 360 && featH == 640) || (featW == 640 && featH == 360)))
    {
        std::cout << "Feature size uses CPU extraction and scoring." << std::endl;
        cfg.useGpu = false;
    }
    if (cfg.useGpu && cuda::isCudaAvailable())
    {
        std::vector<double>  h_lab(dbCount * 3);
        std::vector<float>   h_grid(dbCount * 192);
        std::vector<uint8_t> h_tiny(dbCount * 256);
        std::vector<double>  h_edge(dbCount);
        std::vector<float>   h_lbp(dbCount * 256);
        std::vector<int>     h_use(dbCount);

        // 将 DB 记录的 lab/grid 数据拷贝到连续缓冲区，减少 GPU 上传时的 I/O 碎片
        for (int i = 0; i < dbCount; ++i)
        {
            const auto& rec = allRecords[i];
            h_lab[i*3+0] = rec.avgL; h_lab[i*3+1] = rec.avgA; h_lab[i*3+2] = rec.avgB;
            if (rec.grid4x4.size() == 192)
                std::memcpy(&h_grid[i*192], rec.grid4x4.data(), 192*sizeof(float));
            h_edge[i] = rec.edgeDensity;
            h_use[i] = rec.useCount;
        }

        std::vector<int> needed(dbCount);
        std::iota(needed.begin(), needed.end(), 0);
        std::vector<uint8_t> tinyValid, lbpValid;
        const bool cacheLoaded = FeaturePack::load(featDirCache, allRecords, needed, h_tiny, h_lbp, tinyValid, lbpValid,
                                                   cfg.benchmark ? &libraryReadStats : nullptr);
        if (!cacheLoaded)
        {
            std::cout << "  (validated feature cache miss, using source files)" << std::endl;
        }
        if (cuda::uploadLibrary(gpuLib, h_lab.data(), h_grid.data(), h_tiny.data(), h_edge.data(), h_lbp.data(),
                                h_use.data(), dbCount, cfg.benchmark ? &libraryTimings : nullptr, tinyValid.data(),
                                lbpValid.data()))
        {
            std::cout << "GPU library: " << dbCount << " images ("
                      << (dbCount * (192*4+256+256*4) / 1024) << " KB)" << std::endl;
        }
        else
        {
            std::cerr << "ERROR: GPU library upload failed." << std::endl;
            releaseGpuLib();
            return false;
        }
    }

    // 计算 tile 网格：按 tileW x tileH 划分目标图
    int tilesX = (target.cols + cfg.tileW - 1) / cfg.tileW;
    int tilesY = (target.rows + cfg.tileH - 1) / cfg.tileH;

    // Read feature resolution from DB meta (required; old DBs must be rebuilt),
    // 单图模式检查 65500px 上限（JPEG 编码器限制）
    int outTileW = cfg.nativeTileW;
    int outTileH = cfg.nativeTileH;
    const int64_t proposedW = static_cast<int64_t>(tilesX) * outTileW;
    const int64_t proposedH = static_cast<int64_t>(tilesY) * outTileH;
    const int64_t limit = cfg.outputFormat == "webp" ? 16383 : 65500;
    if (!cfg.tiledOutput && (cfg.outputFormat == "jpg" || cfg.outputFormat == "webp") &&
        (proposedW > limit || proposedH > limit))
    {
        std::cerr << "ERROR: Output exceeds " << cfg.outputFormat << " dimension limit " << limit
                  << ". Choose --format png/tiff or --tiled." << std::endl;
        return false;
    }
    // 超大输出默认允许，但必须在转换为 int 前防止尺寸溢出。
    if (proposedW > INT_MAX || proposedH > INT_MAX)
    {
        std::cerr << "ERROR: Output dimensions exceed INT_MAX." << std::endl;
        return false;
    }
    int outW = static_cast<int>(proposedW);
    int outH = static_cast<int>(proposedH);

    // 宽高比校验：DB 特征空间 vs 输出 tile，featW/featH 从meta解析而来
    if (featH <= 0 || featW <= 0) {
        std::cerr << "ERROR: Invalid DB feature dimensions (" << featW << "x" << featH << ")" << std::endl;
        releaseGpuLib();
        return false;
    }
    float dbAspect = static_cast<float>(featW) / featH;
    float outAspect = static_cast<float>(outTileW) / outTileH;
    if (std::abs(dbAspect - outAspect) > 0.02f)
    {
        std::cout << "  Warning: DB aspect ratio (" << featW << "x" << featH
                  << ") differs from output tile (" << outTileW << "x" << outTileH
                  << "). Output may be distorted." << std::endl;
    }

    // 目标图尺寸不是 tile 的整数倍时，pad 补齐（不改变 tile 尺寸）
    int padRight  = tilesX * cfg.tileW - target.cols;
    int padBottom = tilesY * cfg.tileH - target.rows;
    if (padRight > 0 || padBottom > 0)
    {
        cv::copyMakeBorder(target, target, 0, padBottom, 0, padRight,
                           cv::BORDER_REFLECT);
    }

    if (tilesX == 0 || tilesY == 0)
    {
        std::cerr << "ERROR: Image too small for tile size." << std::endl;
        releaseGpuLib();
        return false;
    }

    std::cout << "Tiles: " << tilesX << " x " << tilesY
              << " = " << (static_cast<int64_t>(tilesX) * tilesY)
              << "  (output " << outW << "x" << outH
              << ", tile " << outTileW << "x" << outTileH;
    if (padRight > 0 || padBottom > 0)
    {
        std::cout << ", padded +" << padRight << "x" << padBottom;
    }
    std::cout << ")" << std::endl;
    // 打印宽高比信息
    double srcRatio = static_cast<double>(target.cols) / target.rows;
    double outRatio = static_cast<double>(outW) / outH;
    std::cout << "  Aspect: src=" << std::fixed << std::setprecision(3) << srcRatio
              << " out=" << outRatio << " (diff=" << std::abs(srcRatio - outRatio) << ")"
              << std::endl;

    // 计算总 tile 数量，溢出检查
    int64_t totalTiles64 = static_cast<int64_t>(tilesX) * tilesY;
    if (totalTiles64 > INT_MAX) {
        std::cerr << "ERROR: Too many tiles (" << totalTiles64 << ")" << std::endl;
        releaseGpuLib();
        return false;
    }
    int totalTiles = static_cast<int>(totalTiles64);

    // 进度期间隐藏光标，消除 \r 回行首时光标闪烁导致的"跳动"视觉
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci;
    bool cursorWasVisible = GetConsoleCursorInfo(hOut, &ci) && ci.bVisible;
    struct CursorRestoreGuard
    {
        HANDLE hOut = nullptr;
        bool restore = false;
        CONSOLE_CURSOR_INFO original{};
        ~CursorRestoreGuard()
        {
            if (restore) SetConsoleCursorInfo(hOut, &original);
        }
    };
    CursorRestoreGuard cursorRestore{hOut, cursorWasVisible, ci};
    if (cursorWasVisible) {
        CONSOLE_CURSOR_INFO hidden = ci;
        hidden.bVisible = FALSE;
        SetConsoleCursorInfo(hOut, &hidden);
    }
#endif

    // --analyze: ...
    std::vector<double> analyzeScores;
    std::vector<int>    analyzeImageIds;
    std::vector<double> analyzeLabD, analyzeGridD, analyzeEdgeD;
    std::vector<double> analyzeGaps;      // winner-runnerUp , , ,
    std::vector<int>    analyzeRanks;     // winner 在 Top-N 中的排名 (1-based)
    std::vector<int>    analyzeAnnRanks;  // winner 在 ANN Top200 中的排名 (0=未找到)
    std::vector<int>    analyzeCat;       // 0=Smooth, 1=Edge, 2=Texture, 3=Normal
    double analyzeGridCellSum[64] = {0};   // 每个 cell 的累计 L2 误差，用于分析网格匹配质量
    WorstTiles worstTiles;
    size_t matchingBatches = 0, matchingPeakBytes = 0, maximumBatchTiles = 0;
    double matchingWallMs = 0;
    bool matchingPrefetch = false;
    FeaturePack::Reader::ReadStats featureReadStats;

    int N = cfg.candidates;  // 候选数量，GPU 路径需提前确定，用于 benchmark

    // Benchmark 报告 lambda：汇总各阶段耗时，输出 totalTiles/N 等关键指标
    auto printBenchmark = [&](const char* label) {
        if (!cfg.benchmark) return;
        double msTotal = Ms(Clock::now() - tStart).count();
        std::cout << "\n=== Benchmark " << label << " ===\n";
        std::cout << "  Total tiles:     " << totalTiles << "\n";
        std::cout << "  Candidates/tile: " << N << "\n";
        if (msPrep > 0)
            std::cout << "  Prep (DB+GPU): " << std::fixed << std::setprecision(1) << msPrep    << " ms\n";
        std::cout << "  Features:    " << std::fixed << std::setprecision(1) << msFeat       << " ms\n";
        if (opResizeNs > 0)
        {
            auto toMs = [](int64_t ns) { return ns / 1000000.0; };
            std::cout << "    Resize:   " << std::setprecision(1) << toMs(opResizeNs) << " ms\n";
            std::cout << "    LAB:      " << std::setprecision(1) << toMs(opLabNs)    << " ms\n";
            std::cout << "    Grid:     " << std::setprecision(1) << toMs(opGridNs)   << " ms\n";
            std::cout << "    Tiny:     " << std::setprecision(1) << toMs(opTinyNs)   << " ms\n";
            std::cout << "    Edge:     " << std::setprecision(1) << toMs(opEdgeNs)   << " ms\n";
            std::cout << "    LBP:      " << std::setprecision(1) << toMs(opLbpNs)    << " ms\n";
        }
        if (msANNBuild > 0)
            std::cout << "  ANN build:   " << msANNBuild << " ms\n";
        std::cout << "  ANN load:    " << msANNLoad << " ms\n";
        std::cout << "  ANN query:   " << msANNQuery << " ms\n";
        if (msGPUScore > 0)
        {
            std::cout << "  GPU scoring: " << msGPUScore  << " ms\n";
            std::cout << "  GPU speed:   " << std::fixed << std::setprecision(0)
                      << (totalTiles * static_cast<double>(N) / msGPUScore * 1000.0)
                      << " scores/s\n";
        }
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  CPU scoring: " << msCPUScore << " ms\n";
        std::cout << "  Selection:   " << (msSelect - msCPUScore)    << " ms\n";
        std::cout << "  CPU feature loading: " << msCPUFeatures << " ms\n";
        std::cout << "  Neighbor penalties: " << msNeighbor << " ms\n";
        std::cout << "  Candidate sorting: " << msSort << " ms\n";
        std::cout << "  Analysis: " << msAnalysis << " ms\n";
        std::cout << "  Output loading (worker sum): " << outputStats.loadingMs << " ms\n";
        std::cout << "  Overlay sampling (worker sum): " << outputStats.overlaySamplingMs << " ms\n"
                  << "  Overlay blending (worker sum): " << outputStats.overlayBlendMs << " ms\n"
                  << "  Overlay reference bytes: " << outputStats.overlayReferenceBytes << "\n"
                  << "  Overlay worker budget bytes: " << outputStats.overlayWorkingBytes << "\n";
        std::cout << "  Color conversion: " << outputStats.colorMs << " ms\n";
        std::cout << "  Row encoding: " << outputStats.codecMs << " ms\n";
        std::cout << "  Producer wait: " << outputStats.producerWaitMs << " ms\n";
        std::cout << "  Consumer wait: " << outputStats.consumerWaitMs << " ms\n";
        std::cout << "  Image cache hits: " << outputStats.cacheHits << "\n"
                  << "  Image decodes: " << outputStats.cacheDecodes << "\n"
                  << "  Image cache peak bytes: " << outputStats.cachePeakBytes << "\n"
                  << "  Image shared loads: " << outputStats.cacheSharedLoads << "\n";
        std::cout << "  Placement:   " << msPlace     << " ms\n";
        std::cout << "  Encoding:    " << msEncode << " ms\n";
        if (opPlaceDecodeNs > 0)
        {
            auto toMs = [](int64_t ns) { return ns / 1000000.0; };
            std::cout << "    Decode:   " << std::setprecision(1) << toMs(opPlaceDecodeNs) << " ms\n";
            std::cout << "    Resize:   " << std::setprecision(1) << toMs(opPlaceResizeNs) << " ms\n";
            std::cout << "    Copy:     " << std::setprecision(1) << toMs(opPlaceCopyNs) << " ms\n";
        }
        if (cfg.useGpu)
        {
            std::cout << "  Library GPU allocation: " << libraryTimings.allocationMs << " ms\n"
                      << "  Library GPU upload: " << libraryTimings.uploadMs << " ms\n"
                      << "  Feature GPU allocation: " << featureTimings.allocationMs << " ms\n"
                      << "  Feature GPU upload: " << featureTimings.uploadMs << " ms\n"
                      << "  Feature GPU compute: " << featureTimings.computeMs << " ms\n"
                      << "  Feature GPU download: " << featureTimings.downloadMs << " ms\n"
                      << "  Feature CPU Canny: " << featureTimings.cannyMs << " ms\n"
                      << "  Feature GPU wait: " << featureTimings.waitMs << " ms\n"
                      << "  Feature CUDA event upload: " << featureTimings.eventUploadMs << " ms\n"
                      << "  Feature CUDA event compute: " << featureTimings.eventComputeMs << " ms\n"
                      << "  Feature CUDA event download: " << featureTimings.eventDownloadMs << " ms\n"
                      << "  Feature upload bytes: " << featureTimings.uploadBytes << "\n"
                      << "  Feature download bytes: " << featureTimings.downloadBytes << "\n"
                      << "  Feature pinned bytes: " << featureTimings.pinnedBytes << "\n"
                      << "  Feature resident bytes: " << featureTimings.residentBytes << "\n"
                      << "  Feature device pixel bytes: " << featureTimings.pixelBytes << "\n"
                      << "  Score GPU allocation: " << scoreTimings.allocationMs << " ms\n"
                      << "  Score GPU upload: " << scoreTimings.uploadMs << " ms\n"
                      << "  Score GPU compute: " << scoreTimings.computeMs << " ms\n"
                      << "  Score GPU download: " << scoreTimings.downloadMs << " ms\n"
                      << "  Score GPU wait: " << scoreTimings.waitMs << " ms\n"
                      << "  Score CUDA event upload: " << scoreTimings.eventUploadMs << " ms\n"
                      << "  Score CUDA event compute: " << scoreTimings.eventComputeMs << " ms\n"
                      << "  Score CUDA event download: " << scoreTimings.eventDownloadMs << " ms\n"
                      << "  Score upload bytes: " << scoreTimings.uploadBytes << "\n"
                      << "  Score download bytes: " << scoreTimings.downloadBytes << "\n"
                      << "  Score pinned bytes: " << scoreTimings.pinnedBytes << "\n"
                      << "  Score temporary bytes: " << scoreTimings.peakTemporaryBytes << "\n";
        }
        std::cout << "  Matching batches: " << matchingBatches << "\n"
                  << "  Matching prefetch: " << matchingPrefetch << "\n"
                  << "  Matching wall: " << matchingWallMs << " ms\n"
                  << "  Matching max tiles: " << maximumBatchTiles << "\n"
                  << "  Matching buffer peak bytes: " << matchingPeakBytes << "\n"
                  << "  Feature cache peak bytes: " << featureReadStats.peakBytes << "\n"
                  << "  Feature cache hits: " << featureReadStats.hits << "\n"
                  << "  Feature source reads: " << featureReadStats.sourceReads << "\n"
                  << "  Feature cache evictions: " << featureReadStats.evictions << "\n"
                  << "  Feature cache preloaded: " << featureReadStats.preloaded << "\n";
        std::cout << "  Library feature source reads: " << libraryReadStats.sourceReads << "\n"
                  << "  Library feature pack hits: " << libraryReadStats.packedRows << "\n";
        std::cout << "  === Total: " << msTotal     << " ms ===\n";
        if (totalTiles > 0)
            std::cout << "  Avg/tile:    " << (msTotal / totalTiles) << " ms\n";
        std::cout << std::flush;
    };

    std::atomic<int> matched{0};
    int loadFail = 0;
    int cntGrid = 0, cntMissGrid = 0;
    int cntTiny = 0, cntMissTiny = 0;
    int cntEdge = 0, cntMissEdge = 0;
    int cntLBP  = 0, cntMissLBP  = 0;

    // 自适应邻居窗口：基准 2 倍 tile 行宽，默认下限 300，上限 400
    auto autoNeighborWindow = [&]() {
        int base = std::max(300, tilesX * 2);
        int dynamic = static_cast<int>(std::sqrt(static_cast<double>(allRecords.size())) * 1.5);
        return std::max(base, std::min(dynamic, 400));
    };

    if (cfg.neighborWindow <= 0)
    {
        // 自适应邻居窗口：由 autoNeighborWindow 自动计算（下限 300，上限 400）
        // 未设置窗口大小时自动计算，否则全库扫描 O(库大小) 开销过大
        cfg.neighborWindow = autoNeighborWindow();
    }

    // 邻居去重：滑动窗口 + 频次计数，限制同图重复出现
    std::deque<int> recentIds;
    std::unordered_map<int, int> freqInWindow;
    // 强制间距：同一图片在 minGap 个 tile 内禁止复用
    const int MIN_GAP = std::max(50, tilesX);  // 至少间隔一行
    std::unordered_map<int, int> lastUsedAt;   // imageId → 最近一次使用的 tile 索引
    std::deque<int> recentGrids;
    GridDuplicateCache duplicateCache(allRecords);  // 最近 Grid 特征滑动窗口，上限 50
    constexpr double GRID_DUP_THRESHOLD = 0.010;  // Grid 相似阈值：低于此值视为重复
    constexpr double GRID_DUP_PENALTY = 200.0;     // Grid 重复惩罚分，加 200
    constexpr int GRID_DUP_WINDOW = 50;            // 滑动窗口大小，约等于一行 tile 数量

    // 权重归一化：将各特征权重转为 tile 评分系数
    double wSum = cfg.labWeight + cfg.gridWeight + cfg.tinyWeight;
    if (cfg.edgeWeight > 0) wSum += cfg.edgeWeight;
    if (cfg.lbpWeight > 0)  wSum += cfg.lbpWeight;
    double nLabW = cfg.labWeight / wSum;
    double nGridW = cfg.gridWeight / wSum;
    double nTinyW = cfg.tinyWeight / wSum;
    double nEdgeW = cfg.edgeWeight / wSum;
    double nLbpW  = cfg.lbpWeight / wSum;
    N = cfg.candidates;

    // 每个 tile 的最终选择结果：GPU 路径预分配，CPU 路径后续填充
    std::vector<int> bestLibIdx(totalTiles, -1);

    // 输出 Mat：批量模式时直接拼接，流式模式时仅作占位

    auto runAnalysis = [&](const std::string &analysisOutputPath)
    {
        writeAnalysisReport(AnalysisReportContext{cfg,
                                                  db,
                                                  analysisOutputPath,
                                                  targetPath,
                                                  targetHash,
                                                  target,
                                                  totalTiles,
                                                  tilesX,
                                                  tilesY,
                                                  outTileW,
                                                  outTileH,
                                                  featW,
                                                  featH,
                                                  dbCount,
                                                  matched.load(),
                                                  analyzeScores,
                                                  analyzeImageIds,
                                                  analyzeLabD,
                                                  analyzeGridD,
                                                  analyzeEdgeD,
                                                  analyzeGaps,
                                                  analyzeRanks,
                                                  analyzeAnnRanks,
                                                  analyzeCat,
                                                  analyzeGridCellSum,
                                                  worstTiles,
                                                  allRecords,
                                                  bestLibIdx});
    };

    // 可控匹配缓冲按预算分批；图库记录和精简选择结果仍独立持有。
    size_t batchRows =
        std::min<size_t>(totalTiles, matchingBatchRows(m_limits, featW, featH, N, dbCount, cfg.useGpu));
    maximumBatchTiles = batchRows;
    int noCandidateCount = 0;
    int grid4Top1 = 0, grid8Top1 = 0, top1Differ = 0;
    int adaptiveSmoothCount = 0, adaptiveEdgeCount = 0, adaptiveTextureCount = 0, adaptiveNormalCount = 0;
    std::vector<double> edgeDistribution, entropyDistribution;
    std::mt19937 seededRng(m_seed);
    const auto matchingStart = Clock::now();
    auto &freq = freqInWindow;
    auto downsampleGrid = [](const auto &g8, std::vector<float> &g4)
    {
        g4.resize(48);
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                for (int ch = 0; ch < 3; ++ch)
                {
                    float sum = 0;
                    for (int dr = 0; dr < 2; ++dr)
                    {
                        for (int dc = 0; dc < 2; ++dc)
                        {
                            sum += g8[((r * 2 + dr) * 8 + (c * 2 + dc)) * 3 + ch];
                        }
                    }
                    g4[(r * 4 + c) * 3 + ch] = sum / 4.0f;
                }
            }
        }
    };
    {
        const auto preparationEnd = Clock::now();
        msPrep = Ms(preparationEnd - tStart).count();
        auto annIndex = std::make_unique<FeatureIndex>();
        const std::string annPath = featDirCache.empty() ? "lib.ann196" : featDirCache + "/lib.ann196";
        const auto indexStart = Clock::now();
        bool loaded = annIndex->load(annPath, FeatureIndex::kDimension, allRecords);
        msANNLoad = Ms(Clock::now() - indexStart).count();
        if (!loaded)
        {
            const auto buildStart = Clock::now();
            std::cout << "  building ANN index (196 dimensions)..." << std::flush;
            if (!annIndex->build(allRecords))
            {
                releaseGpuLib();
                return false;
            }
            if (!annIndex->save(annPath))
            {
                std::cerr << "WARNING: ANN cache could not be saved." << std::endl;
            }
            msANNBuild = Ms(Clock::now() - buildStart).count();
        }

        WorkerPool queryWorkers;
        unsigned featureThreads = cfg.useGpu ? std::min(8u, std::max(1u, std::thread::hardware_concurrency()))
                                             : std::max(2u, std::thread::hardware_concurrency());
        WorkerPool featureWorkers(static_cast<unsigned>(std::min<size_t>(batchRows, featureThreads)));
        std::unique_ptr<cuda::FeatureWorkspace> workspace;
        std::unique_ptr<cuda::ScoreWorkspace> scoreWorkspace;
        const size_t scratch = checkedMatchProduct(N, sizeof(int) + sizeof(std::pair<double, int>)) +
                               FeatureIndex::kDimension * sizeof(float);
        bool pipeline = false;
        size_t pinnedBytes = 0;
        if (cfg.useGpu)
        {
            workspace = std::make_unique<cuda::FeatureWorkspace>();
            scoreWorkspace = std::make_unique<cuda::ScoreWorkspace>();
            // 先预算并准备全部可选资源；计算开始后不能用同步重算掩盖异步错误。
            const size_t scoreRowBytes = 9 * sizeof(double) + checkedMatchProduct(N, sizeof(int) + sizeof(double));
            while (batchRows > 1 &&
                   gpuMatchingBufferBytes(batchRows, featBytes, N, cfg.analyze) +
                           std::min<size_t>(batchRows, 256) * 2072 + batchRows * scoreRowBytes + scratch >
                       m_limits.hostBytes)
            {
                batchRows /= 2;
            }
            maximumBatchTiles = batchRows;
            const size_t hostRows = gpuMatchingBufferBytes(batchRows, featBytes, N, cfg.analyze, true);
            const size_t scoreHost = batchRows * scoreRowBytes;
            if (m_limits.cudaPipeline && hostRows + scratch + scoreHost < m_limits.hostBytes &&
                scoreRowBytes < 256ULL * 1024 * 1024)
            {
                cuda::FeaturePipelineConfig options;
                options.maxTiles = static_cast<int>(batchRows);
                options.width = featW;
                options.height = featH;
                options.downloadTiny = cfg.analyze;
                options.downloadLbp = cfg.analyze || cfg.adaptiveWeights;
                options.hostBudget = m_limits.hostBytes - hostRows - scratch - scoreHost;
                options.deviceBudget -= scoreRowBytes;
                pipeline = workspace->preparePipeline(options, cfg.benchmark ? &featureTimings : nullptr);
                if (pipeline)
                {
                    const auto allocationStart = Clock::now();
                    pipeline = scoreWorkspace->preparePipeline(static_cast<int>(batchRows), N, scoreHost,
                                                                 workspace->residentBytes());
                    scoreTimings.allocationMs += Ms(Clock::now() - allocationStart).count();
                }
                if (!pipeline)
                {
                    // 释放已部分准备的可选资源后，才允许使用完整的原同步 CUDA 路径。
                    workspace->resetPipeline();
                    scoreWorkspace->resetPipeline();
                    featureTimings.pinnedBytes = featureTimings.residentBytes = featureTimings.pixelBytes = 0;
                }
                pinnedBytes = workspace->pinnedBytes() + scoreWorkspace->pinnedBytes();
            }
        }
        std::unique_ptr<FeaturePack::Reader> featureReader;
        if (!cfg.useGpu || cfg.analyze)
        {
            const auto readStart = Clock::now();
            featureReader = std::make_unique<FeaturePack::Reader>(cfg.useGpu ? std::string() : featDirCache, allRecords,
                                                                  m_limits.featureBytes);
            if (!cfg.useGpu)
            {
                msCPUFeatures += Ms(Clock::now() - readStart).count();
            }
        }
        std::vector<std::vector<float>> libGrid4x4;
        if (cfg.useGpu && cfg.analyze)
        {
            libGrid4x4.resize(dbCount);
            for (int i = 0; i < dbCount; ++i)
            {
                downsampleGrid(allRecords[i].grid4x4, libGrid4x4[i]);
            }
        }
        MatchingBuffers buffers;
        std::vector<float> tileVector;
        std::vector<int> idxs(N);
        std::vector<std::pair<double, int>> scored;
        scored.reserve(N);
        bool hasCandidates = false;
        MatchingBuffers ahead;
        auto prepare = [&](int first, int count, MatchingBuffers &buffers)
        {
            buffers.resize(count, featBytes, N, cfg.useGpu, cfg.analyze, pipeline);
            auto &allTL = buffers.l;
            auto &allTA = buffers.a;
            auto &allTB = buffers.b;
            auto &allEdge = buffers.edge;
            auto &allGrid = buffers.grid;
            auto &allTiny = buffers.tiny;
            auto &allLBP = buffers.lbp;
            auto &allIndices = buffers.indices;
            auto &allScores = buffers.scores;
            auto &tileLabW = buffers.labW;
            auto &tileGridW = buffers.gridW;
            auto &tileTinyW = buffers.tinyW;
            auto &tileEdgeW = buffers.edgeW;
            auto &tileLbpW = buffers.lbpW;
            auto &tileGrid4x4 = buffers.grid4;
            const auto featureStart = Clock::now();
            if (cfg.useGpu)
            {
                // CUDA 沿用原有 256 张提取粒度；外层较大批次仍统一查询、评分和顺序选图。
                // 复用输入暂存，避免放大 kernel/暂存容量导致吞吐回退。
                if (pipeline)
                {
                    workspace->beginBatch(count);
                }
                for (int piece = 0; piece < count; piece += 256)
                {
                    const int pieceCount = std::min(256, count - piece);
                    auto *input = pipeline ? workspace->pipelineInput() : buffers.images.data();
                    featureWorkers.run(
                        pieceCount,
                        [&](size_t local)
                        {
                            const int ti = first + piece + static_cast<int>(local);
                            cv::Mat roi = target(
                                cv::Rect((ti % tilesX) * cfg.tileW, (ti / tilesX) * cfg.tileH, cfg.tileW, cfg.tileH));
                            cv::Mat resized(featH, featW, CV_8UC3, input + local * featBytes);
                            cv::resize(roi, resized, cv::Size(featW, featH), 0, 0, cv::INTER_LINEAR);
                        });
                    if (pipeline)
                    {
                        workspace->extract(piece, pieceCount, buffers.lab.data() + piece * 3, allGrid[piece].data(),
                                           allTiny[piece].data(), allEdge.data() + piece, allLBP[piece].data(),
                                           cfg.benchmark ? &featureTimings : nullptr);
                    }
                    else if (cuda::extractFeaturesRaw(input, pieceCount, featW, featH,
                                                 buffers.lab.data() + piece * 3, allGrid[piece].data(),
                                                 allTiny[piece].data(), allEdge.data() + piece, allLBP[piece].data(),
                                                 workspace.get(), cfg.benchmark ? &featureTimings : nullptr) < 0)
                    {
                        std::cerr << "ERROR: GPU feature extraction failed." << std::endl;
                        return false;
                    }
                }
                for (int local = 0; local < count; ++local)
                {
                    allTL[local] = buffers.lab[local * 3];
                    allTA[local] = buffers.lab[local * 3 + 1];
                    allTB[local] = buffers.lab[local * 3 + 2];
                }
            }
            else
            {
                featureWorkers.run(count,
                                   [&](size_t local)
                                   {
                                       using Ns = std::chrono::nanoseconds;
                                       const int ti = first + static_cast<int>(local);
                                       int ty = ti / tilesX, tx = ti % tilesX;
                                       cv::Mat roi =
                                           target(cv::Rect(tx * cfg.tileW, ty * cfg.tileH, cfg.tileW, cfg.tileH));
                                       cv::Mat roiNative;
                                       auto t0 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       cv::resize(roi, roiNative, cv::Size(featW, featH), 0, 0, cv::INTER_LINEAR);
                                       auto t1 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           opResizeNs += std::chrono::duration_cast<Ns>(t1 - t0).count();
                                       }
                                       cv::Mat lab;
                                       cv::cvtColor(roiNative, lab, cv::COLOR_BGR2Lab);
                                       cv::Scalar m = cv::mean(lab);
                                       allTL[local] = m[0];
                                       allTA[local] = m[1];
                                       allTB[local] = m[2];
                                       auto t2 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           opLabNs += std::chrono::duration_cast<Ns>(t2 - t1).count();
                                       }
                                       allGrid[local] = computeGrid8x8FromLab(lab);
                                       auto t3 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           opGridNs += std::chrono::duration_cast<Ns>(t3 - t2).count();
                                       }
                                       cv::Mat gray;
                                       cv::cvtColor(roiNative, gray, cv::COLOR_BGR2GRAY);
                                       allTiny[local] = computeTinyImageFromGray(gray);
                                       auto t4 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           opTinyNs += std::chrono::duration_cast<Ns>(t4 - t3).count();
                                       }
                                       allEdge[local] = computeEdgeDensityFromGray(gray);
                                       auto t5 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           opEdgeNs += std::chrono::duration_cast<Ns>(t5 - t4).count();
                                       }
                                       allLBP[local] = computeLBPHistogramFromGray(gray);
                                       auto t6 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           opLbpNs += std::chrono::duration_cast<Ns>(t6 - t5).count();
                                       }
                                   });
            }
            msFeat += Ms(Clock::now() - featureStart).count();
            const auto queryStart = Clock::now();
            for (int local = 0; local < count; ++local)
            {
                buildTileVector(allTL[local], allTA[local], allTB[local], allGrid[local], allTiny[local],
                                allEdge[local], allLBP[local], tileVector);
                std::copy(tileVector.begin(), tileVector.end(),
                          buffers.queries.begin() + static_cast<size_t>(local) * FeatureIndex::kDimension);
            }
            annIndex->queryBatch(buffers.queries, N, allIndices, queryWorkers);
            for (int local = 0; local < count; ++local)
            {
                for (int j = 0; j < N; ++j)
                {
                    auto &index = allIndices[static_cast<size_t>(local) * N + j];
                    if (index >= 0 && std::abs(allRecords[index].avgL - allTL[local]) > cfg.lRange)
                    {
                        index = -1;
                    }
                    hasCandidates = hasCandidates || index >= 0;
                }
            }
            msANNQuery += Ms(Clock::now() - queryStart).count();
            std::fill(tileLabW.begin(), tileLabW.end(), nLabW);
            std::fill(tileGridW.begin(), tileGridW.end(), nGridW);
            std::fill(tileTinyW.begin(), tileTinyW.end(), nTinyW);
            std::fill(tileEdgeW.begin(), tileEdgeW.end(), nEdgeW);
            std::fill(tileLbpW.begin(), tileLbpW.end(), nLbpW);
            if (cfg.adaptiveWeights)
            {
                for (int ti = 0; ti < count; ++ti)
                {
                    double e = allEdge[ti];
                    double lbpEnt = 0.0;
                    for (int k = 0; k < 256; ++k)
                    {
                        float v = allLBP[ti][k];
                        if (v > 0.0f)
                        {
                            lbpEnt -= v * std::log2(v);
                        }
                    }
                    double lSum = 0, lSq = 0;
                    for (int k = 0; k < 64; ++k)
                    {
                        double lv = allGrid[ti][k * 3];
                        lSum += lv;
                        lSq += lv * lv;
                    }
                    double lVar = lSq / 64.0 - (lSum / 64.0) * (lSum / 64.0);

                    if (e < 0.005 && lVar < 100.0)
                    {
                        // Smooth: 低纹理区域，侧重 LAB 颜色和 Grid 结构匹配
                        tileLabW[ti] = 0.25;
                        tileGridW[ti] = 0.45;
                        tileTinyW[ti] = 0.20;
                        tileEdgeW[ti] = 0.05;
                        tileLbpW[ti] = 0.05;
                        adaptiveSmoothCount++;
                    }
                    else if (e > 0.01)
                    {
                        // Edge-heavy: 边缘密度 > 0.01，侧重 Grid 和 Tiny 纹理
                        tileLabW[ti] = 0.15;
                        tileGridW[ti] = 0.40;
                        tileTinyW[ti] = 0.25;
                        tileEdgeW[ti] = 0.15;
                        tileLbpW[ti] = 0.05;
                        adaptiveEdgeCount++;
                    }
                    else if (lbpEnt > 3.0)
                    {
                        // Texture-heavy: LBP 熵 > 3.0，侧重 LBP 纹理特征
                        tileLabW[ti] = 0.15;
                        tileGridW[ti] = 0.40;
                        tileTinyW[ti] = 0.20;
                        tileEdgeW[ti] = 0.05;
                        tileLbpW[ti] = 0.20;
                        adaptiveTextureCount++;
                    }
                    else
                    {
                        adaptiveNormalCount++;
                    }
                }
            }

            if (cfg.useGpu && cfg.adaptiveWeights)
            {
                for (int local = 0; local < count; ++local)
                {
                    edgeDistribution.push_back(allEdge[local]);
                    double entropy = 0;
                    for (int k = 0; k < 256; ++k)
                    {
                        float value = allLBP[local][k];
                        if (value > 0.0f)
                        {
                            entropy -= value * std::log2(value);
                        }
                    }
                    entropyDistribution.push_back(entropy);
                }
            }
            return true;
        };
        const bool prefetch =
            cfg.useGpu && totalTiles > batchRows && m_limits.hostBytes >= scratch + pinnedBytes &&
            gpuMatchingBufferBytes(batchRows, featBytes, N, cfg.analyze, pipeline) <=
                (m_limits.hostBytes - scratch - pinnedBytes) / 2;
        matchingPrefetch = prefetch;
        bool preparedNext = false;
        // 池必须在缓冲/prepare 之后创建，错误退出也会先等待预取线程，再销毁它引用的数据。
        std::unique_ptr<WorkerPool> prefetchWorkers;
        if (prefetch)
        {
            prefetchWorkers = std::make_unique<WorkerPool>(1);
        }
        if (!prepare(0, static_cast<int>(batchRows), buffers))
        {
            return false;
        }
        std::cout << "Matching: ordered batches, max " << batchRows << " tiles"
                  << (prefetch ? " (one batch ahead)" : "") << std::endl;
        for (int first = 0; first < totalTiles;)
        {
            const int count = static_cast<int>(std::min<size_t>(batchRows, totalTiles - first));
            const int next = first + count;
            const int nextCount = static_cast<int>(std::min<size_t>(batchRows, totalTiles - next));
            ++matchingBatches;
            auto &allTL = buffers.l;
            auto &allTA = buffers.a;
            auto &allTB = buffers.b;
            auto &allEdge = buffers.edge;
            auto &allGrid = buffers.grid;
            auto &allTiny = buffers.tiny;
            auto &allLBP = buffers.lbp;
            auto &allIndices = buffers.indices;
            auto &allScores = buffers.scores;
            auto &tileLabW = buffers.labW;
            auto &tileGridW = buffers.gridW;
            auto &tileTinyW = buffers.tinyW;
            auto &tileEdgeW = buffers.edgeW;
            auto &tileLbpW = buffers.lbpW;
            auto &tileGrid4x4 = buffers.grid4;
            if (cfg.useGpu)
            {
                const auto scoreStart = Clock::now();
                cuda::GpuTimings batchTimings;
                const bool scoredOnGpu = pipeline
                    ? cuda::scoreDeviceBatch(workspace->deviceView(0, count), allTL.data(), allTA.data(), allTB.data(),
                                             allEdge.data(), allIndices.data(), N, gpuLib, tileLabW.data(),
                                             tileGridW.data(), tileTinyW.data(), tileEdgeW.data(), tileLbpW.data(),
                                             cfg.usePenalty, allScores.data(), *scoreWorkspace,
                                             cfg.benchmark ? &batchTimings : nullptr)
                    : cuda::scoreBatch(count, allTL.data(), allTA.data(), allTB.data(), allGrid.data(), allTiny.data(),
                                      allEdge.data(), allLBP.data(), allIndices.data(), N, gpuLib, tileLabW.data(),
                                      tileGridW.data(), tileTinyW.data(), tileEdgeW.data(), tileLbpW.data(),
                                      cfg.usePenalty, allScores.data(), cfg.benchmark ? &batchTimings : nullptr,
                                      256ULL * 1024 * 1024, scoreWorkspace.get());
                if (!scoredOnGpu)
                {
                    std::cerr << "ERROR: GPU scoring failed; mosaic was not generated." << std::endl;
                    return false;
                }
                scoreTimings.allocationMs += batchTimings.allocationMs;
                scoreTimings.uploadMs += batchTimings.uploadMs;
                scoreTimings.computeMs += batchTimings.computeMs;
                scoreTimings.downloadMs += batchTimings.downloadMs;
                scoreTimings.waitMs += batchTimings.waitMs;
                scoreTimings.eventUploadMs += batchTimings.eventUploadMs;
                scoreTimings.eventComputeMs += batchTimings.eventComputeMs;
                scoreTimings.eventDownloadMs += batchTimings.eventDownloadMs;
                scoreTimings.uploadBytes += batchTimings.uploadBytes;
                scoreTimings.downloadBytes += batchTimings.downloadBytes;
                scoreTimings.pinnedBytes = std::max(scoreTimings.pinnedBytes, batchTimings.pinnedBytes);
                scoreTimings.peakTemporaryBytes =
                    std::max(scoreTimings.peakTemporaryBytes, batchTimings.peakTemporaryBytes);
                msGPUScore += Ms(Clock::now() - scoreStart).count();
                if (prefetch && nextCount > 0)
                {
                    // 下一批只准备与邻域状态无关的数据；评分和选择仍由当前线程顺序执行。
                    preparedNext = false;
                    prefetchWorkers->start(1,
                                           [&, next, nextCount](size_t)
                                           {
                                               preparedNext = prepare(next, nextCount, ahead);
                                           });
                }
                const auto selectionStart = Clock::now();
                if (cfg.analyze)
                {
                    for (int local = 0; local < count; ++local)
                    {
                        downsampleGrid(allGrid[local], tileGrid4x4[local]);
                    }
                }
                for (int local = 0; local < count; ++local)
                {
                    const int ti = first + local;
                    const size_t rowOffset = static_cast<size_t>(local) * static_cast<size_t>(N);
                    double *scores = &allScores[rowOffset];
                    const int *indices = &allIndices[rowOffset];
                    // 统计有效候选数，跳过全为 -1 的 tile
                    int validCount = 0;
                    for (int j = 0; j < N; ++j)
                    {
                        if (indices[j] >= 0)
                        {
                            validCount++;
                        }
                    }
                    if (validCount == 0)
                    {
                        noCandidateCount++;
                        continue;
                    }
                    // 频次分级惩罚：1次轻微、2次中等、3+次重度（上限封顶）
                    const auto neighborStart = cfg.benchmark ? Clock::now() : Clock::time_point{};
                    for (int j = 0; j < N; ++j)
                    {
                        int libIdx = indices[j];
                        if (libIdx < 0 || libIdx >= static_cast<int>(allRecords.size()))
                        {
                            continue;
                        }
                        int imgId = allRecords[libIdx].id;
                        auto it = freqInWindow.find(imgId);
                        int cnt = (it != freqInWindow.end()) ? it->second : 0;
                        if (cnt >= 3)
                        {
                            scores[j] += cfg.neighborPenalty;
                        }
                        else if (cnt == 2)
                        {
                            scores[j] += cfg.neighborPenalty * 0.4;
                        }
                        else if (cnt == 1)
                        {
                            scores[j] += cfg.neighborPenalty * 0.1;
                        }
                        // 强制间距：同一图片在 MIN_GAP 内复用加 500 分惩罚
                        auto gapIt = lastUsedAt.find(imgId);
                        if (gapIt != lastUsedAt.end() && (ti - gapIt->second) < MIN_GAP)
                        {
                            scores[j] += 500.0;
                        }
                        // Grid 去重：当前 tile 与最近窗口内 Grid 相似则加惩罚
                        for (const auto &rg : recentGrids)
                        {
                            if (duplicateCache.similar(indices[j], rg, GRID_DUP_THRESHOLD))
                            {
                                scores[j] += GRID_DUP_PENALTY;
                                break;
                            }
                        }
                    }
                    // Top-N 随机选取：从前 topN 个最佳候选中随机选择
                    // 8, 8 vs 4, 4 Grid 对比实验（--analyze 模式）
                    if (cfg.benchmark)
                    {
                        msNeighbor += Ms(Clock::now() - neighborStart).count();
                    }
                    if (cfg.analyze && validCount > 0)
                    {
                        BenchmarkTimer analysisTimer(cfg.benchmark, msAnalysis);
                        double best4 = 1e30, best8 = 1e30;
                        int best4idx = -1, best8idx = -1;
                        for (int j = 0; j < N; ++j)
                        {
                            if (indices[j] < 0)
                            {
                                continue;
                            }
                            // GPU scores 基于 8x8 grid，4x4 分数 = 原分数 - 8x8 grid项 + 4x4 grid项
                            double grid8d = gridDistance8x8(allGrid[local], allRecords[indices[j]].grid4x4, true);
                            double grid4d = gridDistance(tileGrid4x4[local], libGrid4x4[indices[j]]);
                            double score4 = scores[j] - nGridW * grid8d + nGridW * grid4d;
                            double score8 = scores[j]; // GPU , ,  8, 8
                            if (score4 < best4)
                            {
                                best4 = score4;
                                best4idx = indices[j];
                            }
                            if (score8 < best8)
                            {
                                best8 = score8;
                                best8idx = indices[j];
                            }
                        }
                        if (best4idx >= 0)
                        {
                            grid4Top1++;
                        }
                        if (best8idx >= 0)
                        {
                            grid8Top1++;
                        }
                        if (best4idx != best8idx)
                        {
                            top1Differ++;
                        }
                    }

                    const auto sortStart = cfg.benchmark ? Clock::now() : Clock::time_point{};
                    std::iota(idxs.begin(), idxs.end(), 0);
                    int topN = std::min(cfg.topNrandom, std::min(N, validCount));
                    std::partial_sort(idxs.begin(), idxs.begin() + topN, idxs.end(),
                                      [&](int a, int b)
                                      {
                                          return scores[a] < scores[b];
                                      });
                    if (cfg.benchmark)
                    {
                        msSort += Ms(Clock::now() - sortStart).count();
                    }
                    thread_local std::mt19937 rng(std::random_device{}());
                    int rankPos = std::uniform_int_distribution<int>(0, topN - 1)(
                        m_fixedSeed ? seededRng : rng); // 随机选取 0-based 索引，存储时转为 rank-1
                    int pick = idxs[rankPos];
                    int chosenLibIdx = indices[pick];
                    bestLibIdx[ti] = chosenLibIdx;
                    // --analyze: 记录每个 tile 的详细匹配数据，用于后续分析报告
                    if (cfg.analyze)
                    {
                        BenchmarkTimer analysisTimer(cfg.benchmark, msAnalysis);
                        const auto &rec = allRecords[chosenLibIdx];
                        double labD =
                            labDistance(allTL[local], allTA[local], allTB[local], rec.avgL, rec.avgA, rec.avgB);
                        double gridD = gridDistance8x8(allGrid[local], rec.grid4x4, true);
                        const auto &features = featureReader->get(chosenLibIdx);
                        const auto *recTiny = features.tinyValid ? &features.tiny : nullptr;
                        const auto *recLBP = features.lbpValid ? &features.lbp : nullptr;
                        double tinyD = recTiny ? tinyMSE(allTiny[local], *recTiny) : 1.0;
                        double edgeD = std::abs(allEdge[local] - rec.edgeDensity);
                        double lbpD = recLBP ? lbpDistance(allLBP[local], *recLBP) : 1.0;
                        double totalW =
                            cfg.labWeight + cfg.gridWeight + cfg.tinyWeight + cfg.edgeWeight + cfg.lbpWeight;
                        double featScore = (cfg.labWeight * labD + cfg.gridWeight * gridD + cfg.tinyWeight * tinyD +
                                            cfg.edgeWeight * edgeD + cfg.lbpWeight * lbpD) /
                                           totalW;
                        analyzeScores.push_back(featScore);
                        worstTiles.remember(featScore, static_cast<int>(analyzeScores.size()) - 1, ti, allTL[local],
                                            allTA[local], allTB[local], allEdge[local], allGrid[local]);
                        analyzeImageIds.push_back(rec.id);
                        analyzeLabD.push_back(labD);
                        analyzeGridD.push_back(gridD);
                        analyzeEdgeD.push_back(edgeD);

                        // Top-K Gap: winner vs true best 的分数差距，衡量随机选取代价
                        double winnerScore = scores[pick];
                        double gap = 0.0;
                        if (validCount >= 2)
                        {
                            if (rankPos == 0) // winner , , ,
                            {
                                gap = scores[idxs[1]] - winnerScore;
                            }
                            else // 未选第一名时，gap 为正
                            {
                                gap = winnerScore - scores[idxs[0]];
                            }
                        }
                        analyzeGaps.push_back(gap);
                        analyzeRanks.push_back(rankPos + 1); // 1-based rank in sorted Top-N

                        // ANN rank: winner 在 ANN 返回列表中的位置 (-1=未找到, 0=Top1)
                        int annRank = -1;
                        for (int j = 0; j < N; ++j)
                        {
                            if (allIndices[static_cast<size_t>(local) * static_cast<size_t>(N) +
                                           static_cast<size_t>(j)] == chosenLibIdx)
                            {
                                annRank = j;
                                break;
                            }
                        }
                        analyzeAnnRanks.push_back(annRank);

                        // 记录 tile 分类（用于自适应权重分析）
                        int cat = 3; // Normal
                        if (allEdge[local] < 0.005)
                        {
                            double lSum = 0, lSq = 0;
                            for (int k = 0; k < 64; ++k)
                            {
                                double lv = allGrid[local][k * 3];
                                lSum += lv;
                                lSq += lv * lv;
                            }
                            double lVar = lSq / 64.0 - (lSum / 64.0) * (lSum / 64.0);
                            if (lVar < 100.0)
                            {
                                cat = 0; // Smooth
                            }
                        }
                        else if (allEdge[local] > 0.01)
                        {
                            cat = 1;
                        } // Edge
                        else
                        {
                            double lbpEnt = 0.0;
                            for (int k = 0; k < 256; ++k)
                            {
                                float v = allLBP[local][k];
                                if (v > 0.0f)
                                {
                                    lbpEnt -= v * std::log2(v);
                                }
                            }
                            if (lbpEnt > 3.0)
                            {
                                cat = 2; // Texture
                            }
                        }
                        analyzeCat.push_back(cat);

                        // Grid 8x8 每个 cell 的 LAB 误差累计，用于分析网格匹配质量
                        for (int ci = 0; ci < 64; ++ci)
                        {
                            int off = ci * 3;
                            double dl = allGrid[local][off] / 255.0 - rec.grid4x4[off] / 255.0;
                            double da = allGrid[local][off + 1] / 255.0 - rec.grid4x4[off + 1] / 255.0;
                            double db = allGrid[local][off + 2] / 255.0 - rec.grid4x4[off + 2] / 255.0;
                            analyzeGridCellSum[ci] += std::sqrt(dl * dl + da * da + db * db);
                        }
                    }
                    // 更新滑动窗口和频次统计
                    int chosenId = allRecords[bestLibIdx[ti]].id;
                    recentIds.push_back(chosenId);
                    freqInWindow[chosenId]++;
                    lastUsedAt[chosenId] = ti; // 记录最近使用位置
                    recentGrids.push_back(chosenLibIdx);
                    while (static_cast<int>(recentGrids.size()) > GRID_DUP_WINDOW)
                    {
                        recentGrids.pop_front();
                    }
                    if (static_cast<int>(recentIds.size()) > cfg.neighborWindow)
                    {
                        int oldId = recentIds.front();
                        recentIds.pop_front();
                        if (--freqInWindow[oldId] <= 0)
                        {
                            freqInWindow.erase(oldId);
                        }
                    }
                }

                msSelect += Ms(Clock::now() - selectionStart).count();
            }
            else
            {
                const auto readStart = Clock::now();
                featureReader->prepare(allIndices);
                const double readMs = Ms(Clock::now() - readStart).count();
                msCPUFeatures += readMs;
                msSelect += readMs;
                const auto selectionStart = Clock::now();
                for (int local = 0; local < count; ++local)
                {
                    const int ti = first + local;
                    scored.clear();
                    const auto scoreStart = cfg.benchmark ? Clock::now() : Clock::time_point{};
                    for (int j = 0; j < N; ++j)
                    {
                        int li = allIndices[static_cast<size_t>(local) * N + j];
                        if (li < 0)
                        {
                            continue;
                        }
                        const auto &r = allRecords[li];
                        const auto &features = featureReader->get(li);

                        double labD = tileLabW[local] *
                                      labDistance(allTL[local], allTA[local], allTB[local], r.avgL, r.avgA, r.avgB);
                        double gridD = tileGridW[local] * gridDistance8x8(allGrid[local], r.grid4x4, true);
                        double edgeD = tileEdgeW[local] * std::abs(allEdge[local] - r.edgeDensity);
                        double s =
                            labD + gridD +
                            tileTinyW[local] * (features.tinyValid ? tinyMSE(allTiny[local], features.tiny) : 1.0) +
                            edgeD +
                            tileLbpW[local] * (features.lbpValid ? lbpDistance(allLBP[local], features.lbp) : 1.0) +
                            r.useCount * cfg.usePenalty;
                        BenchmarkTimer neighborTimer(cfg.benchmark, msNeighbor);
                        auto it = freq.find(r.id);
                        int cnt = (it != freq.end()) ? it->second : 0;
                        if (cnt >= 3)
                        {
                            s += cfg.neighborPenalty;
                        }
                        else if (cnt == 2)
                        {
                            s += cfg.neighborPenalty * 0.4;
                        }
                        else if (cnt == 1)
                        {
                            s += cfg.neighborPenalty * 0.1;
                        }
                        auto gapIt = lastUsedAt.find(r.id); // 强制间距检查
                        if (gapIt != lastUsedAt.end() && (ti - gapIt->second) < MIN_GAP)
                        {
                            s += 500.0;
                        }
                        for (const auto &rg : recentGrids)
                        {
                            if (duplicateCache.similar(li, rg, GRID_DUP_THRESHOLD))
                            {
                                s += GRID_DUP_PENALTY;
                                break;
                            }
                        }
                        scored.push_back({s, li});
                    }
                    if (cfg.benchmark)
                    {
                        // 候选评分包含原有邻域惩罚；排序和最终选图另计。
                        msCPUScore += Ms(Clock::now() - scoreStart).count();
                    }
                    if (scored.empty())
                    {
                        noCandidateCount++;
                        continue;
                    }
                    const auto sortStart = cfg.benchmark ? Clock::now() : Clock::time_point{};
                    const int topN = std::min(cfg.topNrandom, static_cast<int>(scored.size()));
                    std::partial_sort(scored.begin(), scored.begin() + topN, scored.end());
                    if (cfg.benchmark)
                    {
                        msSort += Ms(Clock::now() - sortStart).count();
                    }
                    thread_local std::mt19937 rng2(std::random_device{}());
                    int pickIdx =
                        scored[std::uniform_int_distribution<int>(0, topN - 1)(m_fixedSeed ? seededRng : rng2)].second;
                    bestLibIdx[ti] = pickIdx;
                    if (cfg.analyze)
                    {
                        BenchmarkTimer analysisTimer(cfg.benchmark, msAnalysis);
                        const auto &w = allRecords[pickIdx];
                        const auto &winnerFeatures = featureReader->get(pickIdx);

                        // --analyze: 仅记录胜出者的匹配数据，每个 tile 一条记录

                        double wLabD = tileLabW[local] *
                                       labDistance(allTL[local], allTA[local], allTB[local], w.avgL, w.avgA, w.avgB);
                        double wGridD = tileGridW[local] * gridDistance8x8(allGrid[local], w.grid4x4, true);
                        double wEdgeD = tileEdgeW[local] * std::abs(allEdge[local] - w.edgeDensity);
                        double wS =
                            wLabD + wGridD + wEdgeD +
                            tileTinyW[local] *
                                (winnerFeatures.tinyValid ? tinyMSE(allTiny[local], winnerFeatures.tiny) : 1.0) +
                            tileLbpW[local] *
                                (winnerFeatures.lbpValid ? lbpDistance(allLBP[local], winnerFeatures.lbp) : 1.0);
                        analyzeScores.push_back(wS);
                        worstTiles.remember(wS, static_cast<int>(analyzeScores.size()) - 1, ti, allTL[local],
                                            allTA[local], allTB[local], allEdge[local], allGrid[local]);
                        analyzeImageIds.push_back(w.id);
                        analyzeLabD.push_back(wLabD);
                        analyzeGridD.push_back(wGridD);
                        analyzeEdgeD.push_back(wEdgeD);
                    }
                    // 更新滑动窗口统计
                    int chosenId = allRecords[bestLibIdx[ti]].id;
                    recentIds.push_back(chosenId);
                    freq[chosenId]++;
                    lastUsedAt[chosenId] = ti;
                    recentGrids.push_back(pickIdx);
                    while (static_cast<int>(recentGrids.size()) > GRID_DUP_WINDOW)
                    {
                        recentGrids.pop_front();
                    }
                    if ((int)recentIds.size() > cfg.neighborWindow)
                    {
                        int old = recentIds.front();
                        recentIds.pop_front();
                        if (--freq[old] <= 0)
                        {
                            freq.erase(old);
                        }
                    }
                }

                msSelect += Ms(Clock::now() - selectionStart).count();
            }
            if (prefetch && nextCount > 0)
            {
                prefetchWorkers->wait();
                if (!preparedNext)
                {
                    return false;
                }
            }
            const size_t temporary = featureReader ? featureReader->stats().temporaryPeakBytes : 0;
            matchingPeakBytes = std::max(matchingPeakBytes, buffers.capacityBytes() + ahead.capacityBytes() +
                                                                temporary + pinnedBytes + idxs.capacity() * sizeof(int) +
                                                                scored.capacity() * sizeof(std::pair<double, int>) +
                                                                tileVector.capacity() * sizeof(float));
            first += count;
            std::cout << "\r  matching " << first << "/" << totalTiles << std::flush;
            if (nextCount > 0)
            {
                if (prefetch)
                {
                    std::swap(buffers, ahead);
                }
                else if (!prepare(first, nextCount, buffers))
                {
                    return false;
                }
            }
        }
        if (featureReader)
        {
            featureReadStats = featureReader->stats();
        }
        if (!hasCandidates)
        {
            std::cerr << "ERROR: No candidates within --l-range; increase the brightness range." << std::endl;
            return false;
        }
    } // 索引、缓存、线程池及大批次缓冲在贴图/编码之前释放。
    matchingWallMs = Ms(Clock::now() - matchingStart).count();
    if (m_selectedObserver)
    {
        *m_selectedObserver = bestLibIdx;
    }
    std::cout << " done" << std::endl;
    if (cfg.useGpu && cfg.adaptiveWeights)
    {
        std::sort(edgeDistribution.begin(), edgeDistribution.end());
        std::sort(entropyDistribution.begin(), entropyDistribution.end());
        auto pct = [](const auto &values, double p)
        {
            return values[std::min(values.size() - 1, static_cast<size_t>(p * values.size()))];
        };
        std::cout << "  Edge: P50=" << std::fixed << std::setprecision(3) << pct(edgeDistribution, 0.50)
                  << " P90=" << pct(edgeDistribution, 0.90) << " P95=" << pct(edgeDistribution, 0.95)
                  << " P99=" << pct(edgeDistribution, 0.99) << "\n"
                  << "  LBP: P50=" << std::setprecision(2) << pct(entropyDistribution, 0.50)
                  << " P90=" << pct(entropyDistribution, 0.90) << " P95=" << pct(entropyDistribution, 0.95)
                  << " P99=" << pct(entropyDistribution, 0.99) << "\n"
                  << "  Class: S=" << adaptiveSmoothCount << " E=" << adaptiveEdgeCount << " T=" << adaptiveTextureCount
                  << " N=" << adaptiveNormalCount << "\n";
    }
    std::vector<double>().swap(edgeDistribution);
    std::vector<double>().swap(entropyDistribution);
    if (cfg.useGpu && cfg.analyze && totalTiles > noCandidateCount)
    {
        std::cout << "  Grid 8x8 experiment: Top1 differ=" << top1Differ << "/" << totalTiles - noCandidateCount
                  << "\n";
    }
    cntGrid = cntTiny = cntEdge = cntLBP = totalTiles;
    if (noCandidateCount)
    {
        std::cout << "  " << noCandidateCount << " tiles had no candidates\n";
    }
    if (!cfg.analyze && cfg.overlayStrength == 0)
    {
        target.release();
    }

    releaseGpuLib();
    if (!writeIndexedMosaicOutput(cfg, outputPath, tilesX, tilesY, outTileW, outTileH, allRecords, bestLibIdx,
                                  adjustColor, outputStats, target))
    {
        return false;
    }
    matched = outputStats.matched;
    loadFail = outputStats.failed;
    msPlace = outputStats.placementMs;
    msEncode = outputStats.encodingMs;
    std::cout << "Mosaic saved: " << outputStats.path << " (" << matched << " / " << totalTiles
              << " tiles, loadFail=" << loadFail << ")" << std::endl;
    // 修正特征计数器：各路径可能在 return 前未完整设置
    if (cntEdge + cntMissEdge == 0)
    {
        cntEdge = totalTiles;
        cntMissEdge = 0;
    }
    if (cntGrid + cntMissGrid == 0)
    {
        cntGrid = totalTiles;
        cntMissGrid = 0;
    }
    if (cntTiny + cntMissTiny == 0)
    {
        cntTiny = totalTiles;
        cntMissTiny = 0;
    }
    if (cntLBP + cntMissLBP == 0)
    {
        cntLBP = totalTiles;
        cntMissLBP = 0;
    }
    std::cout << "  Features used:"
              << " grid=" << cntGrid << "/" << (cntGrid + cntMissGrid) << " tiny=" << cntTiny << "/"
              << (cntTiny + cntMissTiny) << " edge=" << cntEdge << "/" << (cntEdge + cntMissEdge) << " lbp=" << cntLBP
              << "/" << (cntLBP + cntMissLBP) << std::endl;

    // 根据扩展名与 --format 自动切换或保持原路径格式一致
    {
        BenchmarkTimer analysisTimer(cfg.benchmark, msAnalysis);
        runAnalysis(outputStats.path);
    }

    releaseGpuLib();

#ifdef _WIN32
#endif

    // 运行分析报告（如有 --analyze）
    printBenchmark("single");
    return true;
}

} // namespace mosaicraft
