// 只用于离线画质诊断；复用生产特征和评分，不改变生成路径。
#include "core/ApiRequestParser.h"
#include "core/FeatureExtractor.h"
#include "core/FeatureIndex.h"
#include "core/GridDuplicateCache.h"
#include "core/MatchingScore.h"
#include "core/UnicodeIO.h"
#include "core/json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <set>

using namespace mosaicraft;
using Json = nlohmann::json;

static Json readJson(const std::string &path)
{
    std::ifstream stream(u8path(path));
    Json value;
    stream >> value;
    return value;
}

static void saveJson(const std::string &path, const Json &value)
{
    std::ofstream stream(u8path(path));
    stream << value.dump(2) << '\n';
    stream.close();
    if (!stream)
    {
        throw std::runtime_error("cannot write quality results");
    }
}

static double percentile(std::vector<double> values, double fraction)
{
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(fraction * values.size()))];
}

struct Target
{
    MatchingBuffers features;
    int columns = 0, rows = 0;
};

static Target prepare(const Json &item, int featureWidth, int featureHeight)
{
    auto image = imreadUnicode(item.at("input").get<std::string>());
    if (image.empty())
    {
        throw std::runtime_error("cannot read diagnostic target");
    }
    cv::resize(image, image, cv::Size(item.at("width"), item.at("height")), 0, 0, cv::INTER_AREA);
    Target target;
    target.columns = (image.cols + 8) / 9;
    target.rows = (image.rows + 15) / 16;
    cv::copyMakeBorder(image, image, 0, target.rows * 16 - image.rows, 0,
                       target.columns * 9 - image.cols, cv::BORDER_REFLECT);
    const size_t count = size_t(target.columns) * target.rows;
    target.features.resize(count, 0, 0, false, false);
    // 一个公共 CPU 参考空间，避免把模板 CUDA 的近似误差当成方案效果。
    WorkerPool workers(8);
    workers.run(count, [&](size_t index)
    {
        cv::Mat native, lab, gray;
        const cv::Rect area(int(index % target.columns) * 9, int(index / target.columns) * 16, 9, 16);
        cv::resize(image(area), native, cv::Size(featureWidth, featureHeight), 0, 0, cv::INTER_LINEAR);
        cv::cvtColor(native, lab, cv::COLOR_BGR2Lab);
        const auto mean = cv::mean(lab);
        auto &f = target.features;
        f.l[index] = mean[0];
        f.a[index] = mean[1];
        f.b[index] = mean[2];
        f.grid[index] = computeGrid8x8FromLab(lab);
        cv::cvtColor(native, gray, cv::COLOR_BGR2GRAY);
        f.tiny[index] = computeTinyImageFromGray(gray);
        f.lbp[index] = computeLBPHistogramFromGray(gray);
        f.edge[index] = computeEdgeDensityFromGray(gray);
        f.labW[index] = 0.20;
        f.gridW[index] = 0.45;
        f.tinyW[index] = 0.25;
        f.edgeW[index] = f.lbpW[index] = 0.05;
    });
    return target;
}

static std::vector<int> readSelection(const std::string &path, size_t count)
{
    std::ifstream stream(u8path(path), std::ios::binary);
    std::vector<int> indices(count);
    stream.read(reinterpret_cast<char *>(indices.data()), count * sizeof(int));
    if (!stream || stream.peek() != std::char_traits<char>::eof())
    {
        throw std::runtime_error("selection length does not match target grid");
    }
    return indices;
}

static Json evaluate(const Target &target, const std::vector<int> &indices,
                     const std::vector<ImageRecord> &records,
                     const std::vector<FeaturePack::Reader::Row> &payloads, GridDuplicateCache &duplicates)
{
    std::vector<double> scores;
    std::vector<std::pair<double, size_t>> ranked;
    std::set<int> unique;
    std::map<int, int> frequencies;
    for (size_t i = 0; i < indices.size(); ++i)
    {
        const int selected = indices[i];
        if (selected < 0)
        {
            continue;
        }
        if (size_t(selected) >= records.size())
        {
            throw std::runtime_error("selection index exceeds snapshot");
        }
        // usePenalty=0 排除历史使用惩罚，衡量可比较的五特征误差。
        scores.push_back(cpuBaseScore(target.features, i, records[selected], payloads[selected], 0));
        ranked.emplace_back(scores.back(), i);
        unique.insert(selected);
        ++frequencies[selected];
    }
    if (scores.empty())
    {
        throw std::runtime_error("no matched tiles in quality evaluation");
    }
    Json result = {{"matched", scores.size()}, {"missing", indices.size() - scores.size()},
                   {"unique_images", unique.size()},
                   {"mean", std::accumulate(scores.begin(), scores.end(), 0.0) / scores.size()},
                   {"p90", percentile(scores, 0.90)}, {"p99", percentile(scores, 0.99)}};
    int maximum = 0;
    for (const auto &entry : frequencies)
    {
        maximum = std::max(maximum, entry.second);
    }
    result["max_image_uses"] = maximum;
    std::partial_sort(ranked.begin(), ranked.begin() + std::min<size_t>(5, ranked.size()), ranked.end(),
                      std::greater<std::pair<double, size_t>>());
    result["worst_tiles"] = Json::array();
    for (size_t i = 0; i < std::min<size_t>(5, ranked.size()); ++i)
    {
        result["worst_tiles"].push_back(ranked[i].second);
    }
    for (int radius = 1; radius <= 3; ++radius)
    {
        uint64_t pairs = 0, same = 0, similar = 0;
        std::vector<bool> repeated(indices.size(), false);
        for (int y = 0; y < target.rows; ++y)
        {
            for (int x = 0; x < target.columns; ++x)
            {
                const int a = y * target.columns + x;
                if (indices[a] < 0)
                {
                    continue;
                }
                for (int dy = 0; dy <= radius; ++dy)
                {
                    for (int dx = -radius; dx <= radius; ++dx)
                    {
                        // 每对只统计一次，半径使用图块中心的欧氏距离。
                        if ((dy == 0 && dx <= 0) || dx * dx + dy * dy > radius * radius ||
                            x + dx < 0 || x + dx >= target.columns || y + dy >= target.rows)
                        {
                            continue;
                        }
                        const int b = (y + dy) * target.columns + x + dx;
                        if (indices[b] < 0)
                        {
                            continue;
                        }
                        ++pairs;
                        if (indices[a] == indices[b])
                        {
                            ++same;
                            repeated[a] = repeated[b] = true;
                        }
                        else if (duplicates.similar(indices[a], indices[b], 0.010))
                        {
                            ++similar;
                        }
                    }
                }
            }
        }
        result["neighborhood"][std::to_string(radius)] = {
            {"pairs", pairs}, {"same_pairs", same}, {"similar_distinct_pairs", similar},
            {"same_pair_rate", pairs ? double(same) / pairs : 0},
            {"similar_including_same_rate", pairs ? double(same + similar) / pairs : 0},
            {"tiles_with_same_neighbor", std::count(repeated.begin(), repeated.end(), true)}
        };
    }
    return result;
}

static Json oracle(const Target &target, const std::vector<ImageRecord> &records,
                   const std::vector<FeaturePack::Reader::Row> &payloads, const FeatureIndex &index)
{
    std::vector<Json> rows(32);
    WorkerPool workers(8);
    workers.run(rows.size(), [&](size_t sample)
    {
        const size_t tile = sample * (size_t(target.columns) * target.rows - 1) / 31;
        std::vector<std::pair<double, int>> eligible;
        double unrestricted = INFINITY;
        for (size_t i = 0; i < records.size(); ++i)
        {
            const double score = cpuBaseScore(target.features, tile, records[i], payloads[i], 0);
            unrestricted = std::min(unrestricted, score);
            if (std::abs(records[i].avgL - target.features.l[tile]) <= 20)
            {
                eligible.emplace_back(score, int(i));
            }
        }
        const size_t top = std::min<size_t>(10, eligible.size());
        std::partial_sort(eligible.begin(), eligible.begin() + top, eligible.end());
        Json row = {{"tile", tile}, {"all_library_best", unrestricted}, {"eligible_count", eligible.size()}};
        row["brightness_eligible_best"] = top ? Json(eligible[0].first) : Json(nullptr);
        row["brightness_filter_loss"] = top ? Json(eligible[0].first - unrestricted) : Json(nullptr);
        std::vector<float> query;
        const auto &f = target.features;
        buildTileVector(f.l[tile], f.a[tile], f.b[tile], f.grid[tile], f.tiny[tile], f.edge[tile], f.lbp[tile], query);
        for (const int k : {150, 600})
        {
            const auto nearest = index.query(query.data(), k);
            std::set<int> accepted;
            double best = INFINITY;
            for (const int id : nearest)
            {
                const int i = index.idToAllRecordsIndex(id);
                if (std::abs(records[i].avgL - f.l[tile]) <= 20)
                {
                    accepted.insert(i);
                    best = std::min(best, cpuBaseScore(f, tile, records[i], payloads[i], 0));
                }
            }
            size_t recalled = 0;
            for (size_t i = 0; i < top; ++i)
            {
                recalled += accepted.count(eligible[i].second);
            }
            row["ann"][std::to_string(k)] = {
                {"accepted", accepted.size()}, {"top10_recall", top ? Json(double(recalled) / top) : Json(nullptr)},
                {"best", std::isfinite(best) ? Json(best) : Json(nullptr)},
                {"search_loss", top && std::isfinite(best) ? Json(best - eligible[0].first) : Json(nullptr)}
            };
        }
        rows[sample] = std::move(row);
    });
    return rows;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: benchmark_quality <study.json> <evaluation.json>\n";
        return 1;
    }
    try
    {
        const bool prepareCache = std::string(argv[1]) == "--prepare-cache";
        const auto study = prepareCache ? Json{{"snapshot", localToUtf8(argv[2])}} : readJson(argv[1]);
        Database db(study.at("snapshot").get<std::string>(), Database::OpenMode::Existing);
        if (!db.isOpen())
        {
            throw std::runtime_error(db.error());
        }
        const auto records = db.allRecords();
        if (records.empty())
        {
            throw std::runtime_error("empty snapshot");
        }
        const auto directory = pathToUtf8(u8path(records.front().tinyPath).parent_path());
        if (prepareCache)
        {
            if (!FeaturePack::buildCache(directory, records))
            {
                throw std::runtime_error("cannot prepare private feature cache");
            }
            std::cout << "Private feature cache ready: " << records.size() << std::endl;
            return 0;
        }
        FeaturePack::Reader reader(directory, records);
        std::vector<FeaturePack::Reader::Row> payloads;
        payloads.reserve(records.size());
        for (size_t i = 0; i < records.size(); ++i)
        {
            payloads.push_back(reader.get(int(i)));
            if (!payloads.back().tinyValid || !payloads.back().lbpValid)
            {
                throw std::runtime_error("incomplete feature payload");
            }
        }
        FeatureIndex index;
        const auto annPath = directory + "/lib.ann196";
        if (!index.load(annPath, FeatureIndex::kDimension, records) && !index.build(records))
        {
            throw std::runtime_error("cannot initialize diagnostic ANN");
        }
        GridDuplicateCache duplicates(records);
        Json output = {{"reference", "CPU v6 five-feature score; no history/neighborhood penalty"},
                       {"library_rows", records.size()}, {"cases", Json::object()}, {"runs", Json::array()}};
        for (const auto &item : study.at("cases"))
        {
            const auto name = item.at("id").get<std::string>();
            std::cout << "Evaluating " << name << std::endl;
            const auto target = prepare(item, std::stoi(db.getMeta("feature_w")), std::stoi(db.getMeta("feature_h")));
            output["cases"][name] = {{"columns", target.columns}, {"rows", target.rows},
                                      {"oracle", oracle(target, records, payloads, index)}};
            std::map<std::string, Json> cache;
            for (const auto &run : study.at("runs"))
            {
                if (run.at("case") != name)
                {
                    continue;
                }
                const auto hash = run.at("selection_sha256").get<std::string>();
                if (!cache.count(hash))
                {
                    const auto selected = readSelection(run.at("selection"), size_t(target.columns) * target.rows);
                    cache[hash] = evaluate(target, selected, records, payloads, duplicates);
                }
                output["runs"].push_back({{"id", run.at("id")}, {"quality", cache.at(hash)}});
            }
            // 长诊断保留已完成场景，失败可定位，不接受残缺输出作为完整结果。
            saveJson(argv[2], output);
        }
        output["complete"] = true;
        saveJson(argv[2], output);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
