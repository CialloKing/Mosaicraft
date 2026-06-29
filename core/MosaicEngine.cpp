#include "MosaicEngine.h"
#include "BigTiffWriter.h"
#include "Database.h"
#include "DeepZoomWriter.h"
#include "PngStreamWriter.h"
#include "PngBatchWriter.h"
#include "JpgStreamWriter.h"
#include "JpgStreamWriter.h"
#include "FeatureIndex.h"
#include "FeaturePack.h"
#include "FeatureUtils.h"
#include "ImageCache.h"
#include "UnicodeIO.h"
#include "compute/CudaBackend.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
// , , , ,
// ============================================================
class FeatureCache
{
public:
    const std::vector<uint8_t>* loadTiny(int id, const std::string& path)
    {
        auto it = m_tiny.find(id);
        if (it != m_tiny.end()) { return &it->second; }

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) { return nullptr; }

        std::vector<uint8_t> data(256);
        ifs.read(reinterpret_cast<char*>(data.data()), 256);
        if (ifs.gcount() != 256) { return nullptr; }

        auto [ins, _] = m_tiny.emplace(id, std::move(data));
        return &ins->second;
    }

    const std::vector<float>* loadLBP(int id, const std::string& path)
    {
        auto it = m_lbp.find(id);
        if (it != m_lbp.end()) { return &it->second; }

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) { return nullptr; }

        std::vector<float> data(256);
        ifs.read(reinterpret_cast<char*>(data.data()),
                 256 * sizeof(float));
        if (ifs.gcount() != static_cast<std::streamsize>(256 * sizeof(float)))
        {
            return nullptr;
        }

        auto [ins, _] = m_lbp.emplace(id, std::move(data));
        return &ins->second;
    }

private:
    std::unordered_map<int, std::vector<uint8_t>> m_tiny;
    std::unordered_map<int, std::vector<float>> m_lbp;
};

// ============================================================
// 锟�??诧拷, 色校, , , 锟轿??�? , �? , , �? , , , , , �? �?
// ,  HSV 锟秸硷拷, , 锟紿 �? , 锟戒，S/V �? ,  [1-strength, 1+strength] , �? , , , �?
// ============================================================
// 锟�??诧拷, 色微, , ,  LAB 锟秸硷拷锟轿??拷锟?L, , 锟饺ｏ拷�?
// LAB , �? 锟饺ｏ拷, ,  L , ,  AB ,  , �? 锟戒�? �? ,
// L , �? [-strength, +strength] �? , , , 浒�??拷锟�??拷锟?
static void adjustColor(cv::Mat& img, double strength)
{
    cv::Mat lab;
    cv::cvtColor(img, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels(3);
    cv::split(lab, channels);
    // channels[0]=L, [1]=A, [2]=B
    // L , 锟接ｏ拷[-s, +s] �? , , , , 锟饺ｏ拷锟�??程帮拷全, thread_local , , , �?
    thread_local std::mt19937 rng(std::random_device{}());
    double lFactor = 1.0 + ((static_cast<int>(rng() % 1001) - 300) / 1000.0) * strength;
    channels[0] = channels[0] * lFactor;
    cv::merge(channels, lab);
    cv::cvtColor(lab, img, cv::COLOR_Lab2BGR);
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
    const std::vector<double>& allTL;
    const std::vector<double>& allTA;
    const std::vector<double>& allTB;
    const std::vector<double>& allEdge;
    const std::vector<std::vector<float>>& allGrid;
    const std::vector<ImageRecord>& allRecords;
    const std::vector<ImageRecord>& bestRecords;
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
    const auto& allTL = ctx.allTL;
    const auto& allTA = ctx.allTA;
    const auto& allTB = ctx.allTB;
    const auto& allEdge = ctx.allEdge;
    const auto& allGrid = ctx.allGrid;
    const auto& allRecords = ctx.allRecords;
    const auto& bestRecords = ctx.bestRecords;

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
    std::vector<std::pair<double,int>> worstIdx;
    for (int i = 0; i < n; ++i)
        worstIdx.push_back({analyzeScores[i], i});
    std::sort(worstIdx.rbegin(), worstIdx.rend());
    for (int k = 0; k < std::min(5, n); ++k)
    {
        int ti = worstIdx[k].second;
        int tx = ti % tilesX, ty = ti / tilesX;
        std::cout << "    (" << tx << "," << ty << ") score="
                  << std::fixed << std::setprecision(4) << worstIdx[k].first;
        if (ti < static_cast<int>(analyzeCat.size()))
        {
            const char* cn = (analyzeCat[ti]==0)?"Smooth":
                             (analyzeCat[ti]==1)?"Edge":
                             (analyzeCat[ti]==2)?"Texture":"Normal";
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
    std::filesystem::create_directories(u8path(anaDir));

    std::string freqDir = anaDir + "/freq_rank";
    std::filesystem::create_directories(u8path(freqDir));
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
                    char fn[256];
                    std::string normFile = std::filesystem::path(allRecords[i].filePath).filename().string();
                    snprintf(fn, sizeof(fn), "%s/rank%02d_%dx_id%d_%s",
                             freqDir.c_str(), exported + 1, cnt, id, normFile.c_str());
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
        int ti = worstIdx[k].second;
        int tx = ti % tilesX, ty = ti / tilesX;
        char fname[256];
        cv::Mat tileROI = target(cv::Rect(tx*cfg.tileW, ty*cfg.tileH, cfg.tileW, cfg.tileH));
        cv::Mat tileBig;
        cv::resize(tileROI, tileBig, cv::Size(featW, featH), 0, 0, cv::INTER_NEAREST);
        const char* cn = (ti < static_cast<int>(analyzeCat.size()) && analyzeCat[ti]==0)?"S":
                         (ti < static_cast<int>(analyzeCat.size()) && analyzeCat[ti]==1)?"E":
                         (ti < static_cast<int>(analyzeCat.size()) && analyzeCat[ti]==2)?"T":"N";
        snprintf(fname, sizeof(fname), "%s/worst_%02d_s%.4f_%s_tile.png",
                 anaDir.c_str(), k, worstIdx[k].first, cn);
        imwriteUnicode(fname, tileBig);
        if (ti < static_cast<int>(bestRecords.size()) && !bestRecords[ti].filePath.empty())
        {
            cv::Mat match = imreadUnicode(bestRecords[ti].filePath, cv::IMREAD_COLOR);
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
            int ti = worstIdx[k].second, tx = ti % tilesX, ty = ti / tilesX;
            const auto& rec = bestRecords[ti];
            double labD = labDistance(allTL[ti],allTA[ti],allTB[ti],rec.avgL,rec.avgA,rec.avgB);
            double gridD = gridDistance8x8(allGrid[ti], rec.grid4x4, true);
            double edgeD = std::abs(allEdge[ti] - rec.edgeDensity);
            rpt << "\n#" << (k+1) << " tile(" << tx << "," << ty << ") score=" << worstIdx[k].first << "\n";
            rpt << "  Tile  LAB=" << allTL[ti] << "," << allTA[ti] << "," << allTB[ti]
                << " Edge=" << allEdge[ti] << "\n";
            rpt << "  Match LAB=" << rec.avgL << "," << rec.avgA << "," << rec.avgB
                << " Edge=" << rec.edgeDensity << "\n";
            rpt << "  Dists: LAB=" << labD << " Grid=" << gridD << " Edge=" << edgeD << "\n";
            std::string cause;
            if (labD > 0.3) cause = "color mismatch (LAB dist " + std::to_string(labD).substr(0,4) + ")";
            else if (gridD > 0.5) cause = "spatial mismatch (Grid dist " + std::to_string(gridD).substr(0,4) + ")";
            else if (allTL[ti] < 60) cause = "dark region (tile L=" + std::to_string((int)allTL[ti]) + ")";
            else if (rec.useCount > 10) cause = "popular image penalty (used " + std::to_string(rec.useCount) + "x)";
            else cause = "combined mismatch";
            rpt << "  Cause: " << cause << "\n";
        }
        std::cout << "  Diagnosis report: " << rptPath << "\n";
    }

    std::string heatPath = anaDir + "/heatmap.png";
    double sMin = sortedScores.front(), sRange = sortedScores.back() - sMin;
    if (sRange < 0.001) sRange = 0.001;
    cv::Mat heat(tilesY * 4, tilesX * 4, CV_8UC3);
    for (int ty = 0; ty < tilesY; ++ty)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            int ti = ty * tilesX + tx;
            if (ti >= n) continue;
            double s = analyzeScores[ti];
            double t = (s - sMin) / sRange;
            cv::Vec3b color;
            if (t < 0.5)
                color = cv::Vec3b(0, static_cast<uchar>(255*t*2), static_cast<uchar>(255*(1-t*2)));
            else
                color = cv::Vec3b(0, static_cast<uchar>(255*(1-(t-0.5)*2)), static_cast<uchar>(255));
            cv::rectangle(heat, cv::Rect(tx*4, ty*4, 4, 4), color, cv::FILLED);
        }
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

        html << "<h2>Match Quality</h2><table>\n";
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
            int ti = worstIdx[k].second;
            int tx = ti % tilesX, ty = ti / tilesX;
            const char* cn = (ti < (int)analyzeCat.size() && analyzeCat[ti]==0)?"S":
                             (ti < (int)analyzeCat.size() && analyzeCat[ti]==1)?"E":
                             (ti < (int)analyzeCat.size() && analyzeCat[ti]==2)?"T":"N";
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
        db.recordRunUsage(useCount, targetHash, targetPath);
}

// ============================================================
// , ,
// ============================================================
bool MosaicEngine::generate(const std::string& targetPath,
                             const std::string& dbPath,
                             const std::string& outputPath,
                             const Config& config)
{
    // , , �? �?CUDA, ,  GPU , 默锟剿伙�?
    Config cfg = config;
    if (cfg.useGpu && !cuda::isCudaAvailable())
    {
        cfg.useGpu = false;
    }

    // Benchmark , �?
    using Clock = std::chrono::steady_clock;
    using Ms = std::chrono::duration<double, std::milli>;
    auto tStart = Clock::now();
    auto tLast  = tStart;
    double msFeat = 0, msANNBuild = 0, msGPUScore = 0, msSelect = 0, msPlace = 0;
    double msPrep = 0;  // DB, ,  + GPU library, , , , GPU�? ,

    // , , , �? , ,  profile, , 锟�??精锟饺�??拷原, 锟�??硷拷, ,
    std::atomic<int64_t> opResizeNs{0};
    std::atomic<int64_t> opLabNs{0};
    std::atomic<int64_t> opGridNs{0};
    std::atomic<int64_t> opTinyNs{0};
    std::atomic<int64_t> opEdgeNs{0};
    std::atomic<int64_t> opLbpNs{0};

    // Placement 锟阶讹拷 profile
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

    // , , �? �? 锟捷癸拷�? , 锟截诧拷, , , , 锟节革拷, 去锟�??���?
    std::string targetHash;
    {
        // �?100 , 锟截诧拷,  1 , , , 锟饺?10000 , ,  ,  3 �?  ,  30KB
        int64_t totalPixels = static_cast<int64_t>(target.rows) * target.cols;
        int step = std::max(1LL, totalPixels / 10000);
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

    // �? , , 叽锟绞憋�? , , 锟侥匡拷锟酵硷拷, , 谋锟?tile , , , , �?tile �? �? 锟�??憋拷锟绞ｏ拷
    if (cfg.outW > 0 && cfg.outH > 0)
    {
        cv::Mat resized;
        cv::resize(target, resized, cv::Size(cfg.outW, cfg.outH), 0, 0, cv::INTER_AREA);
        target = resized;
        std::cout << "Target resized to: " << cfg.outW << "x" << cfg.outH << std::endl;
    }

    // --upscale, 锟脚达拷原图, �? ,  tile, �? , �? �? , , 芏龋�?
    if (cfg.upscale > 1)
    {
        cv::Mat up;
        cv::resize(target, up, cv::Size(target.cols * cfg.upscale, target.rows * cfg.upscale),
                   0, 0, cv::INTER_LINEAR);
        target = up;
        std::cout << "Target upscaled " << cfg.upscale << "x: "
                  << target.cols << "x" << target.rows << std::endl;
    }

    Database db(dbPath);
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

    // Read feature resolution from DB meta (required; old DBs must be rebuilt) , 锟捷旧库�?
    std::string fw = db.getMeta("feature_w");
    std::string fh = db.getMeta("feature_h");
    if (fw.empty() || fh.empty())
    {
        std::cerr << "ERROR: Database missing feature resolution metadata." << std::endl;
        std::cerr << "  This database was built with an older version of Mosaicraft." << std::endl;
        std::cerr << "  Please rebuild with: mosaicraft build -i <photos> -d <db>" << std::endl;
        return false;
    }
    int featW = std::atoi(fw.c_str());
    int featH = std::atoi(fh.c_str());

    int featPixels = featW * featH;
    int featBytes = featPixels * 3;
    std::cout << "  (feature space: " << featW << "x" << featH << ")" << std::endl;

    // �??姩鎺�??杈撳�?tile锛氭í�?�?320�?80锛岀�??��?鏂瑰�?�?180�?20
    if (cfg.nativeTileW == 180 && cfg.nativeTileH == 320)
    {
        if (featW > featH)  // �??�?
        {
            cfg.nativeTileW = 320;
            cfg.nativeTileH = 180;
            std::cout << "  (auto output tile: 320x180, DB is landscape)" << std::endl;
        }
    }


    // , �? , �??��, ,  FeaturePack / ANN 锟�??久伙拷使锟矫ｏ拷
    std::string featDirCache;
    auto allRecords = db.allRecords();  // �? 锟铰硷拷, �?GPU �? 锟叫�??��, , �?
    dbCount = static_cast<int>(allRecords.size());

    // , , , , �? �? , �??��
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

    // , , ,  �? , , , �?GPU, , 锟�??程�??拷锟�??���?  tiny/LBP 锟侥硷拷,  , , ,
    cuda::GpuLibrary gpuLib;
    auto releaseGpuLib = [&]() {
        if (gpuLib.count > 0) cuda::freeLibrary(gpuLib);
    };
    if (cfg.useGpu && cuda::isCudaAvailable())
    {
        std::vector<double>  h_lab(dbCount * 3);
        std::vector<float>   h_grid(dbCount * 192);
        std::vector<uint8_t> h_tiny(dbCount * 256);
        std::vector<double>  h_edge(dbCount);
        std::vector<float>   h_lbp(dbCount * 256);
        std::vector<int>     h_use(dbCount);

        // , , , , , , , , �?I/O, , 锟�??程硷拷锟缴ｏ�?
        for (int i = 0; i < dbCount; ++i)
        {
            const auto& rec = allRecords[i];
            h_lab[i*3+0] = rec.avgL; h_lab[i*3+1] = rec.avgA; h_lab[i*3+2] = rec.avgB;
            if (rec.grid4x4.size() == 192)
                std::memcpy(&h_grid[i*192], rec.grid4x4.data(), 192*sizeof(float));
            h_edge[i] = rec.edgeDensity;
            h_use[i] = rec.useCount;
        }

        // , 锟皆硷拷锟截讹拷, , , , , 锟芥（tiny.bin + lbp.bin,
        // , , , 效时,  2 ,  fread , �?50K , 锟侥硷拷 I/O
        bool cacheLoaded = false;
        if (!allRecords.empty() && !allRecords[0].tinyPath.empty())
        {
            std::vector<int> recordIds;
            recordIds.reserve(dbCount);
            for (const auto& r : allRecords)
                recordIds.push_back(r.id);
            cacheLoaded = FeaturePack::tryLoad(featDirCache, recordIds, h_tiny, h_lbp);
        }

        if (!cacheLoaded)
        {
            // , 锟芥�? 锟节伙拷失效 ,  , 锟剿碉拷, 锟�??筹拷, 锟侥硷拷, �?
            std::cout << "  (feature cache miss, reading individual files)" << std::endl;
            int nUploadThreads = std::thread::hardware_concurrency();
            if (nUploadThreads < 2) nUploadThreads = 2;
            if (nUploadThreads > 16) nUploadThreads = 16;  // , ,  I/O 锟�??程癸拷锟洁反, 锟剿伙拷
            std::vector<std::thread> uploadWorkers;
            for (int t = 0; t < nUploadThreads; ++t)
            {
                uploadWorkers.emplace_back([&, t]() {
                    FeatureCache cache;  // �? 锟�??程�??�? , 锟芥�? , , �?
                    for (int i = t; i < dbCount; i += nUploadThreads)
                    {
                        const auto& rec = allRecords[i];
                        if (!rec.tinyPath.empty())
                        {
                            auto* td = cache.loadTiny(rec.id, rec.tinyPath);
                            if (td) std::memcpy(&h_tiny[i*256], td->data(), 256);
                        }
                        if (!rec.histPath.empty())
                        {
                            auto* ld = cache.loadLBP(rec.id, rec.histPath);
                            if (ld) std::memcpy(&h_lbp[i*256], ld->data(), 256*sizeof(float));
                        }
                    }
                });
            }
            for (auto& w : uploadWorkers) w.join();

            // , , , 桑锟剿�??�? , , , , �? �??拷麓, , , , , 锟叫ｏ拷
            if (!featDirCache.empty())
                FeaturePack::buildCache(featDirCache, allRecords);
        }
        if (cuda::uploadLibrary(gpuLib, h_lab.data(), h_grid.data(),
                                 h_tiny.data(), h_edge.data(),
                                 h_lbp.data(), h_use.data(), dbCount))
        {
            std::cout << "GPU library: " << dbCount << " images ("
                      << (dbCount * (192*4+256+256*4) / 1024) << " KB)" << std::endl;
        }
        else
        {
            cfg.useGpu = false;
        }
    }

    // , , �? ,  tile, , �? , , 洳谷?
    int tilesX = (target.cols + cfg.tileW - 1) / cfg.tileW;
    int tilesY = (target.rows + cfg.tileH - 1) / cfg.tileH;

    // Read feature resolution from DB meta (required; old DBs must be rebuilt),
    // , 图模式锟铰�??�?65500px , �?
    int outTileW = cfg.nativeTileW;
    int outTileH = cfg.nativeTileH;
    const int MAX_DIM = 65500;
    if (!cfg.tiledOutput && cfg.outputFormat != "png"
        && (tilesX * outTileW > MAX_DIM || tilesY * outTileH > MAX_DIM))
    {
        if (cfg.outputFormat == "jpg" && cfg.formatExplicit)
        {
            // , 式指,  jpg , ,  ,  锟饺憋拷, , , �?tile , , �? �?
            double scaleW = (tilesX * outTileW > MAX_DIM) ? static_cast<double>(MAX_DIM) / (tilesX * outTileW) : 1.0;
            double scaleH = (tilesY * outTileH > MAX_DIM) ? static_cast<double>(MAX_DIM) / (tilesY * outTileH) : 1.0;
            double scale = std::min(scaleW, scaleH);
            outTileW = std::max(1, static_cast<int>(outTileW * scale));
            outTileH = std::max(1, static_cast<int>(outTileH * scale));
            std::cout << "  (auto-scaled tile " << outTileW << "x" << outTileH
                      << " to fit JPEG 65500px limit)" << std::endl;
        }
        else if (cfg.outputFormat == "jpg")
        {
            // �? 式指, , �? �?  jpg , ,  ,  锟皆讹拷,  tiff
            cfg.outputFormat = "tiff";
            std::cout << "  (auto-switched to TIFF: output exceeds JPEG 65500px limit)" << std::endl;
        }
        else if (cfg.outputFormat != "tiff" && cfg.outputFormat != "webp")
        {
            // , , , �? ,  ,  锟皆讹拷,  tiled
            cfg.tiledOutput = true;
            std::cout << "  (auto-switched to tiled: output exceeds 65500px encoder limit)" << std::endl;
        }
    }

    // WebP , ,  16383px ,  锟饺憋拷, 锟脚ｏ拷, , �?JPG 锟�??硷拷丢�,
    const int WEBP_MAX = 16383;
    if (!cfg.tiledOutput && cfg.outputFormat == "webp"
        && (tilesX * outTileW > WEBP_MAX || tilesY * outTileH > WEBP_MAX))
    {
        double scaleW = (tilesX * outTileW > WEBP_MAX) ? static_cast<double>(WEBP_MAX) / (tilesX * outTileW) : 1.0;
        double scaleH = (tilesY * outTileH > WEBP_MAX) ? static_cast<double>(WEBP_MAX) / (tilesY * outTileH) : 1.0;
        double scale = std::min(scaleW, scaleH);
        outTileW = std::max(1, static_cast<int>(outTileW * scale));
        outTileH = std::max(1, static_cast<int>(outTileH * scale));
        std::cout << "  (auto-scaled tile " << outTileW << "x" << outTileH
                  << " to fit WebP 16383px limit)" << std::endl;
    }

    // 溢出棢�查：大型�??��克可能超�?int32 范围
    int64_t outW64 = static_cast<int64_t>(tilesX) * outTileW;
    int64_t outH64 = static_cast<int64_t>(tilesY) * outTileH;
    if (outW64 > INT_MAX || outH64 > INT_MAX) {
        std::cerr << "ERROR: Output too large (" << outW64 << "x" << outH64 << ")" << std::endl;
        releaseGpuLib();
        return false;
    }
    int outW = static_cast<int>(outW64);
    int outH = static_cast<int>(outH64);

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

    // , �? �? , �?  tile 锟�??达拷,  pad, , , , �?tile 锟�??寸）
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
              << " = " << (tilesX * tilesY)
              << "  (output " << outW << "x" << outH
              << ", tile " << outTileW << "x" << outTileH;
    if (padRight > 0 || padBottom > 0)
    {
        std::cout << ", padded +" << padRight << "x" << padBottom;
    }
    std::cout << ")" << std::endl;
    // , , 锟皆憋拷
    double srcRatio = static_cast<double>(target.cols) / target.rows;
    double outRatio = static_cast<double>(outW) / outH;
    std::cout << "  Aspect: src=" << std::fixed << std::setprecision(3) << srcRatio
              << " out=" << outRatio << " (diff=" << std::abs(srcRatio - outRatio) << ")"
              << std::endl;

    // , , ,  , 锟�??筹拷�? , , ,  tile , ,  , , ,
    int totalTiles = tilesX * tilesY;

    // --analyze: �? , , , , , , , ,
    std::vector<double> analyzeScores;
    std::vector<int>    analyzeImageIds;
    std::vector<double> analyzeLabD, analyzeGridD, analyzeEdgeD;
    std::vector<double> analyzeGaps;      // winner-runnerUp , , ,
    std::vector<int>    analyzeRanks;     // winner 锟节猴拷�? , 锟叫碉拷�? (1-based)
    std::vector<int>    analyzeAnnRanks;  // winner ,  ANN Top200 锟叫碉拷�? (0=, , )
    std::vector<int>    analyzeCat;       // 0=Smooth, 1=Edge, 2=Texture, 3=Normal
    double analyzeGridCellSum[64] = {0};   // �?  cell 锟侥撅拷, 锟�??计ｏ�? 锟节癸拷锟阶凤拷, ,
    FeatureCache analysisFeatureCache;

    int N = cfg.candidates;  // , �? , GPU �? , , ,  benchmark,

    // Benchmark , ,  lambda, 锟节斤拷, �? 锟矫ｏ拷, , ,  totalTiles/N �? 锟藉�?
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
            std::cout << "  ANN (bld+q): " << msANNBuild  << " ms\n";
        if (msGPUScore > 0)
        {
            std::cout << "  GPU scoring: " << msGPUScore  << " ms\n";
            std::cout << "  GPU speed:   " << std::fixed << std::setprecision(0)
                      << (totalTiles * static_cast<double>(N) / msGPUScore * 1000.0)
                      << " scores/s\n";
        }
        std::cout << "  Selection:   " << msSelect    << " ms\n";
        std::cout << "  Placement:   " << msPlace     << " ms\n";
        if (opPlaceDecodeNs > 0)
        {
            auto toMs = [](int64_t ns) { return ns / 1000000.0; };
            std::cout << "    Decode:   " << std::setprecision(1) << toMs(opPlaceDecodeNs) << " ms\n";
            std::cout << "    Resize:   " << std::setprecision(1) << toMs(opPlaceResizeNs) << " ms\n";
            std::cout << "    Copy:     " << std::setprecision(1) << toMs(opPlaceCopyNs) << " ms\n";
        }
        std::cout << "  === Total: " << msTotal     << " ms ===\n";
        if (totalTiles > 0)
            std::cout << "  Avg/tile:    " << (msTotal / totalTiles) << " ms\n";
        std::cout << std::flush;
    };

    std::vector<double> allTL(totalTiles), allTA(totalTiles), allTB(totalTiles);
    std::vector<std::vector<float>> allGrid(totalTiles);
    std::vector<std::vector<uint8_t>> allTiny(totalTiles);
    std::vector<double> allEdge(totalTiles);
    std::vector<std::vector<float>> allLBP(totalTiles);

    // Phase D , , ,  4, 4 锟皆憋拷锟矫ｏ拷8, 8, 4, 4,

    int nThreads = std::thread::hardware_concurrency();
    if (nThreads < 2) nThreads = 2;

    // �? 锟阶段硷拷时, DB, 锟截★拷GPU library, , , 锟剿斤拷,
    auto tPreFeat = Clock::now();
    msPrep = Ms(tPreFeat - tLast).count();
    tLast = tPreFeat;

    // Phase 0: , , , �? GPU , , , 锟劫ｏ拷CPU , 锟剿ｏ拷
    if (cfg.useGpu)
    {
        const int BATCH = 256;
        std::vector<uint8_t> batchFeat(BATCH * featBytes);
        std::vector<double> batchLAB(BATCH * 3);
        std::vector<float>  batchGrid(BATCH * 192);
        std::vector<uint8_t> batchTiny(BATCH * 256);
        std::vector<double> batchEdgeArr(BATCH);
        std::vector<float>  batchLBP(BATCH * 256);

        int batchStart = 0;
        for (; batchStart + BATCH <= totalTiles; batchStart += BATCH)
        {
            int batchN = BATCH;

            // CPU resize: tile ,  featW, featH, , 锟�??程ｏ�?
            #pragma omp parallel for
            for (int i = 0; i < batchN; ++i)
            {
                int ti = batchStart + i;
                int ty = ti / tilesX, tx = ti % tilesX;
                cv::Mat roi = target(cv::Rect(tx*cfg.tileW, ty*cfg.tileH, cfg.tileW, cfg.tileH));
                cv::Mat roiFeat;
                cv::resize(roi, roiFeat, cv::Size(featW, featH), 0, 0, cv::INTER_LINEAR);
                std::memcpy(&batchFeat[i * featBytes], roiFeat.data, featBytes);
            }

            // GPU , , , �? ,
            int ret = mosaicraft::cuda::extractFeaturesRaw(
                batchFeat.data(), batchN, featW, featH,
                batchLAB.data(), batchGrid.data(), batchTiny.data(),
                batchEdgeArr.data(), batchLBP.data());
            if (ret < 0) { cfg.useGpu = false; break; }

            // 锟截讹拷, �?
            for (int i = 0; i < batchN; ++i)
            {
                int ti = batchStart + i;
                allTL[ti]  = batchLAB[i * 3 + 0];
                allTA[ti]  = batchLAB[i * 3 + 1];
                allTB[ti]  = batchLAB[i * 3 + 2];
                allGrid[ti].assign(&batchGrid[i * 192], &batchGrid[(i + 1) * 192]);
                allTiny[ti].assign(&batchTiny[i * 256], &batchTiny[(i + 1) * 256]);
                allEdge[ti] = batchEdgeArr[i];
                allLBP[ti].assign(&batchLBP[i * 256], &batchLBP[(i + 1) * 256]);
            }
            int done = batchStart + batchN;
            double elapsed = std::chrono::duration<double>(Clock::now() - tPreFeat).count();
            double eta = (elapsed / done) * (totalTiles - done);
            std::string etaStr = (eta < 1.0) ? "<1s" : (std::to_string(static_cast<int>(eta)) + "s");
            std::cout << "\r  features " << done << "/" << totalTiles
                      << " | ETA " << etaStr << "   " << std::flush;  // β���ո񸲸Ǿ��в���
        }

        // 剩锟洁不,  256 , �?
        if (batchStart < totalTiles)
        {
            int tailN = totalTiles - batchStart;
            std::vector<uint8_t> tailFeat(tailN * featBytes);
            std::vector<double> tailLAB(tailN * 3);
            std::vector<float>  tailGrid(tailN * 192);
            std::vector<uint8_t> tailTiny(tailN * 256);
            std::vector<double> tailEdgeArr(tailN);
            std::vector<float>  tailLBP(tailN * 256);

            #pragma omp parallel for
            for (int i = 0; i < tailN; ++i)
            {
                int ti = batchStart + i;
                int ty = ti / tilesX, tx = ti % tilesX;
                cv::Mat roi = target(cv::Rect(tx*cfg.tileW, ty*cfg.tileH, cfg.tileW, cfg.tileH));
                cv::Mat roiFeat;
                cv::resize(roi, roiFeat, cv::Size(featW, featH), 0, 0, cv::INTER_LINEAR);
                std::memcpy(&tailFeat[i * featBytes], roiFeat.data, featBytes);
            }

            int ret = mosaicraft::cuda::extractFeaturesRaw(
                tailFeat.data(), tailN, featW, featH,
                tailLAB.data(), tailGrid.data(), tailTiny.data(),
                tailEdgeArr.data(), tailLBP.data());
            if (ret < 0) { cfg.useGpu = false; }

            if (cfg.useGpu)
            {
                for (int i = 0; i < tailN; ++i)
                {
                    int ti = batchStart + i;
                    allTL[ti]  = tailLAB[i * 3 + 0];
                    allTA[ti]  = tailLAB[i * 3 + 1];
                    allTB[ti]  = tailLAB[i * 3 + 2];
                    allGrid[ti].assign(&tailGrid[i * 192], &tailGrid[(i + 1) * 192]);
                    allTiny[ti].assign(&tailTiny[i * 256], &tailTiny[(i + 1) * 256]);
                    allEdge[ti] = tailEdgeArr[i];
                    allLBP[ti].assign(&tailLBP[i * 256], &tailLBP[(i + 1) * 256]);
                }
            }
            std::cout << "\r  features " << totalTiles << "/" << totalTiles << std::endl;
        }
        else
        {
            std::cout << std::endl;
        }
    }

    if (!cfg.useGpu)  // CPU , 锟剿ｏ拷, ,  16 锟�??筹拷, �?
    {
        std::atomic<int> featDone{0};
        std::vector<std::thread> featWorkers;
        for (int t = 0; t < nThreads; ++t) {
            featWorkers.emplace_back([&, t]() {
                using Ns = std::chrono::nanoseconds;
                for (int idx = t; idx < totalTiles; idx += nThreads) {
                    int ty = idx / tilesX, tx = idx % tilesX;
                    cv::Mat roi = target(cv::Rect(tx*cfg.tileW, ty*cfg.tileH, cfg.tileW, cfg.tileH));
                    cv::Mat roiNative;
                    auto t0 = Clock::now();
                    cv::resize(roi, roiNative, cv::Size(featW, featH), 0, 0, cv::INTER_LINEAR);
                    auto t1 = Clock::now(); opResizeNs += std::chrono::duration_cast<Ns>(t1 - t0).count();
                    cv::Mat lab; cv::cvtColor(roiNative, lab, cv::COLOR_BGR2Lab);
                    cv::Scalar m = cv::mean(lab);
                    allTL[idx]=m[0]; allTA[idx]=m[1]; allTB[idx]=m[2];
                    auto t2 = Clock::now(); opLabNs += std::chrono::duration_cast<Ns>(t2 - t1).count();
                    allGrid[idx] = computeGrid8x8(roiNative);
                    auto t3 = Clock::now(); opGridNs += std::chrono::duration_cast<Ns>(t3 - t2).count();
                    allTiny[idx] = computeTinyImage(roiNative);
                    auto t4 = Clock::now(); opTinyNs += std::chrono::duration_cast<Ns>(t4 - t3).count();
                    allEdge[idx] = computeEdgeDensity(roiNative);
                    auto t5 = Clock::now(); opEdgeNs += std::chrono::duration_cast<Ns>(t5 - t4).count();
                    allLBP[idx] = computeLBPHistogram(roiNative);
                    auto t6 = Clock::now(); opLbpNs += std::chrono::duration_cast<Ns>(t6 - t5).count();
                    int d = ++featDone;
                    if (d % 500 == 0 || d == totalTiles)
                        std::cout << "\r  features " << d << "/" << totalTiles << std::flush;
                }
            });
        }
        for (auto& w : featWorkers) w.join();
        std::cout << std::endl;
    }

    auto tFeat = Clock::now();
    msFeat = Ms(tFeat - tLast).count();
    tLast = tFeat;

    std::atomic<int> matched{0};
    int loadFail = 0;
    int cntGrid = 0, cntMissGrid = 0;
    int cntTiny = 0, cntMissTiny = 0;
    int cntEdge = 0, cntMissEdge = 0;
    int cntSmoothCat = 0, cntEdgeCat = 0, cntTextureCat = 0, cntNormalCat = 0;
    double smoothSum = 0, edgeSum = 0, textureSum = 0, normalSum = 0;
    int cntLBP  = 0, cntMissLBP  = 0;

    // , 锟津窗匡拷锟皆�??�? , 锟劫革拷,  2 ,  tile, , �? 锟津）猴拷默,  300, 水平, ,
    auto autoNeighborWindow = [&]() {
        int base = std::max(300, tilesX * 2);
        int dynamic = static_cast<int>(std::sqrt(static_cast<double>(allRecords.size())) * 1.5);
        return std::max(base, std::min(dynamic, 400));
    };

    if (cfg.neighborWindow <= 0)
    {
        // , , , , ,  2 ,  tile
        // , �? , , �? , , , O(, N), , , 飧�??拷歉, , �?
        cfg.neighborWindow = autoNeighborWindow();
        // 46K, 323, 200K, 400(cap), sweep: 300-400, ,
    }

    // , , , ,  + 频锟绞硷�? , , , , , , 锟矫碉拷, �? ,
    std::deque<int> recentIds;
    std::unordered_map<int, int> freqInWindow;
    // 强锟狡硷�? 同一图片, 锟劫硷拷�?minGap ,  tile , , 锟劫达拷�?
    const int MIN_GAP = std::max(50, tilesX);  // , , 丢�,
    std::unordered_map<int, int> lastUsedAt;   // imageId ,  , 锟绞癸拷�??��?tile , �?
    std::deque<std::vector<float>> recentGrids;  // , 锟酵硷拷锟解�? , , , �?00, ,
    constexpr double GRID_DUP_THRESHOLD = 0.010;  // , 锟较格：革拷小锟侥撅拷锟�??��, 为锟�??���?
    constexpr double GRID_DUP_PENALTY = 200.0;     // , 锟酵硷拷�? , 锟叫??癸拷�?00,
    constexpr int GRID_DUP_WINDOW = 50;            // 锟教讹拷, 锟节ｏ拷, , , 锟脚ｏ拷, , 丢�, ,  tile , , ,

    // 权锟�??��拷一, , , ,  tile , 锟矫ｏ拷
    double wSum = cfg.labWeight + cfg.gridWeight + cfg.tinyWeight;
    if (cfg.edgeWeight > 0) wSum += cfg.edgeWeight;
    if (cfg.lbpWeight > 0)  wSum += cfg.lbpWeight;
    double nLabW = cfg.labWeight / wSum;
    double nGridW = cfg.gridWeight / wSum;
    double nTinyW = cfg.tinyWeight / wSum;
    double nEdgeW = cfg.edgeWeight / wSum;
    double nLbpW  = cfg.lbpWeight / wSum;
    N = cfg.candidates;

    // �?  tile , , 选锟�??��硷拷�? GPU �? 预锟芥，CPU �? , , , , �?
    std::vector<ImageRecord> bestRecords(totalTiles);
    std::vector<int> bestLibIdx(totalTiles, -1);

    // , �? 锟绞憋拷拇锟?Mat, 锟�??匡拷模式, , �? , , 锟节达拷锟皆憋拷, �??��锟矫ｏ拷
    cv::Mat output;

    auto runAnalysis = [&](const std::string& analysisOutputPath) {
        writeAnalysisReport(AnalysisReportContext{
            cfg, db, analysisOutputPath, targetPath, targetHash, target,
            totalTiles, tilesX, tilesY, outTileW, outTileH, featW, featH,
            dbCount, matched.load(), analyzeScores, analyzeImageIds,
            analyzeLabD, analyzeGridD, analyzeEdgeD, analyzeGaps,
            analyzeRanks, analyzeAnnRanks, analyzeCat, analyzeGridCellSum,
            allTL, allTA, allTB, allEdge, allGrid, allRecords, bestRecords
        });
    };

    if (cfg.useGpu && gpuLib.count > 0)
    {
        // 锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋
        // GPU , , , 水锟�??��拷SQLite �?  ,  丢�,  GPU ,  �? �?  ,  , 锟�??筹拷, �?
        // 锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋

        // , ,  Phase A: ANN , , , , , , �?, ,
        // , 锟饺硷拷锟截持久伙拷, , , build �? 锟芥�? , , , 锟津构斤�? , ,
        FeatureIndex annIndex;
        std::string annPath = featDirCache.empty() ? "lib.ann"
                             : (featDirCache + "/lib.ann");
        bool annLoaded = false;
        if (!featDirCache.empty())
        {
            std::cout << "  loading ANN index..." << std::flush;
            annLoaded = annIndex.load(annPath, 708, allRecords);
            std::cout << (annLoaded ? " done" : " not found") << std::endl;
        }
        if (!annLoaded)
        {
            std::cout << "  building ANN index (" << dbCount << " images)..." << std::flush;
            annIndex.build(allRecords);
            std::cout << " done" << std::endl;
            if (!featDirCache.empty())
            {
                if (annIndex.save(annPath))
                    std::cout << "  ANN index saved: " << annPath << std::endl;
            }
        }

        std::cout << "  collecting candidates..." << std::flush;
        std::vector<int> allIndices(totalTiles * N, -1);
        std::vector<float> tileVec;
        int annMissCount = 0;
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            buildTileVector(allTL[ti], allTA[ti], allTB[ti],
                            allGrid[ti], allTiny[ti], allEdge[ti], allLBP[ti],
                            tileVec);
            auto imgIds = annIndex.query(tileVec.data(), N);
            int nc = static_cast<int>(imgIds.size());
            for (int j = 0; j < nc; ++j)
            {
                int libIdx = annIndex.idToAllRecordsIndex(imgIds[j]);
                if (libIdx >= 0)
                    allIndices[ti * N + j] = libIdx;
                else
                    annMissCount++;
            }
            if (ti % 5000 == 0 || ti == totalTiles - 1)
                std::cout << "\r  collecting candidates " << (ti+1) << "/" << totalTiles << std::flush;
        }
        std::cout << " done" << std::endl;

        // Phase A , �? ANN , ,  + , �?
        auto tANN = Clock::now();
        msANNBuild = Ms(tANN - tLast).count();
        tLast = tANN;

        // , ,  Phase B: , �?  tile , , , GPU , �? , 锟节存布锟�??ｏ拷 , ,
        std::vector<float>   flatGrid(totalTiles * 192);
        std::vector<uint8_t> flatTiny(totalTiles * 256);
        std::vector<float>   flatLBP(totalTiles * 256);
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            std::memcpy(&flatGrid[ti * 192], allGrid[ti].data(), 192 * sizeof(float));
            std::memcpy(&flatTiny[ti * 256], allTiny[ti].data(), 256);
            std::memcpy(&flatLBP[ti * 256], allLBP[ti].data(), 256 * sizeof(float));
        }

        // , ,  Phase C: , ,  GPU , ,  , ,
        // , , 应权锟截ｏ拷, ,  tile , , �? , , 预锟借（�? �?  --adaptive-weights,
        std::vector<double> tileLabW(totalTiles, nLabW);
        std::vector<double> tileGridW(totalTiles, nGridW);
        std::vector<double> tileTinyW(totalTiles, nTinyW);
        std::vector<double> tileEdgeW(totalTiles, nEdgeW);
        std::vector<double> tileLbpW(totalTiles, nLbpW);
        int cntSmooth = 0, cntEdge = 0, cntTexture = 0, cntNormal = 0;
        if (cfg.adaptiveWeights)
        {
            for (int ti = 0; ti < totalTiles; ++ti)
            {
                double e = allEdge[ti];
                double lbpEnt = 0.0;
                for (int k = 0; k < 256; ++k)
                {
                    float v = allLBP[ti][k];
                    if (v > 0.0f) lbpEnt -= v * std::log2(v);
                }
                double lSum = 0, lSq = 0;
                for (int k = 0; k < 64; ++k)
                {
                    double lv = allGrid[ti][k * 3];
                    lSum += lv; lSq += lv * lv;
                }
                double lVar = lSq / 64.0 - (lSum / 64.0) * (lSum / 64.0);

                if (e < 0.005 && lVar < 100.0)
                {
                    // Smooth: ,  LAB , �? , ,  Grid, , �? , 锟揭??拷占锟结�?
                    tileLabW[ti] = 0.25;
                    tileGridW[ti] = 0.45;
                    tileTinyW[ti] = 0.20;
                    tileEdgeW[ti] = 0.05;
                    tileLbpW[ti] = 0.05;
                    cntSmooth++;
                }
                else if (e > 0.01)
                {
                    // Edge-heavy: , , 锟结�?> , �? , �?0.01 , ,  9, 16 �?ROI,
                    tileLabW[ti] = 0.15;
                    tileGridW[ti] = 0.40;
                    tileTinyW[ti] = 0.25;
                    tileEdgeW[ti] = 0.15;
                    tileLbpW[ti] = 0.05;
                    cntEdge++;
                }
                else if (lbpEnt > 3.0)
                {
                    // Texture-heavy: , ,  > , �?
                    tileLabW[ti] = 0.15;
                    tileGridW[ti] = 0.40;
                    tileTinyW[ti] = 0.20;
                    tileEdgeW[ti] = 0.05;
                    tileLbpW[ti] = 0.20;
                    cntTexture++;
                }
                else
                {
                    cntNormal++;
                }
            }
        }

        std::cout << "  GPU scoring " << totalTiles << " x " << N;
        if (cfg.adaptiveWeights)
        {
            // 锟秸硷拷锟�??诧拷�? , 校准, �?
            std::vector<double> edgeVals(totalTiles), lbpVals(totalTiles);
            for (int ti = 0; ti < totalTiles; ++ti)
            {
                edgeVals[ti] = allEdge[ti];
                double ent = 0.0;
                for (int k = 0; k < 256; ++k)
                {
                    float v = allLBP[ti][k];
                    if (v > 0.0f) ent -= v * std::log2(v);
                }
                lbpVals[ti] = ent;
            }
            std::sort(edgeVals.begin(), edgeVals.end());
            std::sort(lbpVals.begin(), lbpVals.end());
            auto pct = [&](const auto& v, double p) { return v[static_cast<size_t>(p * v.size())]; };
            std::cout << "\n  Edge:  P50=" << std::fixed << std::setprecision(3) << pct(edgeVals, 0.50)
                      << " P90=" << pct(edgeVals, 0.90) << " P95=" << pct(edgeVals, 0.95)
                      << " P99=" << pct(edgeVals, 0.99);
            std::cout << "\n  LBP:   P50=" << std::setprecision(2) << pct(lbpVals, 0.50)
                      << " P90=" << pct(lbpVals, 0.90) << " P95=" << pct(lbpVals, 0.95)
                      << " P99=" << pct(lbpVals, 0.99);
            std::cout << "\n  Class: S=" << cntSmooth << " E=" << cntEdge
                      << " T=" << cntTexture << " N=" << cntNormal;
        }
        std::cout << "..." << std::flush;
        std::vector<double> allScores(totalTiles * N, 1e30);
        cuda::scoreBatch(
            totalTiles,
            allTL.data(), allTA.data(), allTB.data(),
            flatGrid.data(), flatTiny.data(), allEdge.data(), flatLBP.data(),
            allIndices.data(), N,
            gpuLib,
            tileLabW.data(), tileGridW.data(), tileTinyW.data(), tileEdgeW.data(), tileLbpW.data(),
            cfg.usePenalty,
            allScores.data());
        std::cout << " done" << std::endl;

        // Phase C , �?
        auto tGPU = Clock::now();
        msGPUScore = Ms(tGPU - tLast).count();
        tLast = tGPU;

        // , ,  Phase D: �? �?  + , , �?  , ,
        // 8, 8 vs 4, 4 锟皆比ｏ�?  --analyze �? ,
        std::vector<std::vector<float>> libGrid4x4, tileGrid4x4;
        if (cfg.analyze)
        {
            libGrid4x4.resize(dbCount);
            tileGrid4x4.resize(totalTiles);
            // �? , 锟酵?4, 4
        for (int i = 0; i < dbCount; ++i)
        {
            const auto& g8 = allRecords[i].grid4x4;
            libGrid4x4[i].resize(48);
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        float sum = 0;
                        for (int dr = 0; dr < 2; ++dr)
                            for (int dc = 0; dc < 2; ++dc)
                                sum += g8[((r*2+dr)*8 + (c*2+dc)) * 3 + ch];
                        libGrid4x4[i][(r*4+c)*3 + ch] = sum / 4.0f;
                    }
                }
            }
        }
        // �? ,  tile 4, 4
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            const auto& g8 = allGrid[ti];
            tileGrid4x4[ti].resize(48);
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        float sum = 0;
                        for (int dr = 0; dr < 2; ++dr)
                            for (int dc = 0; dc < 2; ++dc)
                                sum += g8[((r*2+dr)*8 + (c*2+dc)) * 3 + ch];
                        tileGrid4x4[ti][(r*4+c)*3 + ch] = sum / 4.0f;
                    }
                }
            }
        }
        } // if (cfg.analyze) ,  �? , , �?

        int grid4Top1 = 0, grid8Top1 = 0, top1Differ = 0;

        if (cfg.analyze)
        {
            analyzeScores.reserve(totalTiles);
            analyzeImageIds.reserve(totalTiles);
            analyzeLabD.reserve(totalTiles);
            analyzeGridD.reserve(totalTiles);
            analyzeEdgeD.reserve(totalTiles);
            analyzeGaps.reserve(totalTiles);
            analyzeRanks.reserve(totalTiles);
            analyzeAnnRanks.reserve(totalTiles);
            analyzeCat.reserve(totalTiles);
        }

        std::cout << "  selecting best..." << std::flush;
        int noCandidateCount = 0;  // , 希锟酵�??�? 藓锟窖★拷锟?tile
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            double* scores = &allScores[ti * N];
            const int* indices = &allIndices[ti * N];
            // �? , �? �? , 锟脚筹拷 -1 , �?
            int validCount = 0;
            for (int j = 0; j < N; ++j)
                if (indices[j] >= 0) validCount++;
            if (validCount == 0)
            {
                noCandidateCount++;
                continue;
            }
            // 频锟绞分硷拷锟酵凤拷, 1, 锟结�?, , , �?, 2, 锟叫凤拷, 3+, 锟截凤拷(, , , )
            for (int j = 0; j < N; ++j)
            {
                int libIdx = indices[j];
                if (libIdx < 0 || libIdx >= static_cast<int>(allRecords.size())) continue;
                int imgId = allRecords[libIdx].id;
                auto it = freqInWindow.find(imgId);
                int cnt = (it != freqInWindow.end()) ? it->second : 0;
                if (cnt >= 3)      { scores[j] += cfg.neighborPenalty; }
                else if (cnt == 2) { scores[j] += cfg.neighborPenalty * 0.4; }
                else if (cnt == 1) { scores[j] += cfg.neighborPenalty * 0.1; }
                // 强锟狡硷�? 同一图片,  MIN_GAP , 锟截革拷 ,  ,  500, �? , 锟酵硷拷�? �?
                auto gapIt = lastUsedAt.find(imgId);
                if (gapIt != lastUsedAt.end() && (ti - gapIt->second) < MIN_GAP)
                {
                    scores[j] += 500.0;
                }
                // , 锟酵硷拷锟解�? �? , �?tile ,  Grid , �?,  锟接凤拷
                const auto& candGrid = allRecords[indices[j]].grid4x4;
                for (const auto& rg : recentGrids)
                {
                    if (gridDistance8x8(candGrid, rg) < GRID_DUP_THRESHOLD)
                    {
                        scores[j] += GRID_DUP_PENALTY;
                        break;
                    }
                }
            }
            // Top-N , 锟窖★拷锟絫opN , , , , �? �? ,
            // , ,  8, 8 vs , , , 4, 4 锟皆比ｏ�?  --analyze,  , ,
            if (cfg.analyze && validCount > 0)
            {
                double best4 = 1e30, best8 = 1e30;
                int best4idx = -1, best8idx = -1;
                for (int j = 0; j < N; ++j)
                {
                    if (indices[j] < 0) continue;
                    // GPU scores 锟窖猴拷 8, 8 grid, 4, 4 , ,  = , �?8, 8 , ,  + 4, 4 , ,
                    double grid8d = gridDistance8x8(allGrid[ti], allRecords[indices[j]].grid4x4);
                    double grid4d = gridDistance(tileGrid4x4[ti], libGrid4x4[indices[j]]);
                    double score4 = scores[j] - nGridW * grid8d + nGridW * grid4d;
                    double score8 = scores[j];  // GPU , ,  8, 8
                    if (score4 < best4) { best4 = score4; best4idx = indices[j]; }
                    if (score8 < best8) { best8 = score8; best8idx = indices[j]; }
                }
                if (best4idx >= 0) grid4Top1++;
                if (best8idx >= 0) grid8Top1++;
                if (best4idx != best8idx) top1Differ++;
            }

            std::vector<int> idxs(N);
            for (int j = 0; j < N; ++j) idxs[j] = j;
            int topN = std::min(cfg.topNrandom, std::min(N, validCount));
            std::partial_sort(idxs.begin(), idxs.begin() + topN, idxs.end(),
                [&](int a, int b) { return scores[a] < scores[b]; });
            thread_local std::mt19937 rng(std::random_device{}());
            int rankPos = std::uniform_int_distribution<int>(0, topN - 1)(rng);       // �? �?  0-based, ,  rank-1
            int pick = idxs[rankPos];
            int chosenLibIdx = indices[pick];
            bestLibIdx[ti] = chosenLibIdx;
            bestRecords[ti] = allRecords[chosenLibIdx];
            // --analyze: , 录��?  tile , , , , 锟�??�? , , , �? , 锟狡�??��, , , �?
            if (cfg.analyze)
            {
                const auto& rec = allRecords[chosenLibIdx];
                double labD  = labDistance(allTL[ti], allTA[ti], allTB[ti], rec.avgL, rec.avgA, rec.avgB);
                double gridD = gridDistance8x8(allGrid[ti], rec.grid4x4, true);
                const auto* recTiny = rec.tinyPath.empty() ? nullptr : analysisFeatureCache.loadTiny(rec.id, rec.tinyPath);
                const auto* recLBP = rec.histPath.empty() ? nullptr : analysisFeatureCache.loadLBP(rec.id, rec.histPath);
                double tinyD = recTiny ? tinyMSE(allTiny[ti], *recTiny) : 1.0;
                double edgeD = std::abs(allEdge[ti] - rec.edgeDensity);
                double lbpD = recLBP ? lbpDistance(allLBP[ti], *recLBP) : 1.0;
                double totalW = cfg.labWeight + cfg.gridWeight + cfg.tinyWeight + cfg.edgeWeight + cfg.lbpWeight;
                double featScore = (cfg.labWeight*labD + cfg.gridWeight*gridD + cfg.tinyWeight*tinyD
                                  + cfg.edgeWeight*edgeD + cfg.lbpWeight*lbpD) / totalW;
                analyzeScores.push_back(featScore);
                analyzeImageIds.push_back(rec.id);
                analyzeLabD.push_back(labD);
                analyzeGridD.push_back(gridD);
                analyzeEdgeD.push_back(edgeD);

                // Top-K Gap: winner vs true best, , 锟酵凤拷, 原�??, , 锟筋�?
                double winnerScore = scores[pick];
                double gap = 0.0;
                if (validCount >= 2)
                {
                    if (rankPos == 0)  // winner , , ,
                        gap = scores[idxs[1]] - winnerScore;
                    else               // , , �? �?
                        gap = winnerScore - scores[idxs[0]];
                }
                analyzeGaps.push_back(gap);
                analyzeRanks.push_back(rankPos + 1);  // 1-based rank in sorted Top-N

                // ANN rank: winner ,  ANN , �? , 械锟轿伙拷锟?(0=, , )
                int annRank = -1;
                for (int j = 0; j < N; ++j)
                {
                    if (allIndices[ti * N + j] == chosenLibIdx) { annRank = j; break; }
                }
                analyzeAnnRanks.push_back(annRank);

                // , 锟洁�? , , 应权, , �? �? ,
                int cat = 3;  // Normal
                if (allEdge[ti] < 0.005)
                {
                    double lSum = 0, lSq = 0;
                    for (int k = 0; k < 64; ++k)
                    {
                        double lv = allGrid[ti][k * 3];
                        lSum += lv; lSq += lv * lv;
                    }
                    double lVar = lSq / 64.0 - (lSum / 64.0) * (lSum / 64.0);
                    if (lVar < 100.0) cat = 0;  // Smooth
                }
                else if (allEdge[ti] > 0.01) { cat = 1; }  // Edge
                else
                {
                    double lbpEnt = 0.0;
                    for (int k = 0; k < 256; ++k)
                    {
                        float v = allLBP[ti][k];
                        if (v > 0.0f) lbpEnt -= v * std::log2(v);
                    }
                    if (lbpEnt > 3.0) cat = 2;  // Texture
                }
                analyzeCat.push_back(cat);

                // Grid 8, 8 �?cell , 锟阶ｏ拷锟�??硷拷选锟�????碉拷 cell LAB , ,
                for (int ci = 0; ci < 64; ++ci)
                {
                    int off = ci * 3;
                    double dl = allGrid[ti][off] / 255.0 - rec.grid4x4[off] / 255.0;
                    double da = allGrid[ti][off+1] / 255.0 - rec.grid4x4[off+1] / 255.0;
                    double db = allGrid[ti][off+2] / 255.0 - rec.grid4x4[off+2] / 255.0;
                    analyzeGridCellSum[ci] += std::sqrt(dl*dl + da*da + db*db);
                }
            }
            // �? , , , 锟节猴拷频锟绞硷�?
            int chosenId = bestRecords[ti].id;
            recentIds.push_back(chosenId);
            freqInWindow[chosenId]++;
            lastUsedAt[chosenId] = ti;       // , �? 锟绞癸拷锟轿伙拷�?
            recentGrids.push_back(allRecords[chosenLibIdx].grid4x4);
            while (static_cast<int>(recentGrids.size()) > GRID_DUP_WINDOW)
                recentGrids.pop_front();
            if (static_cast<int>(recentIds.size()) > cfg.neighborWindow)
            {
                int oldId = recentIds.front();
                recentIds.pop_front();
                if (--freqInWindow[oldId] <= 0)
                    freqInWindow.erase(oldId);
            }
        }
        cntGrid = totalTiles; cntTiny = totalTiles;
        cntEdge = totalTiles; cntLBP  = totalTiles;
        if (noCandidateCount > 0)
            std::cout << " (" << noCandidateCount << " tiles had no candidates!)";
        // 8, 8 Grid 锟皆憋拷�?
        if (totalTiles > 0)
        {
            int validTiles = totalTiles - noCandidateCount;
            if (cfg.analyze && validTiles > 0)
            {
                std::cout << "\n  Grid 8x8 experiment: "
                          << "Top1 differ=" << top1Differ << "/" << validTiles
                          << " (" << std::fixed << std::setprecision(1)
                          << (100.0 * top1Differ / validTiles) << "%)";
            }
        }
        std::cout << " done" << std::endl;

        // Phase D , �?
        auto tSelect = Clock::now();
        msSelect = Ms(tSelect - tLast).count();
        tLast = tSelect;

        // , ,  Phase E: , �?, ,
        int nThreads = std::thread::hardware_concurrency();
        if (nThreads < 2) nThreads = 2;

        if (cfg.tiledOutput)
        {
            // 锟�??匡拷, , 锟矫?tile , , 锟侥硷拷, 锟睫尺达�? 锟狡ｏ拷, , �?Mat
            std::error_code ec;
            std::string level0Dir = outputPath + "_files/0";
            std::filesystem::create_directories(level0Dir, ec);
            std::cout << "  writing tiles (" << nThreads << " threads)..."
                      << std::flush;
            std::atomic<int> tileDone{0};
            std::atomic<int> tileFail{0};
            std::vector<std::thread> tileWorkers;
            ImageCache imgCache;  // 锟�??程帮拷全, ,
            for (int t = 0; t < nThreads; ++t) {
                tileWorkers.emplace_back([&, t]() {
                    using Ns = std::chrono::nanoseconds;
                    char fname[512];
                    for (int ti = t; ti < totalTiles; ti += nThreads) {
                        int libIdx = bestLibIdx[ti];
                        if (libIdx < 0) { tileFail++; continue; }
                        int ty = ti / tilesX, tx = ti % tilesX;
                        const auto& rec = bestRecords[ti];
                        auto t0 = Clock::now();
                        cv::Mat r = imgCache.getOrLoad(
                            rec.id, rec.filePath, outTileW, outTileH);
                        if (r.empty()) { tileFail++; continue; }
                        auto t1 = Clock::now();
                        opPlaceDecodeNs += std::chrono::duration_cast<Ns>(t1 - t0).count();
                        if (cfg.colorAdjust) { adjustColor(r, cfg.colorStrength); }
                        // DZI , �? {name}_files/{level}/{col}_{row}.jpg
                        snprintf(fname, sizeof(fname), "%s/%d_%d.jpg",
                                 level0Dir.c_str(), tx, ty);
                        imwriteUnicode(fname, r, {cv::IMWRITE_JPEG_QUALITY, cfg.jpegQuality});
                        int d = ++tileDone;
                        if (d % 2000 == 0 || d == totalTiles)
                            std::cout << "\r  writing " << d << "/" << totalTiles << std::flush;
                    }
                });
            }
            for (auto& w : tileWorkers) w.join();
            matched = totalTiles - tileFail.load();
            loadFail = tileFail.load();
            std::cout << std::endl;
            std::cout << "Level 0: " << matched << " / " << totalTiles << " tiles";
            if (loadFail > 0) std::cout << "  (failed: " << loadFail << ")";
            std::cout << std::endl;
            releaseGpuLib();

            if (cfg.deepZoom)
            {
                std::cout << "  building pyramid levels..." << std::endl;
                DeepZoomWriter::buildPyramid(level0Dir, outTileW, outTileH,
                                             tilesX, tilesY, cfg.jpegQuality);
            }

            // , �? �?
            msPlace = Ms(Clock::now() - tLast).count();
            printBenchmark("tiled");
            runAnalysis(outputPath);
        return true;
        }

        // , �? �?
        int64_t rawBytes = static_cast<int64_t>(outW) * outH * 3;

        // --- 统一�? 模式, 锟�??ｏ拷,  PNG/TIFF , �? JPG ,  stream , �? , , ---
        // auto, PNG/TIFF , 锟捷匡拷, 锟节达拷 ,  batch/stream, JPG �? �?
        // stream, �? , �? , 写锟教ｏ�? 锟节存）
        // batch, �? �? , , 锟揭伙拷锟叫达拷�?
        bool useStream = false;   // true=, �? false=�?
        bool isHeavyFormat = (cfg.outputFormat == "png" || cfg.outputFormat == "tiff" || cfg.outputFormat == "jpg");
        bool isJpg = (cfg.outputFormat == "jpg");
        if (isHeavyFormat && rawBytes > 500LL * 1024 * 1024)
        {
            if (cfg.writeMode == "stream")
            {
                useStream = true;
            }
            else if (cfg.writeMode == "batch")
            {
                useStream = false;
            }
            else if (isJpg)
            {
#ifdef _WIN32
                MEMORYSTATUSEX mem = { sizeof(mem) };
                if (GlobalMemoryStatusEx(&mem))
                    useStream = (mem.ullAvailPhys < static_cast<ULONGLONG>(rawBytes) * 2);
                else
                    useStream = true;
#else
                useStream = true;
#endif
            }
            else // auto, PNG/TIFF , 锟捷匡拷, 锟节达拷锟皆讹拷�?
            {
#ifdef _WIN32
                MEMORYSTATUSEX mem = { sizeof(mem) };
                if (GlobalMemoryStatusEx(&mem))
                    useStream = (mem.ullAvailPhys < static_cast<ULONGLONG>(rawBytes) * 2);
                else
                    useStream = true;
#else
                useStream = true;
#endif
            }
        }
        // else: <500MB , �?PNG/TIFF ,  锟�??憋拷准路,

        if (isHeavyFormat && useStream)
            std::cout << "  (streaming mode ,  low memory)" << std::endl;
        else if (isHeavyFormat && rawBytes > 500LL * 1024 * 1024)
            std::cout << "  (batch mode ,  full buffer " << (rawBytes / 1024 / 1024) << " MB)" << std::endl;

        // --- , �?TIFF ---
        if (isHeavyFormat && useStream && cfg.outputFormat == "tiff") {
            BigTiffWriter tiff(outputPath, outW, outH, true);
            std::vector<uint8_t> rowBuf(outW * 3);
            int streamFail = 0;
            int nLoaders = std::min(8, static_cast<int>(std::thread::hardware_concurrency()));
            for (int ty = 0; ty < tilesY; ++ty)
            {
                // , 锟�??筹拷�? , , , ,  tile
                std::vector<cv::Mat> tileRowImgs(tilesX);
                {
                    std::atomic<int> nextTx{0};
                    std::vector<std::thread> loaders;
                    for (int t = 0; t < nLoaders; ++t)
                        loaders.emplace_back([&]() {
                            for (int tx = nextTx++; tx < tilesX; tx = nextTx++)
                            {
                                int ti = ty * tilesX + tx;
                                if (ti >= totalTiles) { tileRowImgs[tx] = cv::Mat(); continue; }
                                cv::Mat m = imreadUnicode(bestRecords[ti].filePath, cv::IMREAD_COLOR);
                                if (!m.empty())
                                    cv::resize(m, tileRowImgs[tx], cv::Size(outTileW, outTileH), 0, 0, cv::INTER_AREA);
                                if (!m.empty() && cfg.colorAdjust) adjustColor(tileRowImgs[tx], cfg.colorStrength);
                            }
                        });
                    for (auto& w : loaders) w.join();
                }
                // , , , , �?
                for (int tx = 0; tx < tilesX; ++tx)
                    if (tileRowImgs[tx].empty()) streamFail++;
                for (int y = 0; y < outTileH; ++y)
                {
                    for (int tx = 0; tx < tilesX; ++tx)
                    {
                        uint8_t* dst = &rowBuf[tx * outTileW * 3];
                        if (tileRowImgs[tx].empty()) {
                            std::memset(dst, 0, outTileW * 3);
                        } else {
                            cv::Mat tr = tileRowImgs[tx].row(y);
                            std::memcpy(dst, tr.data, outTileW * 3);
                        }
                    }
                    if (!tiff.writeRow(ty * outTileH + y, rowBuf.data())) {
                        std::cerr << "\n  TIFF writeRow failed at row " << (ty * outTileH + y) << std::endl;
                        tiff.close();
                        releaseGpuLib();
                        return false;
                    }
                }
                if (ty % 50 == 0)
                    std::cout << "\r  streaming row " << (ty * outTileH) << "/" << outH << std::flush;
            }
            tiff.close();
            matched = totalTiles - streamFail;
            loadFail = streamFail;
            std::cout << "\r  streaming done: " << outH << " rows" << std::endl;
            std::cout << "Mosaic saved: " << outputPath << "  (" << matched
                      << " / " << totalTiles << " tiles"
                      << (loadFail > 0 ? ", loadFail=" + std::to_string(loadFail) : "")
                      << ")" << std::endl;
            printBenchmark("single");
            runAnalysis(outputPath);
            releaseGpuLib();
        return true;
        }  // if (tiff streaming)
        else if (cfg.outputFormat == "png" && !useStream)
        {
            // PNG batch 模式, �? , 锟藉，一, �?
            std::cout << "  (batch mode ,  full buffer " << (rawBytes / 1024 / 1024) << " MB)" << std::endl;
            mosaicraft::PngBatchWriter png(outputPath, outW, outH, cfg.pngCompressionLevel);
            std::vector<cv::Mat> imgs(tilesX);
            int streamFail = 0;
            int nLd = std::min(8, (int)std::thread::hardware_concurrency());
            for (int ty = 0; ty < tilesY; ++ty) {
                { std::atomic<int> nx{0}; std::vector<std::thread> ld;
                  for (int t = 0; t < nLd; ++t) ld.emplace_back([&]() {
                      for (int tx = nx++; tx < tilesX; tx = nx++) {
                          int ti = ty * tilesX + tx; if (ti >= totalTiles) { imgs[tx] = cv::Mat(); continue; }
                          cv::Mat m = imreadUnicode(bestRecords[ti].filePath, cv::IMREAD_COLOR);
                          if (m.empty()) { imgs[tx] = cv::Mat(); continue; } cv::resize(m, imgs[tx], cv::Size(outTileW, outTileH), 0, 0, cv::INTER_AREA); if (cfg.colorAdjust) adjustColor(imgs[tx], cfg.colorStrength);
                      }});
                  for (auto& w : ld) w.join(); }
                for (int tx = 0; tx < tilesX; ++tx)
                    if (imgs[tx].empty()) streamFail++;
                for (int y = 0; y < outTileH; ++y) {
                    uint8_t* dst = png.rowData(ty * outTileH + y);
                    for (int tx = 0; tx < tilesX; ++tx) {
                        uint8_t* tileDst = dst + tx * outTileW * 3;
                        if (imgs[tx].empty()) {
                            std::memset(tileDst, 0, outTileW * 3);
                        } else {
                            std::memcpy(tileDst, imgs[tx].ptr<const uint8_t>(y), outTileW * 3);
                        }
                    }
                }
                if (ty % 10 == 0) std::cout << "\r  batching " << (ty+1) << "/" << tilesY << std::flush;
            }
            if (!png.writeAll()) {
                std::cerr << "\n  PNG writeAll failed" << std::endl;
                releaseGpuLib();
                return false;
            }
            matched = totalTiles - streamFail;
            loadFail = streamFail;
            std::cout << "\r  batch done: " << outH << " rows" << std::endl;
            std::cout << "Mosaic saved: " << outputPath << "  (" << matched
                      << " / " << totalTiles << " tiles"
                      << (loadFail > 0 ? ", loadFail=" + std::to_string(loadFail) : "")
                      << ")" << std::endl;
            printBenchmark("single");
            runAnalysis(outputPath);
            releaseGpuLib();
        return true;
        }
        else if (cfg.outputFormat == "png")
        {
            // PNG stream 模式, , , 写锟教ｏ拷锟节达拷愣?~162KB
            std::cout << "  (streaming mode ,  low memory)" << std::endl;
            mosaicraft::PngStreamWriter png(outputPath, outW, outH, cfg.pngCompressionLevel);
            std::vector<cv::Mat> imgs(tilesX);
            std::vector<uint8_t> rowBuf(outW * 3);
            int streamFail = 0;
            int nLd = std::min(8, (int)std::thread::hardware_concurrency());
            for (int ty = 0; ty < tilesY; ++ty) {
                { std::atomic<int> nx{0}; std::vector<std::thread> ld;
                  for (int t = 0; t < nLd; ++t) ld.emplace_back([&]() {
                      for (int tx = nx++; tx < tilesX; tx = nx++) {
                          int ti = ty * tilesX + tx; if (ti >= totalTiles) { imgs[tx] = cv::Mat(); continue; }
                          cv::Mat m = imreadUnicode(bestRecords[ti].filePath, cv::IMREAD_COLOR);
                          if (m.empty()) { imgs[tx] = cv::Mat(); continue; } cv::resize(m, imgs[tx], cv::Size(outTileW, outTileH), 0, 0, cv::INTER_AREA); if (cfg.colorAdjust) adjustColor(imgs[tx], cfg.colorStrength);
                      }});
                  for (auto& w : ld) w.join(); }
                for (int tx = 0; tx < tilesX; ++tx)
                    if (imgs[tx].empty()) streamFail++;
                for (int y = 0; y < outTileH; ++y) {
                    uint8_t* dst = rowBuf.data();
                    for (int tx = 0; tx < tilesX; ++tx) {
                        if (imgs[tx].empty()) {
                            std::memset(dst, 0, outTileW * 3);
                        } else {
                            std::memcpy(dst, imgs[tx].ptr<const uint8_t>(y), outTileW * 3);
                        }
                        dst += outTileW * 3;
                    }
                    for (int x = 0; x < outW; ++x) {
                        std::swap(rowBuf[x * 3], rowBuf[x * 3 + 2]);
                    }
                    if (!png.writeRow(rowBuf.data())) {
                        std::cerr << "\n  PNG writeRow failed at row " << (ty * outTileH + y) << std::endl;
                        releaseGpuLib();
                        return false;
                    }
                }
                if (ty % 10 == 0) std::cout << "\r  streaming " << (ty+1) << "/" << tilesY << std::flush;
            }
            png.close();
            matched = totalTiles - streamFail;
            loadFail = streamFail;
            std::cout << "\r  streaming done: " << outH << " rows" << std::endl;
            std::cout << "Mosaic saved: " << outputPath << "  (" << matched
                      << " / " << totalTiles << " tiles"
                      << (loadFail > 0 ? ", loadFail=" + std::to_string(loadFail) : "")
                      << ")" << std::endl;
            printBenchmark("single");
            runAnalysis(outputPath);
            releaseGpuLib();
        return true;
        }

        // --- , �?JPG ---
        if (isHeavyFormat && useStream && cfg.outputFormat == "jpg")
        {
            std::cout << "  (streaming mode ,  JPG low memory)" << std::endl;
            mosaicraft::JpgStreamWriter jpg(outputPath, outW, outH, cfg.jpegQuality);
            std::vector<cv::Mat> imgs(tilesX);
            std::vector<uint8_t> rowBuf(outW * 3);
            int streamFail = 0;
            int nLd = std::min(8, (int)std::thread::hardware_concurrency());
            for (int ty = 0; ty < tilesY; ++ty) {
                { std::atomic<int> nx{0}; std::vector<std::thread> ld;
                  for (int t = 0; t < nLd; ++t) ld.emplace_back([&]() {
                      for (int tx = nx++; tx < tilesX; tx = nx++) {
                          int ti = ty * tilesX + tx; if (ti >= totalTiles) { imgs[tx] = cv::Mat(); continue; }
                          cv::Mat m = imreadUnicode(bestRecords[ti].filePath, cv::IMREAD_COLOR);
                          if (m.empty()) { imgs[tx] = cv::Mat(); continue; } cv::resize(m, imgs[tx], cv::Size(outTileW, outTileH), 0, 0, cv::INTER_AREA); if (cfg.colorAdjust) adjustColor(imgs[tx], cfg.colorStrength);
                      }});
                  for (auto& w : ld) w.join(); }
                for (int tx = 0; tx < tilesX; ++tx)
                    if (imgs[tx].empty()) streamFail++;
                for (int y = 0; y < outTileH; ++y) {
                    uint8_t* dst = rowBuf.data();
                    for (int tx = 0; tx < tilesX; ++tx) {
                        if (imgs[tx].empty()) {
                            std::memset(dst, 0, outTileW * 3);
                        } else {
                            std::memcpy(dst, imgs[tx].ptr<const uint8_t>(y), outTileW * 3);
                        }
                        dst += outTileW * 3;
                    }
                    // BGR, RGB 原锟�??���?
                    for (int x = 0; x < outW; ++x) {
                        std::swap(rowBuf[x * 3], rowBuf[x * 3 + 2]);
                    }
                    if (!jpg.writeRow(rowBuf.data())) {
                        std::cerr << "\n  JPG writeRow failed at row " << (ty * outTileH + y) << std::endl;
                        releaseGpuLib();
                        return false;
                    }
                }
                if (ty % 10 == 0) std::cout << "\r  streaming " << (ty+1) << "/" << tilesY << std::flush;
            }
            jpg.close();
            matched = totalTiles - streamFail;
            loadFail = streamFail;
            std::cout << "\r  streaming done: " << outH << " rows" << std::endl;
            std::cout << "Mosaic saved: " << outputPath << "  (" << matched
                      << " / " << totalTiles << " tiles"
                      << (loadFail > 0 ? ", loadFail=" + std::to_string(loadFail) : "")
                      << ")" << std::endl;
            printBenchmark("single");
            runAnalysis(outputPath);
            releaseGpuLib();
        return true;
        }

        output = cv::Mat(outH, outW, CV_8UC3, cv::Scalar(64, 64, 64));
        std::cout << "  placing tiles (" << nThreads << " threads)..."
                  << std::flush;
        std::atomic<int> placeDone{0};
        std::atomic<int> placeFail{0};
        std::atomic<int> placeNoCand{0};  // , �? �? 锟铰碉拷�?
        std::atomic<int> placeLoadErr{0}; // 锟侥硷拷, 取失,
        std::vector<std::thread> placeWorkers;
        ImageCache imgCache;  // 锟�??程帮拷全, 锟芥�? , 锟截革拷 imread
        for (int t = 0; t < nThreads; ++t)
        {
            placeWorkers.emplace_back([&, t]() {
                using Ns = std::chrono::nanoseconds;
                for (int ti = t; ti < totalTiles; ti += nThreads)
                {
                    int libIdx = bestLibIdx[ti];
                    if (libIdx < 0) { placeNoCand++; placeFail++; continue; }
                    const auto& rec = bestRecords[ti];
                    int ty = ti / tilesX, tx = ti % tilesX;

                    auto t0 = Clock::now();
                    cv::Mat resized = imgCache.getOrLoad(
                        rec.id, rec.filePath, outTileW, outTileH);
                    if (resized.empty())
                    {
                        placeLoadErr++; placeFail++;
                        opPlaceDecodeNs += std::chrono::duration_cast<Ns>(Clock::now() - t0).count();
                        continue;
                    }
                    auto t1 = Clock::now();
                    opPlaceDecodeNs += std::chrono::duration_cast<Ns>(t1 - t0).count();

                    if (cfg.colorAdjust) { adjustColor(resized, cfg.colorStrength); }
                    // �? 锟�??筹拷�? 锟截碉拷,  ROI, , , , �?
                    resized.copyTo(output(cv::Rect(tx * outTileW, ty * outTileH,
                                                  outTileW, outTileH)));
                    auto t2 = Clock::now();
                    opPlaceCopyNs += std::chrono::duration_cast<Ns>(t2 - t1).count();

                    int d = ++placeDone;
                    if (d % 500 == 0 || d == totalTiles)
                        std::cout << "\r  placing " << d << "/" << totalTiles
                                  << std::flush;
                }
            });
        }
        for (auto& w : placeWorkers) w.join();
        matched = totalTiles - placeFail.load();
        loadFail = placeFail.load();
        if (placeNoCand > 0 || placeLoadErr > 0)
            std::cout << "  (noCand=" << placeNoCand << " loadErr=" << placeLoadErr << ")";
        std::cout << std::endl;
    }
    else
    {
        // 锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋
        // CPU �? , ,  tile �? , , , 锟皆??�? �? �?
        // 锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋锟絋
        FeatureIndex annCpu;
        std::string annPath = featDirCache.empty() ? "lib.ann" : (featDirCache + "/lib.ann");
        std::cout << "  loading ANN index..." << std::flush;
        if (!annCpu.load(annPath, 708, allRecords)) {
            std::cout << " building..." << std::flush;
            annCpu.build(allRecords);
            if (!featDirCache.empty()) annCpu.save(annPath);
        }
        std::cout << " done" << std::endl;

        ImageCache imgCache;
        FeatureCache cpuFeatureCache;
        output = cv::Mat(outH, outW, CV_8UC3, cv::Scalar(64, 64, 64));
        int noCandidateCount = 0;

        // Phase 1: ANN , �?+ , , �? �? �? �?GPU �? ,
        std::vector<int> bestLibIdxCpu(totalTiles, -1);
        std::vector<ImageRecord> bestRecsCpu(totalTiles);
        std::deque<int> recentIds;
        std::unordered_map<int, int> freq;
        std::unordered_map<int, int> lastUsedAt;
        const int MIN_GAP = std::max(50, tilesX);
        if (cfg.neighborWindow <= 0) cfg.neighborWindow = autoNeighborWindow();

        std::cout << "  selecting best..." << std::flush;
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            std::vector<float> tileVec;
            buildTileVector(allTL[ti],allTA[ti],allTB[ti],allGrid[ti],
                            allTiny[ti],allEdge[ti],allLBP[ti], tileVec);
            auto imgIds = annCpu.query(tileVec.data(), N);
            if (imgIds.empty()) { noCandidateCount++; continue; }
            // , , , �?+ , , 头锟?
            std::vector<std::pair<double,int>> scored;
            for (int j = 0; j < (int)imgIds.size(); ++j) {
                int li = annCpu.idToAllRecordsIndex(imgIds[j]);
                if (li < 0) continue;
                const auto& r = allRecords[li];
                const auto* recTiny = r.tinyPath.empty() ? nullptr : cpuFeatureCache.loadTiny(r.id, r.tinyPath);
                const auto* recLBP = r.histPath.empty() ? nullptr : cpuFeatureCache.loadLBP(r.id, r.histPath);
                double labD  = cfg.labWeight*labDistance(allTL[ti],allTA[ti],allTB[ti],r.avgL,r.avgA,r.avgB);
                double gridD = cfg.gridWeight*gridDistance8x8(allGrid[ti], r.grid4x4);
                double edgeD = cfg.edgeWeight*std::abs(allEdge[ti]-r.edgeDensity);
                double s = labD
                         + gridD
                         + cfg.tinyWeight*(recTiny ? tinyMSE(allTiny[ti], *recTiny) : 1.0)
                         + edgeD
                         + cfg.lbpWeight*(recLBP ? lbpDistance(allLBP[ti], *recLBP) : 1.0);
                auto it = freq.find(r.id);
                int cnt = (it != freq.end()) ? it->second : 0;
                if (cnt >= 3) s += cfg.neighborPenalty;
                else if (cnt == 2) s += cfg.neighborPenalty * 0.4;
                else if (cnt == 1) s += cfg.neighborPenalty * 0.1;
                auto gapIt = lastUsedAt.find(r.id);  // 强锟狡硷拷锟?
                if (gapIt != lastUsedAt.end() && (ti - gapIt->second) < MIN_GAP) s += 500.0;
                scored.push_back({s, li});
            }
            if (scored.empty()) { noCandidateCount++; continue; }
            std::sort(scored.begin(), scored.end());
            int topN = std::min(cfg.topNrandom, (int)scored.size());
            thread_local std::mt19937 rng2(std::random_device{}());
            int pickIdx = scored[std::uniform_int_distribution<int>(0, topN - 1)(rng2)].second;
            bestLibIdxCpu[ti] = pickIdx;
            bestRecsCpu[ti] = allRecords[pickIdx];
            // --analyze: ֻ��¼ʤ���ߵ��������ݣ�ÿ�� tile һ����
            if (cfg.analyze) {
                const auto& w = allRecords[pickIdx];
                const auto* wTiny = w.tinyPath.empty() ? nullptr : cpuFeatureCache.loadTiny(w.id, w.tinyPath);
                const auto* wLBP = w.histPath.empty() ? nullptr : cpuFeatureCache.loadLBP(w.id, w.histPath);
                double wLabD  = cfg.labWeight*labDistance(allTL[ti],allTA[ti],allTB[ti],w.avgL,w.avgA,w.avgB);
                double wGridD = cfg.gridWeight*gridDistance8x8(allGrid[ti], w.grid4x4);
                double wEdgeD = cfg.edgeWeight*std::abs(allEdge[ti]-w.edgeDensity);
                double wS = wLabD + wGridD + wEdgeD
                    + cfg.tinyWeight*(wTiny ? tinyMSE(allTiny[ti], *wTiny) : 1.0)
                    + cfg.lbpWeight*(wLBP ? lbpDistance(allLBP[ti], *wLBP) : 1.0);
                analyzeScores.push_back(wS);
                analyzeImageIds.push_back(w.id);
                analyzeLabD.push_back(wLabD);
                analyzeGridD.push_back(wGridD);
                analyzeEdgeD.push_back(wEdgeD);
            }
            // �? , , , ,
            int chosenId = bestRecsCpu[ti].id;
            recentIds.push_back(chosenId); freq[chosenId]++; lastUsedAt[chosenId] = ti;
            if ((int)recentIds.size() > cfg.neighborWindow) {
                int old = recentIds.front(); recentIds.pop_front();
                if (--freq[old] <= 0) freq.erase(old);
            }
            if (ti % 5000 == 0 || ti == totalTiles-1)
                std::cout << "\r  selecting " << (ti+1) << "/" << totalTiles << std::flush;
        }
        cntGrid = cntTiny = cntEdge = cntLBP = totalTiles;
        if (noCandidateCount > 0)
            std::cout << " (" << noCandidateCount << " tiles no candidates!)";
        std::cout << " done" << std::endl;

        // Phase 2: , 锟�??筹拷, �?
        int nT = std::thread::hardware_concurrency();
        if (nT < 2) nT = 2; if (nT > 16) nT = 16;
        std::atomic<int> placed{0}, pFail{0};
        std::cout << "  placing (" << nT << " threads)..." << std::flush;
        std::vector<std::thread> pWorkers;
        for (int t = 0; t < nT; ++t) {
            pWorkers.emplace_back([&, t]() {
                for (int ti = t; ti < totalTiles; ti += nT) {
                    int li = bestLibIdxCpu[ti];
                    if (li < 0) { pFail++; continue; }
                    int ty = ti / tilesX, tx = ti % tilesX;
                    cv::Mat m = imgCache.getOrLoad(bestRecsCpu[ti].id, bestRecsCpu[ti].filePath, outTileW, outTileH);
                    if (m.empty()) { pFail++; continue; }
                    if (cfg.colorAdjust) adjustColor(m, cfg.colorStrength);
                    m.copyTo(output(cv::Rect(tx*outTileW,ty*outTileH,outTileW,outTileH)));
                    matched++;
                    int d = ++placed;
                    if (d % 2000 == 0 || d == totalTiles)
                        std::cout << "\r  placing " << d << "/" << totalTiles << std::flush;
                }
            });
        }
        for (auto& w : pWorkers) w.join();
        loadFail = pFail.load();
        bestRecords = bestRecsCpu;  // ͬ�������·��ʹ�õ�����
    }

    std::cout << std::endl;

    // , , , , �?
    std::string fmt = cfg.outputFormat;
    // ����δ��ʽָ����ʽʱ������չ���ƶ�
    if ((fmt == "jpg" || fmt.empty()) && !cfg.formatExplicit)
    {
        // , 锟皆达拷 outputPath , �? 锟狡讹拷
        auto dotPos = outputPath.rfind('.');
        if (dotPos != std::string::npos)
        {
            std::string ext = outputPath.substr(dotPos + 1);
            if (ext == "png" || ext == "PNG") fmt = "png";
            else if (ext == "webp" || ext == "WEBP") fmt = "webp";
            else if (ext == "tiff" || ext == "tif" || ext == "TIFF" || ext == "TIF") fmt = "tiff";
            else if (ext == "jpg" || ext == "jpeg" || ext == "JPG" || ext == "JPEG") fmt = "jpg";
        }
    }
    if (fmt != "jpg" && fmt != "png" && fmt != "webp" && fmt != "tiff") fmt = "jpg";

    // ��չ����������ʽ --format ���Զ���ʽ�л��󱣳����·�����ʽһ��
    std::string outPath = outputPath;
    {
        auto dotPos = outPath.rfind('.');
        auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
        if (dotPos != std::string::npos)
        {
            std::string oldExt = lower(outPath.substr(dotPos + 1));
            if (oldExt == "jpeg") oldExt = "jpg";
            if (oldExt == "tif")  oldExt = "tiff";
            if (cfg.formatExplicit && oldExt != fmt)
                outPath = outPath.substr(0, dotPos) + "." + fmt;
            else if (fmt == "tiff" && (oldExt == "jpg" || oldExt == "png" || oldExt == "webp"))
                outPath = outPath.substr(0, dotPos) + ".tiff";
            else if (fmt == "png" && oldExt == "jpg")
                outPath = outPath.substr(0, dotPos) + ".png";
        }
        else if (cfg.formatExplicit)
        {
            outPath += "." + fmt;
        }
    }

    // �? , �?
    if (fmt == "tiff")
    {
        if (output.empty())
        {
            BigTiffWriter tiff(outPath, outW, outH);
            std::vector<uint8_t> rowBuf(outW * 3);
            std::vector<char> failedTiles(totalTiles, 0);
            for (int ty = 0; ty < tilesY; ++ty)
            {
                for (int y = 0; y < outTileH; ++y)
                {
                    for (int tx = 0; tx < tilesX; ++tx)
                    {
                        int ti = ty * tilesX + tx;
                        uint8_t* dst = &rowBuf[tx * outTileW * 3];
                        if (ti >= totalTiles) {
                            std::memset(dst, 0, outTileW * 3);
                            continue;
                        }
                        cv::Mat m = imreadUnicode(bestRecords[ti].filePath, cv::IMREAD_COLOR);
                        if (m.empty()) {
                            failedTiles[ti] = 1;
                            std::memset(dst, 0, outTileW * 3);
                            continue;
                        }
                        cv::resize(m, m, cv::Size(outTileW, outTileH), 0, 0, cv::INTER_AREA);
                        cv::Mat tileRow = m.row(y);
                        std::memcpy(dst, tileRow.data, outTileW * 3);
                    }
                    if (!tiff.writeRow(ty * outTileH + y, rowBuf.data()))
                    {
                        std::cerr << "ERROR: BigTiffWriter failed at row "
                                  << (ty * outTileH + y) << std::endl;
                        releaseGpuLib();
                        return false;
                    }
                }
                if (ty % 20 == 0)
                    std::cout << "\r  streaming " << (ty+1) << "/" << tilesY << std::flush;
            }
            tiff.close();
            loadFail = static_cast<int>(std::count(failedTiles.begin(), failedTiles.end(), 1));
            matched = totalTiles - loadFail;
            std::cout << std::endl;
        }
        else
        {
            BigTiffWriter tiff(outPath, outW, outH);
            if (!tiff.writeMat(output.data, static_cast<int>(output.step)))
            {
                std::cerr << "ERROR: BigTiffWriter failed" << std::endl;
                releaseGpuLib();
                return false;
            }
            tiff.close();
        }
    }
    else
    {
        std::vector<int> writeParams;
        if (fmt == "jpg")
            writeParams = {cv::IMWRITE_JPEG_QUALITY, cfg.jpegQuality};
        else if (fmt == "png")
            writeParams = {cv::IMWRITE_PNG_COMPRESSION, cfg.pngCompressionLevel};
        else if (fmt == "webp")
            writeParams = {cv::IMWRITE_WEBP_QUALITY, cfg.jpegQuality};

        if (!imwriteUnicode(outPath, output, writeParams))
        {
            std::cerr << "ERROR: Cannot write output: " << outPath << std::endl;
            releaseGpuLib();
            return false;
        }
    }

    std::cout << "Mosaic saved: " << outPath
              << "  (" << matched << " / " << totalTiles << " tiles"
              << (loadFail > 0 ? ", loadFail=" + std::to_string(loadFail) : "")
              << ")"
              << std::endl;
    // �??��特征计数器�??�??��各路径可能在return前未完整设置�?
    if (cntEdge + cntMissEdge == 0) { cntEdge = totalTiles; cntMissEdge = 0; }
    if (cntGrid + cntMissGrid == 0) { cntGrid = totalTiles; cntMissGrid = 0; }
    if (cntTiny + cntMissTiny == 0) { cntTiny = totalTiles; cntMissTiny = 0; }
    if (cntLBP + cntMissLBP == 0)   { cntLBP  = totalTiles; cntMissLBP  = 0; }
    std::cout << "  Features used:"
              << " grid=" << cntGrid << "/" << (cntGrid + cntMissGrid)
              << " tiny=" << cntTiny << "/" << (cntTiny + cntMissTiny)
              << " edge=" << cntEdge << "/" << (cntEdge + cntMissEdge)
              << " lbp=" << cntLBP << "/" << (cntLBP + cntMissLBP)
              << std::endl;

    // , ,  ?? , , , , , ,  , ,
    runAnalysis(outPath);

    releaseGpuLib();

    // , �? �?
    msPlace = Ms(Clock::now() - tLast).count();
    printBenchmark("single");
    return true;
}

} // namespace mosaicraft
