#include "core/ApiRequestParser.h"
#include "core/MatchingAccess.h"
#include "core/MosaicService.h"
#include "core/RuntimeContext.h"
#include "core/UnicodeIO.h"
#include "core/json.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <opencv2/core/version.hpp>

using namespace mosaicraft;

// 复用 API 配置以指定小输出 tile，不给生产 CLI 增加基准专用参数。
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: benchmark_match <request.json> [small|indices|small-indices]"
                     " [--seed uint32] [--selections] [--match-only] [--batch-tiles count]"
                     " [--strategy L|Q|D|QD|QDX] [--epsilon fraction] [--diagnostics rows.json]"
                     " [--summary output.json]\n";
        return 1;
    }
    std::string profile, summaryPath;
    std::string strategy = "L", diagnosticsPath;
    SelectionOptions selectionOptions;
    bool fixedSeed = false, capture = false;
    bool matchOnly = false;
    size_t batchTiles = 0;
    uint32_t seed = 0;
    for (int i = 2; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--seed" && i + 1 < argc)
        {
            const std::string value = argv[++i];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seed);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            {
                std::cerr << "invalid uint32 seed\n";
                return 1;
            }
            fixedSeed = true;
        }
        else if (argument == "--summary" && i + 1 < argc)
        {
            summaryPath = argv[++i];
        }
        else if (argument == "--selections")
        {
            capture = true;
        }
        else if (argument == "--match-only")
        {
            matchOnly = true;
            capture = true;
        }
        else if (argument == "--batch-tiles" && i + 1 < argc)
        {
            const std::string value = argv[++i];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), batchTiles);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || batchTiles == 0)
            {
                std::cerr << "invalid batch size\n";
                return 1;
            }
        }
        else if (argument == "--strategy" && i + 1 < argc)
        {
            strategy = argv[++i];
            if (strategy != "L" && strategy != "Q" && strategy != "D" && strategy != "QD" && strategy != "QDX")
            {
                std::cerr << "invalid selection strategy\n";
                return 1;
            }
        }
        else if (argument == "--epsilon" && i + 1 < argc)
        {
            const std::string value = argv[++i];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), selectionOptions.relative);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                !std::isfinite(selectionOptions.relative) || selectionOptions.relative < 0)
            {
                std::cerr << "invalid quality tolerance\n";
                return 1;
            }
        }
        else if (argument == "--diagnostics" && i + 1 < argc)
        {
            diagnosticsPath = argv[++i];
        }
        else if (profile.empty() && (argument == "small" || argument == "indices" || argument == "small-indices"))
        {
            profile = argument;
        }
        else
        {
            std::cerr << "unknown benchmark option: " << argument << '\n';
            return 1;
        }
    }
    std::ifstream input(u8path(argv[1]));
    const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    MosaicRequest request;
    std::string error;
    if (!input || !parseMosaicRequestJson(body, request, error))
    {
        std::cerr << "invalid benchmark request: " << error << '\n';
        return 1;
    }
    request.config.benchmark = true;
    selectionOptions.quality = strategy == "Q" || strategy == "QD" || strategy == "QDX";
    selectionOptions.spatial = strategy == "D" || strategy == "QD" || strategy == "QDX";
    selectionOptions.expand = strategy == "QDX";
    if (!profile.empty() || fixedSeed || capture || !summaryPath.empty() || batchTiles ||
        selectionOptions.enabled() || !diagnosticsPath.empty())
    {
        MosaicEngine engine;
        MatchingLimits limits;
        if (profile == "small" || profile == "small-indices")
        {
            limits = MatchingLimits{64ULL * 1024 * 1024, 64ULL * 1024 * 1024, 256};
        }
        if (batchTiles)
        {
            limits.maxTiles = batchTiles;
        }
        MatchingAccess::configure(engine, limits, fixedSeed, seed);
        MatchingAccess::matchOnly(engine, matchOnly);
        std::vector<SelectionDiagnostic> diagnostics;
        MatchingAccess::selectionPolicy(engine, selectionOptions, diagnosticsPath.empty() ? nullptr : &diagnostics);
        std::vector<int> selected;
        capture = capture || profile == "indices" || profile == "small-indices";
        if (capture)
        {
            MatchingAccess::captureSelections(engine, selected);
        }
        RuntimeContext runtime;
        TaskResourceMonitor monitor(runtime, request.resourceMonitor);
        if (!engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config, runtime))
        {
            return 1;
        }
        monitor.stop();
        if (capture)
        {
            std::ofstream output(u8path(request.outputPath + ".indices.bin"), std::ios::binary);
            output.write(reinterpret_cast<const char *>(selected.data()), selected.size() * sizeof(int));
            if (!output)
            {
                return 1;
            }
        }
        if (!diagnosticsPath.empty())
        {
            nlohmann::json rows = nlohmann::json::array();
            for (const auto &d : diagnostics)
            {
                rows.push_back({{"selected", d.selected}, {"candidates", d.candidates},
                                {"quality_candidates", d.qualityCandidates}, {"minimum", d.minimum},
                                {"limit", d.limit}, {"error", d.error}, {"radius", d.radius},
                                {"radius3_candidates", d.radius3Candidates}, {"similar_fallback", d.similarFallback},
                                {"initial_candidates", d.initialCandidates}, {"initial_minimum", d.initialMinimum},
                                {"initial_radius3_candidates", d.initialRadius3Candidates}, {"queried", d.queried},
                                {"expansion_steps", d.expansionSteps}, {"expansion_ms", d.expansionMs},
                                {"radius_reason", d.radius >= 0 && d.radius < 3 ? "same_image_conflict" : "none"}});
            }
            std::ofstream output(u8path(diagnosticsPath));
            output << rows.dump() << '\n';
            output.close();
            if (!output)
            {
                return 1;
            }
        }
        if (!summaryPath.empty())
        {
            const auto state = runtime.snapshot();
            nlohmann::json summary = {
                {"seed", fixedSeed ? nlohmann::json(seed) : nlohmann::json(nullptr)},
                {"selected_count", selected.size()}, {"feature_backend", state.featureBackend},
                {"score_backend", state.scoreBackend}, {"width", state.width}, {"height", state.height},
                {"device", state.computeDevice}, {"score_mode", state.scoreMode}, {"kernel", state.kernelVersion},
                {"matched", state.matched}, {"failed", state.failed}, {"warnings", state.warningCount},
                {"metrics", state.metrics}, {"opencv_version", CV_VERSION}, {"match_only", matchOnly},
                {"strategy", strategy}, {"epsilon", selectionOptions.relative}
            };
            summary["resources"] = nlohmann::json::object();
            for (size_t i = 0; i < resourceNames.size(); ++i)
            {
                const auto &value = state.resources[i];
                summary["resources"][resourceNames[i]] = value.count ? nlohmann::json(value.peak) : nlohmann::json(nullptr);
            }
            std::ofstream output(u8path(summaryPath));
            output << summary.dump(2) << '\n';
            output.close();
            if (!output)
            {
                std::cerr << "cannot write benchmark summary\n";
                return 1;
            }
        }
        return 0;
    }
    const auto result = MosaicService().run(request);
    if (!result.ok)
    {
        std::cerr << result.message << '\n';
    }
    return result.exitCode;
}
