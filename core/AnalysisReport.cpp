#include "AnalysisReport.h"
#include "RuntimeContext.h"
#include "FeatureUtils.h"
#include "UnicodeIO.h"
#include "json.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace mosaicraft::detail
{
namespace
{
bool writeAnalysisThumbnail(RuntimeContext &runtime, const std::string &dir, const std::string &id,
                            const cv::Mat &image)
{
    const std::string path = dir + "/" + id + ".jpg";
    try
    {
        cv::Mat thumbnail = image;
        const int longestEdge = std::max(image.cols, image.rows);
        if (longestEdge > 256)
        {
            const double scale = 256.0 / longestEdge;
            cv::resize(image, thumbnail, {std::max(1, cvRound(image.cols * scale)),
                                          std::max(1, cvRound(image.rows * scale))}, 0, 0, cv::INTER_AREA);
        }
        if (imwriteUnicode(path, thumbnail, {cv::IMWRITE_JPEG_QUALITY, 85}))
        {
            runtime.artifact(id, "analysis-image", path, "image/jpeg");
            return true;
        }
    }
    catch (const std::exception &error)
    {
        runtime.emit("warning", "analysis_write_failed", error.what(), {}, path);
        return false;
    }
    runtime.emit("warning", "analysis_write_failed", "cannot write analysis thumbnail", {}, path);
    return false;
}

void writeAnalysisData(RuntimeContext &runtime, const std::string &dir, const nlohmann::json &data)
{
    const std::string path = dir + "/analysis-data.json";
    std::ofstream file(u8path(path), std::ios::binary);
    file << data;
    file.close();
    if (file)
    {
        runtime.artifact("analysis-data", "report", path, "application/json");
    }
    else
    {
        runtime.emit("warning", "analysis_write_failed", "cannot write analysis data", {}, path);
    }
}
} // namespace

std::unordered_map<int, int> writeAnalysisReport(const AnalysisReportContext &ctx, RuntimeContext *runtime)
{
    const auto &cfg = ctx.cfg;
    const auto &outputPath = ctx.outputPath;
    const auto &target = ctx.target;
    const int totalTiles = ctx.totalTiles;
    const int tilesX = ctx.tilesX;
    const int tilesY = ctx.tilesY;
    const int outTileW = ctx.outTileW;
    const int outTileH = ctx.outTileH;
    const int featW = ctx.featW;
    const int featH = ctx.featH;
    const int dbCount = ctx.dbCount;
    const int matched = ctx.matchedTiles > 0 ? ctx.matchedTiles : totalTiles;
    const auto &analyzeScores = ctx.analyzeScores;
    const auto &analyzeImageIds = ctx.analyzeImageIds;
    const auto &analyzeLabD = ctx.analyzeLabD;
    const auto &analyzeGridD = ctx.analyzeGridD;
    const auto &analyzeEdgeD = ctx.analyzeEdgeD;
    const auto &analyzeGaps = ctx.analyzeGaps;
    const auto &analyzeRanks = ctx.analyzeRanks;
    const auto &analyzeAnnRanks = ctx.analyzeAnnRanks;
    const auto &analyzeCat = ctx.analyzeCat;
    const double *analyzeGridCellSum = ctx.analyzeGridCellSum;
    const auto &worstTiles = ctx.worstTiles;
    const auto &allRecords = ctx.allRecords;
    const auto &selected = ctx.selected;

    if (!cfg.analyze || analyzeScores.empty() || analyzeGridCellSum == nullptr)
    {
        return {};
    }

    int n = static_cast<int>(analyzeScores.size());
    std::vector<double> sortedScores = analyzeScores;
    std::sort(sortedScores.begin(), sortedScores.end());
    double scoreMean = 0, scoreMin = 1e30, scoreMax = 0;
    for (double s : analyzeScores)
    {
        scoreMean += s;
        if (s < scoreMin)
        {
            scoreMin = s;
        }
        if (s > scoreMax)
        {
            scoreMax = s;
        }
    }
    scoreMean /= n;
    double scoreP50 = sortedScores[n / 2];
    double scoreP90 = sortedScores[n * 9 / 10];
    double scoreP99 = sortedScores[n * 99 / 100];
    if (runtime)
    {
        runtime->qualitySummary({static_cast<uint64_t>(n), scoreMean, scoreP50, scoreP90, scoreP99});
    }

    double labSum = 0, gridSum = 0, edgeSum = 0;
    double labW = cfg.labWeight, gridW = cfg.gridWeight, edgeW = cfg.edgeWeight;
    double totalW = labW + gridW + cfg.tinyWeight + edgeW + cfg.lbpWeight;
    labW /= totalW;
    gridW /= totalW;
    edgeW /= totalW;
    for (int i = 0; i < n; ++i)
    {
        labSum += labW * analyzeLabD[i];
        gridSum += gridW * analyzeGridD[i];
        edgeSum += edgeW * analyzeEdgeD[i];
    }
    double contribTotal = labSum + gridSum + edgeSum;

    std::unordered_map<int, int> useCount;
    for (int id : analyzeImageIds)
    {
        useCount[id]++;
    }
    std::vector<std::pair<int, int>> topUsed;
    for (auto &[id, cnt] : useCount)
    {
        topUsed.push_back({cnt, id});
    }
    std::sort(topUsed.rbegin(), topUsed.rend());

    std::cout << "\n=== Match Quality Analysis ===\n";
    std::cout << "  Scores describe selected tiles before brightness adjustment and base-image overlay.\n";
    std::cout << "  Tiles: " << n << "\n";
    std::cout << "  Score: mean=" << std::fixed << std::setprecision(4) << scoreMean << " median=" << scoreP50
              << " p90=" << scoreP90 << " p99=" << scoreP99 << " max=" << scoreMax << "\n";
    std::cout << "  Feature contribution (LAB/Grid/Edge only):\n";
    if (contribTotal > 0)
    {
        std::cout << "    LAB=" << std::setprecision(1) << (labSum * 100 / contribTotal)
                  << "%  Grid=" << (gridSum * 100 / contribTotal) << "%  Edge=" << (edgeSum * 100 / contribTotal)
                  << "%\n";
    }
    if (!analyzeGaps.empty())
    {
        auto sortedGaps = analyzeGaps;
        std::sort(sortedGaps.begin(), sortedGaps.end());
        double gapMean = 0;
        for (double g : analyzeGaps)
        {
            gapMean += g;
        }
        gapMean /= analyzeGaps.size();
        std::cout << "  Winner-RunnerUp gap: mean=" << std::setprecision(4) << gapMean
                  << " median=" << sortedGaps[analyzeGaps.size() / 2]
                  << " p90=" << sortedGaps[analyzeGaps.size() * 9 / 10] << "\n";
    }
    if (!analyzeRanks.empty())
    {
        int rankBuckets[4] = {0, 0, 0, 0};
        for (int r : analyzeRanks)
        {
            if (r <= 3)
            {
                rankBuckets[r - 1]++;
            }
            else
            {
                rankBuckets[3]++;
            }
        }
        double total = static_cast<double>(analyzeRanks.size());
        std::cout << "  Winner rank in TopN: #1=" << std::setprecision(1) << (rankBuckets[0] * 100 / total)
                  << "% #2=" << (rankBuckets[1] * 100 / total) << "% #3=" << (rankBuckets[2] * 100 / total) << "%\n";
    }
    if (!analyzeAnnRanks.empty())
    {
        int annTop1 = 0, annTop5 = 0, annTop10 = 0, annTop20 = 0, annTop50 = 0;
        for (int r : analyzeAnnRanks)
        {
            if (r == 0)
            {
                annTop1++;
            }
            if (r < 5)
            {
                annTop5++;
            }
            if (r < 10)
            {
                annTop10++;
            }
            if (r < 20)
            {
                annTop20++;
            }
            if (r < 50)
            {
                annTop50++;
            }
        }
        double t = static_cast<double>(analyzeAnnRanks.size());
        std::cout << "  ANN recall: Top1=" << std::setprecision(1) << (annTop1 * 100 / t)
                  << "% Top5=" << (annTop5 * 100 / t) << "% Top10=" << (annTop10 * 100 / t)
                  << "% Top20=" << (annTop20 * 100 / t) << "%\n";
    }
    if (!analyzeCat.empty())
    {
        double catScores[4] = {0, 0, 0, 0};
        int catCounts[4] = {0, 0, 0, 0};
        const char *catNames[] = {"Smooth", "Edge", "Texture", "Normal"};
        for (int i = 0; i < static_cast<int>(analyzeCat.size()); ++i)
        {
            int c = analyzeCat[i];
            if (c >= 0 && c < 4)
            {
                catScores[c] += analyzeScores[i];
                catCounts[c]++;
            }
        }
        std::cout << "  Score by category:\n";
        for (int c = 0; c < 4; ++c)
        {
            if (catCounts[c] > 0)
            {
                std::cout << "    " << catNames[c] << "(" << catCounts[c] << "): " << std::setprecision(4)
                          << (catScores[c] / catCounts[c]) << "\n";
            }
        }
    }

    std::cout << "  Worst 5 tiles:\n";
    const auto worstIdx = worstTiles.ranks();
    const int worstCount = static_cast<int>(worstIdx.size());
    for (int k = 0; k < std::min(5, worstCount); ++k)
    {
        int sample = worstIdx[k].second;
        int ti = worstTiles.at(sample).tile;
        int tx = ti % tilesX, ty = ti / tilesX;
        std::cout << "    (" << tx << "," << ty << ") score=" << std::fixed << std::setprecision(4)
                  << worstIdx[k].first;
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
        for (int i = 0; i < 64; ++i)
        {
            cellSum += analyzeGridCellSum[i];
        }
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
                if (d < 0.001)
                {
                    d = 0.001;
                }
                wTotal += 1.0 / d;
            }
            std::cout << "  Grid weights (auto-tuned): {";
            for (int r = 0; r < 8; ++r)
            {
                if (r > 0)
                {
                    std::cout << "   ";
                }
                for (int c = 0; c < 8; ++c)
                {
                    double w = (1.0 / (analyzeGridCellSum[r * 8 + c] / n)) / wTotal * 64.0;
                    std::cout << std::fixed << std::setprecision(2) << w;
                    if (r * 8 + c < 63)
                    {
                        std::cout << ",";
                    }
                }
                std::cout << (r < 7 ? "\n" : "}\n");
            }
        }
    }
    std::cout << "  Reuse: unique=" << useCount.size() << "/" << n << " ratio=" << std::setprecision(2)
              << (static_cast<double>(n) / useCount.size()) << "x\n";
    std::cout << "  Top 10 most used:\n";
    int top10Total = 0;
    for (int i = 0; i < std::min(10, static_cast<int>(topUsed.size())); ++i)
    {
        std::cout << "    id=" << topUsed[i].second << " : " << topUsed[i].first << " times\n";
        top10Total += topUsed[i].first;
    }
    std::cout << "  Top10 share: " << std::fixed << std::setprecision(1) << (100.0 * top10Total / n)
              << "% of all tiles\n";

    int freqDist[6] = {0};
    for (const auto &[id, cnt] : useCount)
    {
        if (cnt == 1)
        {
            freqDist[0]++;
        }
        else if (cnt == 2)
        {
            freqDist[1]++;
        }
        else if (cnt == 3)
        {
            freqDist[2]++;
        }
        else if (cnt <= 5)
        {
            freqDist[3]++;
        }
        else if (cnt <= 10)
        {
            freqDist[4]++;
        }
        else
        {
            freqDist[5]++;
        }
    }
    std::cout << "  Freq dist: 1x=" << freqDist[0] << " 2x=" << freqDist[1] << " 3x=" << freqDist[2]
              << " 4-5x=" << freqDist[3] << " 6-10x=" << freqDist[4] << " 10x+=" << freqDist[5] << "\n";

    std::string anaDir = outputPath;
    auto dp = anaDir.rfind('.');
    if (dp != std::string::npos)
    {
        anaDir = anaDir.substr(0, dp) + "_analysis";
    }
    else
    {
        anaDir += "_analysis";
    }
    std::error_code ec;
    std::filesystem::create_directories(u8path(anaDir), ec);
    if (ec)
    {
        if (runtime)
        {
            runtime->emit("warning", "analysis_write_failed", ec.message(), {}, anaDir);
        }
        else
        {
            std::cerr << "WARNING: analysis dir creation failed: " << ec.message() << std::endl;
        }
        return {};
    }

    std::string freqDir = anaDir + "/freq_rank";
    std::filesystem::create_directories(u8path(freqDir), ec);
    if (ec)
    {
        if (runtime)
        {
            runtime->emit("warning", "analysis_write_failed", ec.message(), {}, freqDir);
        }
        else
        {
            std::cerr << "WARNING: freq_rank dir creation failed: " << ec.message() << std::endl;
        }
        return {};
    }
    auto writeImage = [&](const std::string &path, const cv::Mat &image)
    {
        if (!imwriteUnicode(path, image) && runtime)
        {
            runtime->emit("warning", "analysis_write_failed", "cannot write report image", {}, path);
        }
    };
    int exported = 0;
    for (const auto &[cnt, id] : topUsed)
    {
        if (cnt < 2)
        {
            break;
        }
        for (int i = 0; i < dbCount; ++i)
        {
            if (allRecords[i].id == id && !allRecords[i].filePath.empty())
            {
                cv::Mat img = imreadUnicode(allRecords[i].filePath, cv::IMREAD_COLOR);
                if (!img.empty())
                {
                    std::string normFile = std::filesystem::path(allRecords[i].filePath).filename().string();
                    std::string fn = freqDir + "/rank" + (exported + 1 < 10 ? "0" : "") + std::to_string(exported + 1) +
                                     "_" + std::to_string(cnt) + "x_id" + std::to_string(id) + "_" + normFile;
                    writeImage(fn, img);
                    exported++;
                }
                break;
            }
        }
    }
    if (exported > 0)
    {
        std::cout << "  Frequency ranking: " << freqDir << "/ (x" << exported << ")\n";
    }

    auto webTiles = nlohmann::json::array();
    for (int k = 0; k < worstCount; ++k)
    {
        int sample = worstIdx[k].second;
        int ti = worstTiles.at(sample).tile;
        int tx = ti % tilesX, ty = ti / tilesX;
        char fname[256];
        cv::Mat tileROI = target(cv::Rect(tx * cfg.tileW, ty * cfg.tileH, cfg.tileW, cfg.tileH));
        cv::Mat tileBig;
        cv::resize(tileROI, tileBig, cv::Size(featW, featH), 0, 0, cv::INTER_NEAREST);
        const char *cn = (sample < static_cast<int>(analyzeCat.size()) && analyzeCat[sample] == 0)   ? "S"
                         : (sample < static_cast<int>(analyzeCat.size()) && analyzeCat[sample] == 1) ? "E"
                         : (sample < static_cast<int>(analyzeCat.size()) && analyzeCat[sample] == 2) ? "T"
                                                                                                     : "N";
        snprintf(fname, sizeof(fname), "%s/worst_%02d_s%.4f_%s_tile.png", anaDir.c_str(), k, worstIdx[k].first, cn);
        writeImage(fname, tileBig);
        const std::string rank = std::to_string(k + 1);
        nlohmann::json webTile;
        if (runtime)
        {
            webTile = {{"row", ty}, {"column", tx}, {"score", worstIdx[k].first},
                       {"imageId", allRecords[selected[ti]].id}};
            const std::string targetId = "analysis-target-" + rank;
            if (writeAnalysisThumbnail(*runtime, anaDir, targetId, tileROI))
            {
                webTile["targetArtifactId"] = targetId;
            }
        }
        if (selected[ti] >= 0 && !allRecords[selected[ti]].filePath.empty())
        {
            cv::Mat match = imreadUnicode(allRecords[selected[ti]].filePath, cv::IMREAD_COLOR);
            if (!match.empty())
            {
                snprintf(fname, sizeof(fname), "%s/worst_%02d_match.png", anaDir.c_str(), k);
                writeImage(fname, match);
                const std::string matchId = "analysis-match-" + rank;
                if (runtime && writeAnalysisThumbnail(*runtime, anaDir, matchId, match))
                {
                    webTile["matchArtifactId"] = matchId;
                }
            }
        }
        if (runtime)
        {
            webTiles.push_back(std::move(webTile));
        }
    }
    std::cout << "  Worst tiles exported: " << anaDir << "/ (x" << worstCount << ")\n";

    {
        std::string rptPath = anaDir + "/worst_report.txt";
        std::ofstream rpt(u8path(rptPath));
        rpt << "=== Worst Tile Analysis ===\n";
        for (int k = 0; k < worstCount; ++k)
        {
            int sample = worstIdx[k].second;
            int ti = worstTiles.at(sample).tile, tx = ti % tilesX, ty = ti / tilesX;
            const auto &rec = allRecords[selected[ti]];
            double labD = labDistance(worstTiles.at(sample).lab[0], worstTiles.at(sample).lab[1],
                                      worstTiles.at(sample).lab[2], rec.avgL, rec.avgA, rec.avgB);
            double gridD = gridDistance8x8(
                std::vector<float>(worstTiles.at(sample).grid.begin(), worstTiles.at(sample).grid.end()), rec.gridLab,
                true);
            double edgeD = std::abs(worstTiles.at(sample).edge - rec.edgeDensity);
            rpt << "\n#" << (k + 1) << " tile(" << tx << "," << ty << ") score=" << worstIdx[k].first << "\n";
            rpt << "  Tile  LAB=" << worstTiles.at(sample).lab[0] << "," << worstTiles.at(sample).lab[1] << ","
                << worstTiles.at(sample).lab[2] << " Edge=" << worstTiles.at(sample).edge << "\n";
            rpt << "  Match LAB=" << rec.avgL << "," << rec.avgA << "," << rec.avgB << " Edge=" << rec.edgeDensity
                << "\n";
            rpt << "  Dists: LAB=" << labD << " Grid=" << gridD << " Edge=" << edgeD << "\n";
            std::string cause;
            if (labD > 0.3)
            {
                cause = "color mismatch (LAB dist " + std::to_string(labD).substr(0, 4) + ")";
            }
            else if (gridD > 0.5)
            {
                cause = "spatial mismatch (Grid dist " + std::to_string(gridD).substr(0, 4) + ")";
            }
            else if (worstTiles.at(sample).lab[0] < 60)
            {
                cause = "dark region (tile L=" + std::to_string((int)worstTiles.at(sample).lab[0]) + ")";
            }
            else if (rec.useCount > 10)
            {
                cause = "popular image penalty (used " + std::to_string(rec.useCount) + "x)";
            }
            else
            {
                cause = "combined mismatch";
            }
            rpt << "  Cause: " << cause << "\n";
        }
        rpt.close();
        if (runtime)
        {
            if (rpt)
            {
                runtime->artifact("analysis-text", "report", rptPath, "text/plain; charset=utf-8");
            }
            else
            {
                runtime->emit("warning", "analysis_write_failed", "cannot write text report", {}, rptPath);
            }
        }
        std::cout << "  Diagnosis report: " << rptPath << "\n";
    }

    std::string heatPath = anaDir + "/heatmap.png";
    double sMin = sortedScores.front(), sRange = sortedScores.back() - sMin;
    if (sRange < 0.001)
    {
        sRange = 0.001;
    }
    int cellW = cfg.tileW;
    int cellH = cfg.tileH;
    cv::Mat heat(tilesY * cellH, tilesX * cellW, CV_8UC3, cv::Scalar(64, 64, 64));
    const int webColumns = std::min(256, tilesX);
    const int webRows = std::min(256, tilesY);
    std::vector<int> webBins(runtime ? webColumns * webRows : 0, -1);
    std::array<uint64_t, 32> scoreHistogram{};
    const double webRange = scoreMax - scoreMin;
    int sample = 0;
    for (int ty = 0; ty < tilesY; ++ty)
    {
        for (int tx = 0; tx < tilesX; ++tx)
        {
            int ti = ty * tilesX + tx;
            if (selected[ti] < 0)
            {
                continue;
            }
            double s = analyzeScores[sample++];
            if (runtime && std::isfinite(s))
            {
                // 保留区域内最高误差，避免孤立的坏格在缩小时被平均掉。
                const auto column = static_cast<int64_t>(tx) * webColumns / tilesX;
                const auto row = static_cast<int64_t>(ty) * webRows / tilesY;
                int &bin = webBins[row * webColumns + column];
                const double ratio = webRange > 0 ? std::clamp((s - scoreMin) / webRange, 0.0, 1.0) : 0.0;
                bin = std::max(bin, static_cast<int>(std::lround(255 * ratio)));
                ++scoreHistogram[std::min<size_t>(scoreHistogram.size() - 1, static_cast<size_t>(ratio * scoreHistogram.size()))];
            }
            double t = (s - sMin) / sRange;
            cv::Vec3b color;
            if (t < 0.5)
            {
                color = cv::Vec3b(0, static_cast<uchar>(255 * t * 2), static_cast<uchar>(255 * (1 - t * 2)));
            }
            else
            {
                color = cv::Vec3b(0, static_cast<uchar>(255 * (1 - (t - 0.5) * 2)), static_cast<uchar>(255));
            }
            cv::rectangle(heat, cv::Rect(tx * cellW, ty * cellH, cellW, cellH), color, cv::FILLED);
        }
    }
    // 缩放到与原图一致的比例
    double srcAspect = (double)target.cols / target.rows;
    double heatAspect = (double)heat.cols / heat.rows;
    if (std::abs(srcAspect - heatAspect) > 0.01)
    {
        int newW = static_cast<int>(heat.rows * srcAspect);
        int newH = heat.rows;
        if (newW > heat.cols)
        {
            newW = heat.cols;
            newH = static_cast<int>(heat.cols / srcAspect);
        }
        if (newW > 1 && newH > 1)
        {
            cv::resize(heat, heat, cv::Size(newW, newH), 0, 0, cv::INTER_NEAREST);
        }
    }
    writeImage(heatPath, heat);
    std::cout << "  Heatmap: " << heatPath << "\n";
    if (runtime)
    {
        writeAnalysisData(*runtime, anaDir,
                          {{"grid", {{"columns", tilesX}, {"rows", tilesY}}},
                           {"scores", {{"min", scoreMin}, {"median", scoreP50}, {"p90", scoreP90},
                                        {"p99", scoreP99}, {"max", scoreMax}, {"histogram", scoreHistogram}}},
                           {"heatmap", {{"columns", webColumns}, {"rows", webRows}, {"bins", std::move(webBins)}}},
                           {"worst", std::move(webTiles)}});
    }

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
        html << "<tr><th>Output</th><td>" << (tilesX * outTileW) << " x " << (tilesY * outTileH) << "</td></tr>\n";
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
            for (double s : analyzeScores)
            {
                int bi = static_cast<int>(s * 30);
                if (bi < 0)
                {
                    bi = 0;
                }
                if (bi > 9)
                {
                    bi = 9;
                }
                histBins[bi]++;
            }
            int histMax = 1;
            for (int h : histBins)
            {
                if (h > histMax)
                {
                    histMax = h;
                }
            }
            html << "<h2>Score Distribution</h2>\n";
            html << "<div style=\"font-family:monospace;font-size:12px;line-height:1.4\">\n";
            for (int i = 0; i < 10; ++i)
            {
                int w = (int)(60.0 * histBins[i] / histMax);
                double lo = i * 0.033;
                html << "<span style=\"color:#888\">" << std::fixed << std::setprecision(2) << lo << "</span> "
                     << "<span style=\"background:#e94560;display:inline-block;width:" << w << "px;height:12px\" "
                     << "title=\"" << histBins[i] << " tiles\"></span> " << histBins[i] << "<br>\n";
            }
            html << "</div>\n";
        }

        html << "<h2>Diversity</h2><table>\n";
        html << "<tr><th>Unique Images</th><td>" << useCount.size() << " / " << n << "</td></tr>\n";
        html << "<tr><th>Reuse Ratio</th><td>" << std::fixed << std::setprecision(2) << (double)n / useCount.size()
             << "x</td></tr>\n";
        html << "<tr><th>Top10 Share</th><td>" << std::setprecision(1) << (100.0 * top10Total / n) << "%</td></tr>\n";
        html << "</table>\n";

        html << "<h2>Top 10 Most Used</h2><table>\n";
        html << "<tr><th>Rank</th><th>Image ID</th><th>Uses</th><th>Bar</th></tr>\n";
        int topMax = topUsed.empty() ? 1 : topUsed[0].first;
        for (int i = 0; i < std::min(10, (int)topUsed.size()); ++i)
        {
            int w = (int)(100.0 * topUsed[i].first / topMax);
            html << "<tr><td>" << (i + 1) << "</td><td>" << topUsed[i].second << "</td><td>" << topUsed[i].first
                 << "</td><td><span class=\"bar\" style=\"width:" << w << "px\"></span></td></tr>\n";
        }
        html << "</table>\n";

        html << "<h2>Frequency Distribution</h2><table>\n";
        html << "<tr><th>Category</th><th>Count</th><th>%</th></tr>\n";
        const char *catNames[] = {"1x", "2x", "3x", "4-5x", "6-10x", "10x+"};
        int htmlFreqDist[6] = {0};
        for (const auto &[id, cnt] : useCount)
        {
            if (cnt == 1)
            {
                htmlFreqDist[0]++;
            }
            else if (cnt == 2)
            {
                htmlFreqDist[1]++;
            }
            else if (cnt == 3)
            {
                htmlFreqDist[2]++;
            }
            else if (cnt <= 5)
            {
                htmlFreqDist[3]++;
            }
            else if (cnt <= 10)
            {
                htmlFreqDist[4]++;
            }
            else
            {
                htmlFreqDist[5]++;
            }
        }
        for (int i = 0; i < 6; ++i)
        {
            html << "<tr><td>" << catNames[i] << "</td><td>" << htmlFreqDist[i] << "</td><td>" << std::setprecision(1)
                 << (100.0 * htmlFreqDist[i] / n) << "%</td></tr>\n";
        }
        html << "</table>\n";

        html << "<h2>Worst Tiles Gallery (Top 10)</h2>\n";
        html << "<div style=\"display:grid;grid-template-columns:repeat(5,1fr);gap:8px\">\n";
        constexpr int kShowWorst = 10;
        for (int k = 0; k < std::min(kShowWorst, worstCount); ++k)
        {
            int sample = worstIdx[k].second;
            int ti = worstTiles.at(sample).tile;
            int tx = ti % tilesX, ty = ti / tilesX;
            const char *cn = (sample < (int)analyzeCat.size() && analyzeCat[sample] == 0)   ? "S"
                             : (sample < (int)analyzeCat.size() && analyzeCat[sample] == 1) ? "E"
                             : (sample < (int)analyzeCat.size() && analyzeCat[sample] == 2) ? "T"
                                                                                            : "N";
            const char *cnFull = cn[0] == 'S' ? "Smooth" : cn[0] == 'E' ? "Edge" : cn[0] == 'T' ? "Texture" : "Normal";
            html << "<div style=\"background:#0f3460;padding:4px;text-align:center\">\n";
            html << "<img src=\"worst_" << std::setfill('0') << std::setw(2) << k << "_s" << std::fixed
                 << std::setprecision(4) << worstIdx[k].first << "_" << cn
                 << "_tile.png\" style=\"width:90px;height:160px\"><br>\n";
            html << "<img src=\"worst_" << std::setfill('0') << std::setw(2) << k
                 << "_match.png\" style=\"width:90px;height:160px;margin-top:2px\"><br>\n";
            html << "<span style=\"font-size:10px;color:#aaa\">(" << tx << "," << ty << ") " << std::setprecision(3)
                 << worstIdx[k].first << " " << cnFull << "</span>\n";
            html << "</div>\n";
        }
        html << "</div>\n";

        html << "<p>Heatmap: <a href=\"heatmap.png\">heatmap.png</a></p>\n";
        html << "</body></html>";
        html.close();
        if (runtime && html)
        {
            runtime->artifact("analysis-html", "report", htmlPath, "text/html; charset=utf-8");
        }
        std::cout << "  HTML report: " << htmlPath << "\n";
    }

    if (runtime && !html)
    {
        // 报告写入失败仍沿用原先的尽力生成和使用统计提交规则。
        runtime->emit("warning", "analysis_write_failed", "cannot write HTML report", {}, htmlPath);
    }

    // 仅原实现到达提交点时返回计数；提前退出仍不更新图库使用统计。
    return useCount;
}

} // namespace mosaicraft::detail
