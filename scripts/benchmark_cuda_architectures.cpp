// Explicit A/B harness: same private database, fixed RNG seed and captured tile IDs.
#include "core/MatchingAccess.h"
#include "core/UnicodeIO.h"
#include "core/json.hpp"
#include "compute/CudaBackend.h"
#include <chrono>
#include <fstream>
#include <numeric>
#include <opencv2/imgproc.hpp>

using namespace mosaicraft;
int main(int argc, char **argv)
{
    if (argc != 5)
    {
        return 1;
    }
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    if (!cuda::isCudaAvailable())
    {
        return 2;
    }
    const auto ready = Clock::now();
    constexpr int count = 5, candidates = 2;
    std::vector<uint8_t> images(count * 180 * 320 * 3), tiny(count * 256);
    cv::RNG random(123);
    cv::Mat pixels(1, static_cast<int>(images.size()), CV_8UC1, images.data());
    random.fill(pixels, cv::RNG::UNIFORM, 0, 256);
    std::vector<double> lab(count * 3), edge(count);
    std::vector<float> grid(count * 192), lbp(count * 256);
    if (cuda::extractFeaturesRaw(images.data(), count, 180, 320, lab.data(), grid.data(), tiny.data(), edge.data(), lbp.data()) != 0)
    {
        return 3;
    }
    cuda::GpuLibrary library;
    int use[count] = {1, 2, 3, 4, 5};
    if (!cuda::uploadLibrary(library, lab.data(), grid.data(), tiny.data(), edge.data(), lbp.data(), use, count))
    {
        return 4;
    }
    double l[count], a[count], b[count], weights[count];
    int indices[count * candidates];
    for (int i = 0; i < count; ++i)
    {
        l[i] = lab[i * 3]; a[i] = lab[i * 3 + 1]; b[i] = lab[i * 3 + 2]; weights[i] = 0.2;
        indices[i * 2] = i; indices[i * 2 + 1] = (i + 1) % count;
    }
    std::vector<double> scores(count * candidates);
    const bool scored = cuda::scoreBatch(count, l, a, b, grid.data(), tiny.data(), edge.data(), lbp.data(), indices, candidates,
        library, weights, weights, weights, weights, weights, 0.01, scores.data());
    cuda::freeLibrary(library);
    if (!scored)
    {
        return 5;
    }
    MosaicEngine engine;
    MatchingAccess::configure(engine, MatchingLimits{}, true, 123);
    std::vector<int> selected;
    MatchingAccess::captureSelections(engine, selected);
    MosaicEngine::Config cfg;
    cfg.outW = 180; cfg.outH = 320; cfg.nativeTileW = 18; cfg.nativeTileH = 32;
    cfg.candidates = 16; cfg.topNRandom = 3; cfg.lRange = 100;
    cfg.outputFormat = "png"; cfg.formatExplicit = true;
    // Brightness jitter has a separate random generator; leave it at the product
    // default so A/B pixels measure architecture changes, not fresh jitter.
    cfg.overlayStrength = 0.1;
    const auto generation = Clock::now();
    if (!engine.generate(argv[1], argv[2], argv[4], cfg))
    {
        return 6;
    }
    const auto end = Clock::now();
    nlohmann::json report = {
        {"startup_ms", std::chrono::duration<double, std::milli>(ready - begin).count()},
        {"generation_ms", std::chrono::duration<double, std::milli>(end - generation).count()},
        {"total_ms", std::chrono::duration<double, std::milli>(end - begin).count()},
        {"lab", lab}, {"grid", grid}, {"tiny", tiny}, {"lbp", lbp}, {"edge", edge},
        {"scores", scores}, {"selected", selected}
    };
    std::ofstream output(u8path(argv[3]));
    output << report.dump(2) << '\n';
    return output ? 0 : 7;
}
