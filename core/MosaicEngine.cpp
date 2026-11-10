#include "compute/CompatibleScoring.h"
#include "MatchingScore.h"
#include "MosaicEngine.h"
#include "RuntimeContext.h"
#include "AnalysisReport.h"
#include "BenchmarkTimer.h"
#include "BrightnessAdjustment.h"
#include "CandidateFeatures.h"
#include "Database.h"
#include "FeatureExtractor.h"
#include "FeatureIndex.h"
#include "FeatureMatrix.h"
#include "FeaturePack.h"
#include "FeatureUtils.h"
#include "NeighborhoodState.h"
#include "MosaicOutput.h"
#include "MosaicMetrics.h"
#include "RequestValidation.h"
#include "UnicodeIO.h"
#include "compute/CudaBackend.h"
#include "compute/CompatibleFeatures.h"
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
#include <iomanip>
#include <mutex>
#include <random>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif
#include <string>
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

// ============================================================
// 主马赛克生成流程
// ============================================================
// Construct nested defaults after the enclosing class is complete (GCC/Clang).
bool MosaicEngine::generate(const std::string& targetPath,
                            const std::string& dbPath,
                            const std::string& outputPath)
{
    return generate(targetPath, dbPath, outputPath, Config{});
}

bool MosaicEngine::generate(const std::string& targetPath,
                             const std::string& dbPath,
                             const std::string& outputPath,
                             const Config& config)
{
    RuntimeContext runtime;
    return generate(targetPath, dbPath, outputPath, config, runtime);
}

bool MosaicEngine::generate(const std::string &targetPath, const std::string &dbPath, const std::string &outputPath,
                            const Config &config, RuntimeContext &runtime)
{
    runtime.stage("validating");
    m_compatibleFeatureImages = 0;
    m_compatibleScoreTiles = 0;
    m_compatibleTemporaryBytes = 0;
    // 检查 CUDA 可用性，GPU 不可用时默认回退 CPU
    MosaicRequest validated{targetPath, dbPath, outputPath, config};
    std::string validationError, validationField;
    if (!validateMosaicRequest(validated, validationError, &validationField))
    {
        {
            std::ostringstream detail;
            detail << "" << validationError;
            runtime.fail("invalid_argument", detail.str(), validationField);
        }
        return false;
    }
    Config cfg = validated.config;
    if (m_selectionOptions.enabled() &&
        (!std::isfinite(m_selectionOptions.relative) || m_selectionOptions.relative < 0 ||
         !std::isfinite(m_selectionOptions.absolute) || m_selectionOptions.absolute < 0 || cfg.analyze))
    {
        return runtime.fail("invalid_argument", "invalid experimental selection options or analyze enabled");
    }
    if (m_selectionObserver)
    {
        m_selectionObserver->clear();
    }
    const double scoreUsePenalty = m_selectionOptions.enabled() ? 0.0 : cfg.usePenalty;

    // Benchmark 计时变量
    using Clock = std::chrono::steady_clock;
    using Ms = std::chrono::duration<double, std::milli>;
    auto tStart = Clock::now();
    const auto preflight = cfg.useGpu ? cuda::queryCudaAvailability() : cuda::CudaAvailability{};
    const bool cudaEnabled = cfg.useGpu && preflight.available;
    const double preflightMs = Ms(Clock::now() - tStart).count();
    // 跨阶段统计集中持有，算子线程继续使用原子计数。
    detail::MosaicMetrics metrics;

    std::cout << "GPU: " << (cudaEnabled ? "CUDA enabled" : "disabled (CPU only)") << std::endl;
    if (cfg.useGpu && !cudaEnabled)
    {
        runtime.emit("warning", "cuda_unavailable", preflight.reason);
        runtime.backends("cpu", "cpu", preflight.reason);
        std::cout << "  CUDA unavailable; using CPU before computation: " << preflight.reason << std::endl;
    }
    if (cfg.benchmark && cfg.useGpu)
    {
        std::cout << "  CUDA preflight: " << preflightMs << " ms" << std::endl;
    }
    cfg.print();

    cv::Mat target = imreadUnicode(targetPath, cv::IMREAD_COLOR);
    if (target.empty())
    {
        {
            std::ostringstream detail;
            detail << "Cannot read target image: " << targetPath;
            runtime.fail("input_unreadable", detail.str(), "inputPath", targetPath);
        }
        return false;
    }
    if (!validImageSize(target.cols, target.rows))
    {
        {
            std::ostringstream detail;
            detail << "Target exceeds 100 million pixels.";
            runtime.fail("invalid_argument", detail.str(), "config");
        }
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
            {
                std::ostringstream detail;
                detail << "Target resize exceeds 100 million pixels.";
                runtime.fail("invalid_argument", detail.str(), "config");
            }
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
            {
                std::ostringstream detail;
                detail << "Upscaled target exceeds 100 million pixels.";
                runtime.fail("invalid_argument", detail.str(), "config");
            }
            return false;
        }
        cv::Mat up;
        cv::resize(target, up, cv::Size(static_cast<int>(width), static_cast<int>(height)), 0, 0, cv::INTER_LINEAR);
        target = up;
        std::cout << "Target upscaled " << cfg.upscale << "x: "
                  << target.cols << "x" << target.rows << std::endl;
    }

    runtime.stage("reading_library");
    Database db(dbPath, Database::OpenMode::Existing);
    if (!db.isOpen())
    {
        {
            std::ostringstream detail;
            detail << "Cannot open database: " << dbPath;
            runtime.fail("library_unavailable", detail.str(), "dbPath", dbPath);
        }
        return false;
    }

    int dbCount = db.totalCount();
    if (dbCount == 0)
    {
        {
            std::ostringstream detail;
            detail << "Database is empty.";
            runtime.fail("library_unavailable", detail.str(), "dbPath", dbPath);
        }
        return false;
    }
    std::cout << "Database: " << dbCount << " images" << std::endl;

    // Read feature resolution from DB meta (required; old DBs must be rebuilt)，旧库需重建
    std::string fw = db.getMeta("feature_w");
    std::string fh = db.getMeta("feature_h");
    if (fw.empty() || fh.empty())
    {
        {
            std::ostringstream detail;
            detail << "Database missing feature resolution metadata.";
            runtime.fail("library_incompatible", detail.str(), "dbPath", dbPath);
        }
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
        {
            std::ostringstream detail;
            detail << "Invalid DB feature dimensions. Rebuild the library.";
            runtime.fail("library_incompatible", detail.str(), "dbPath", dbPath);
        }
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
            {
                std::ostringstream detail;
                detail << "Invalid grid features for image " << record.id
                       << ". Rebuild this library before generating a mosaic.";
                runtime.fail("library_incompatible", detail.str(), "dbPath", dbPath);
            }
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
    MatchingBackends backends;
    if (cudaEnabled)
    {
        backends.features = cuda::hasTemplateFeatures(featW, featH)
            ? FeatureBackend::TemplateCuda : FeatureBackend::CompatibleCuda;
        backends.scoring = backends.features == FeatureBackend::TemplateCuda
            ? ScoreBackend::TemplateCuda : (m_limits.compatibleScoring ? ScoreBackend::CompatibleCuda : ScoreBackend::Cpu);
        if (backends.features == FeatureBackend::CompatibleCuda && !m_limits.compatibleFeatures)
        {
            backends.features = FeatureBackend::Cpu;
        }
    }
    const bool templateCuda = backends.scoring == ScoreBackend::TemplateCuda;
    bool compatibleFeatures = backends.features == FeatureBackend::CompatibleCuda;
    if (templateCuda)
    {
        std::cout << "Feature backend: template CUDA; scoring: CUDA." << std::endl;
    }
    if (templateCuda && cudaEnabled)
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
                                                   cfg.benchmark ? &metrics.libraryReadStats : nullptr);
        if (!cacheLoaded)
        {
            std::cout << "  (validated feature cache miss, using source files)" << std::endl;
        }
        for (int i = 0; i < dbCount; ++i)
        {
            if (!tinyValid[i] || !lbpValid[i])
            {
                runtime.emit("warning", "feature_missing", "missing features retain the existing matching penalty",
                             "dbPath", !tinyValid[i] ? allRecords[i].tinyPath : allRecords[i].histPath);
            }
        }
        if (cuda::uploadLibrary(gpuLib, h_lab.data(), h_grid.data(), h_tiny.data(), h_edge.data(), h_lbp.data(),
                                h_use.data(), dbCount, cfg.benchmark ? &metrics.libraryTimings : nullptr, tinyValid.data(),
                                lbpValid.data()))
        {
            std::cout << "GPU library: " << dbCount << " images ("
                      << (dbCount * (192*4+256+256*4) / 1024) << " KB)" << std::endl;
        }
        else
        {
            {
                std::ostringstream detail;
                detail << "GPU library upload failed.";
                runtime.fail("cuda_execution_failed", detail.str(), "");
            }
            releaseGpuLib();
            return false;
        }
    }

    // 计算 tile 网格：按 tileW x tileH 划分目标图
    int tilesX = (target.cols + cfg.tileW - 1) / cfg.tileW;
    int tilesY = (target.rows + cfg.tileH - 1) / cfg.tileH;

    detail::OutputFormat outputFormat;
    detail::OutputPlan outputPlan;
    std::string outputError;
    const bool singleImage = !cfg.tiledOutput && !cfg.deepZoom;
    if (!detail::resolveOutputFormat(targetPath, outputPath, cfg.outputFormat, cfg.formatExplicit,
                                     singleImage, outputFormat, outputError) ||
        !detail::makeOutputPlan(outputFormat, outputPath, tilesX, tilesY, cfg.nativeTileW, cfg.nativeTileH,
                                singleImage, outputPlan, outputError))
    {
        return runtime.fail("invalid_argument", outputError, "format");
    }
    const int outTileW = outputPlan.tileWidth, outTileH = outputPlan.tileHeight;
    const int outW = outputPlan.width, outH = outputPlan.height;
    // 实际编码格式不回写请求和匹配配置；报告、编码及产物使用同一份结果。
    Config outputConfig = cfg;
    outputConfig.outputFormat = outputPlan.format;
    if (singleImage)
    {
        runtime.planOutput(outputPlan, outputPath);
    }

    // 宽高比校验：DB 特征空间 vs 输出 tile，featW/featH 从meta解析而来
    if (featH <= 0 || featW <= 0) {
        {
            std::ostringstream detail;
            detail << "Invalid DB feature dimensions (" << featW << "x" << featH << ")";
            runtime.fail("library_incompatible", detail.str(), "dbPath", dbPath);
        }
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
        {
            std::ostringstream detail;
            detail << "Image too small for tile size.";
            runtime.fail("invalid_argument", detail.str(), "config");
        }
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
        {
            std::ostringstream detail;
            detail << "Too many tiles (" << totalTiles64 << ")";
            runtime.fail("invalid_argument", detail.str(), "config");
        }
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

    int N = cfg.candidates;  // 候选数量，GPU 路径需提前确定，用于 benchmark

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

    detail::NeighborhoodState neighborhood(allRecords, tilesX, cfg.neighborWindow, cfg.neighborPenalty);

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
    std::unique_ptr<detail::SelectionPolicy> selection;
    size_t selectionBytes = 0;
    if (m_selectionOptions.enabled())
    {
        selection = std::make_unique<detail::SelectionPolicy>(allRecords, neighborhood, m_selectionOptions,
                                                              cfg.usePenalty, N);
        selectionBytes = selection->capacityBytes();
        if (m_selectionObserver)
        {
            m_selectionObserver->resize(totalTiles);
            selectionBytes += m_selectionObserver->capacity() * sizeof(SelectionDiagnostic);
        }
    }
    if (selectionBytes >= m_limits.hostBytes)
    {
        return runtime.fail("invalid_argument", "selection diagnostics exceed matching host budget");
    }
    const size_t hostBudget = m_limits.hostBytes - selectionBytes;
    MatchingLimits batchLimits = m_limits;
    batchLimits.hostBytes = hostBudget;

    // 输出 Mat：批量模式时直接拼接，流式模式时仅作占位

    auto runAnalysis = [&](const std::string &analysisOutputPath)
    {
        if (cfg.analyze)
        {
            runtime.stage("analyzing");
        }
        const auto usage = detail::writeAnalysisReport(
            detail::AnalysisReportContext{
                outputConfig, analysisOutputPath, target,        totalTiles,      tilesX,
                tilesY,       outTileW,           outTileH,      featW,           featH,
                dbCount,      matched.load(),     analyzeScores, analyzeImageIds, analyzeLabD,
                analyzeGridD, analyzeEdgeD,       analyzeGaps,   analyzeRanks,    analyzeAnnRanks,
                analyzeCat,   analyzeGridCellSum, worstTiles,    allRecords,      bestLibIdx},
            &runtime);
        if (db.isOpen() && !usage.empty() && !db.recordRunUsage(usage, targetHash, targetPath))
        {
            runtime.fail("usage_commit_failed", "mosaic output saved, but usage statistics could not be committed",
                         "dbPath", dbPath);
            throw std::runtime_error("mosaic output saved, but usage statistics could not be committed");
        }
    };

    // 可控匹配缓冲按预算分批；图库记录和精简选择结果仍独立持有。
    size_t batchRows =
        std::min<size_t>(totalTiles, matchingBatchRows(batchLimits, featW, featH, N, dbCount, templateCuda));
    metrics.maximumBatchTiles = batchRows;
    int noCandidateCount = 0;
    int grid4Top1 = 0, grid8Top1 = 0, top1Differ = 0;
    int adaptiveSmoothCount = 0, adaptiveEdgeCount = 0, adaptiveTextureCount = 0, adaptiveNormalCount = 0;
    std::vector<double> edgeDistribution, entropyDistribution;
    std::mt19937 seededRng(m_seed);
    const auto matchingStart = Clock::now();
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
        metrics.msPrep = Ms(preparationEnd - tStart).count();
        runtime.stage("building_index");
        auto annIndex = std::make_unique<FeatureIndex>();
        const std::string annPath = featDirCache.empty() ? "lib.ann196" : featDirCache + "/lib.ann196";
        const auto indexStart = Clock::now();
        bool loaded = annIndex->load(annPath, FeatureIndex::kDimension, allRecords);
        metrics.msANNLoad = Ms(Clock::now() - indexStart).count();
        if (!loaded)
        {
            const auto buildStart = Clock::now();
            std::cout << "  building ANN index (196 dimensions)..." << std::flush;
            if (!annIndex->build(allRecords))
            {
                releaseGpuLib();
                return runtime.fail("library_unavailable", "ANN index could not be built", "dbPath", dbPath);
            }
            if (!annIndex->save(annPath))
            {
                {
                    std::ostringstream detail;
                    detail << "ANN cache could not be saved.";
                    runtime.emit("warning", "feature_cache_failed", detail.str(), "dbPath", annPath);
                }
            }
            metrics.msANNBuild = Ms(Clock::now() - buildStart).count();
        }

        runtime.stage("matching");
        WorkerPool queryWorkers;
        unsigned featureThreads = templateCuda ? std::min(8u, std::max(1u, std::thread::hardware_concurrency()))
                                             : std::max(2u, std::thread::hardware_concurrency());
        WorkerPool featureWorkers(static_cast<unsigned>(std::min<size_t>(batchRows, featureThreads)));
        std::unique_ptr<cuda::FeatureWorkspace> workspace;
        std::unique_ptr<cuda::ScoreWorkspace> scoreWorkspace;
        const size_t scratch = checkedMatchProduct(N, sizeof(int) + sizeof(std::pair<double, int>)) +
                               FeatureIndex::kDimension * sizeof(float);
        cuda::CompatibleFeatureWorkspace compatible;
        cuda::CompatibleScoreWorkspace compatibleScore;
        const size_t deviceBudget = std::min(m_limits.deviceBytes, size_t(256ULL * 1024 * 1024));
        cuda::CompatibleTimings compatibleTimings;
        std::vector<cv::Mat> compatibleImages;
        std::vector<cuda::CompatibleFeature> compatibleValues;
        if (compatibleFeatures)
        {
            const size_t requestedRows = std::min<size_t>(batchRows, 256);
            const size_t hostRows = cpuMatchingBufferBytes(requestedRows, N);
            compatibleFeatures = hostRows < hostBudget && scratch < hostBudget - hostRows &&
                compatible.prepare(featW, featH, static_cast<int>(requestedRows),
                                   hostBudget - hostRows - scratch, false, deviceBudget, m_limits.featureFault);
            backends.features = compatibleFeatures ? FeatureBackend::CompatibleCuda : FeatureBackend::Cpu;
            if (compatibleFeatures)
            {
                batchRows = static_cast<size_t>(compatible.capacity());
                metrics.maximumBatchTiles = batchRows;
            }
            if (!compatibleFeatures)
            {
                runtime.emit("warning", "cuda_resource_fallback",
                             "Generic CUDA resources unavailable; using CPU before extraction.");
                std::cout << "  Generic CUDA resources unavailable; using CPU before extraction." << std::endl;
            }
        }
        bool pipeline = false;
        size_t pinnedBytes = 0;
        if (templateCuda)
        {
            workspace = std::make_unique<cuda::FeatureWorkspace>();
            scoreWorkspace = std::make_unique<cuda::ScoreWorkspace>();
            // 先预算并准备全部可选资源；计算开始后不能用同步重算掩盖异步错误。
            const size_t scoreRowBytes = 9 * sizeof(double) + checkedMatchProduct(N, sizeof(int) + sizeof(double));
            while (batchRows > 1 &&
                   gpuMatchingBufferBytes(batchRows, featBytes, N, cfg.analyze) +
                           std::min<size_t>(batchRows, 256) * 2072 + batchRows * scoreRowBytes + scratch >
                       hostBudget)
            {
                batchRows /= 2;
            }
            metrics.maximumBatchTiles = batchRows;
            if (gpuMatchingBufferBytes(batchRows, featBytes, N, cfg.analyze) + scratch > hostBudget)
            {
                {
                    std::ostringstream detail;
                    detail << "Matching host budget cannot hold one CUDA tile.";
                    runtime.fail("cuda_execution_failed", detail.str(), "");
                }
                return false;
            }
            const size_t hostRows = gpuMatchingBufferBytes(batchRows, featBytes, N, cfg.analyze, true);
            const size_t scoreHost = batchRows * scoreRowBytes;
            if (m_limits.cudaPipeline && hostRows + scratch + scoreHost < hostBudget &&
                scoreRowBytes < 256ULL * 1024 * 1024)
            {
                cuda::FeaturePipelineConfig options;
                options.maxTiles = static_cast<int>(batchRows);
                options.width = featW;
                options.height = featH;
                options.downloadTiny = cfg.analyze;
                options.downloadLbp = cfg.analyze || cfg.adaptiveWeights;
                options.hostBudget = hostBudget - hostRows - scratch - scoreHost;
                options.deviceBudget -= scoreRowBytes;
                pipeline = workspace->preparePipeline(options, cfg.benchmark ? &metrics.featureTimings : nullptr);
                if (pipeline)
                {
                    const auto allocationStart = Clock::now();
                    pipeline = scoreWorkspace->preparePipeline(static_cast<int>(batchRows), N, scoreHost,
                                                                 workspace->residentBytes());
                    metrics.scoreTimings.allocationMs += Ms(Clock::now() - allocationStart).count();
                }
                if (!pipeline)
                {
                    // 释放已部分准备的可选资源后，才允许使用完整的原同步 CUDA 路径。
                    workspace->resetPipeline();
                    scoreWorkspace->resetPipeline();
                    metrics.featureTimings.pinnedBytes = metrics.featureTimings.residentBytes = metrics.featureTimings.pixelBytes = 0;
                }
                pinnedBytes = workspace->pinnedBytes() + scoreWorkspace->pinnedBytes();
            }
        }
        std::unique_ptr<FeaturePack::Reader> featureReader;
        if (!templateCuda || cfg.analyze)
        {
            const auto readStart = Clock::now();
            featureReader = std::make_unique<FeaturePack::Reader>(templateCuda ? std::string() : featDirCache, allRecords,
                                                                  m_limits.featureBytes, templateCuda ? nullptr : &runtime);
            if (!templateCuda)
            {
                metrics.msCPUFeatures += Ms(Clock::now() - readStart).count();
            }
        }
        if (backends.scoring == ScoreBackend::CompatibleCuda)
        {
            const auto allocationStart = Clock::now();
            // 先保留提取工作区；仅在连一行评分都放不下时缩小提取批次。
            const size_t scoreMinimum = 2048 + 9 * sizeof(double) +
                size_t(N) * (sizeof(int) + sizeof(double)) + 8 + 3 * sizeof(int);
            size_t rows = std::min<size_t>(batchRows, 256);
            bool ready = false;
            for (;;)
            {
                const size_t hostRows = cpuMatchingBufferBytes(rows, N) +
                    rows * N * (sizeof(double) + 2 * sizeof(int)) + scratch;
                const size_t hostUsed = hostRows + compatible.hostBytes();
                const size_t remainingHost = hostUsed < hostBudget ? hostBudget - hostUsed : 0;
                const size_t remainingDevice = compatible.deviceBytes() < deviceBudget
                    ? deviceBudget - compatible.deviceBytes() : 0;
                if (remainingHost >= scoreMinimum && remainingDevice >= scoreMinimum)
                {
                    ready = compatibleScore.prepare(static_cast<int>(rows), N, dbCount, remainingHost,
                                                     remainingDevice, m_limits.scoreFault, &preflight);
                    break;
                }
                if (rows == 1)
                {
                    break;
                }
                rows = std::max<size_t>(1, rows / 2);
                if (compatibleFeatures)
                {
                    const size_t reserved = cpuMatchingBufferBytes(rows, N) + scratch;
                    compatibleFeatures = reserved < hostBudget &&
                        compatible.prepare(featW, featH, static_cast<int>(rows), hostBudget - reserved,
                                           false, deviceBudget, m_limits.featureFault);
                    backends.features = compatibleFeatures ? FeatureBackend::CompatibleCuda : FeatureBackend::Cpu;
                    if (compatibleFeatures)
                    {
                        rows = std::min(rows, size_t(compatible.capacity()));
                    }
                }
            }
            metrics.scoreTimings.allocationMs += Ms(Clock::now() - allocationStart).count();
            if (ready)
            {
                batchRows = static_cast<size_t>(compatibleScore.capacity());
                pinnedBytes += compatibleScore.hostBytes();
                m_compatibleTemporaryBytes = compatible.deviceBytes() + compatibleScore.deviceBytes();
                // Reader 保留原来源校验及缺失标志，上传暂存按块复用，不复制整库到另一份主机数组。
                compatibleScore.uploadLibrary(allRecords, *featureReader, &metrics.libraryTimings);
                metrics.libraryReadStats = featureReader->stats();
            }
            else
            {
                backends.scoring = ScoreBackend::Cpu;
                runtime.emit("warning", "cuda_resource_fallback", compatibleScore.preparationFailure());
                // 评分准备失败不牺牲已经可用的提取能力。
                batchRows = compatibleFeatures ? size_t(compatible.capacity()) : batchRows;
                std::cout << "  Compatible CUDA scoring unavailable: "
                          << (rows == 1 && compatibleScore.capacity() == 0 &&
                              std::string(compatibleScore.preparationFailure()) == "not prepared"
                              ? "combined host/device budget cannot hold one row"
                              : compatibleScore.preparationFailure()) << "; using CPU scoring." << std::endl;
            }
            metrics.maximumBatchTiles = batchRows;
        }
        if (!templateCuda)
        {
            std::cout << "Feature backend: " << (compatibleFeatures ? "generic CUDA (CPU compatible)" : "CPU")
                      << "; scoring: " << (backends.scoring == ScoreBackend::CompatibleCuda
                          ? "compatible CUDA" : "CPU") << "." << std::endl;
        }
        const auto backendName = [](auto value)
        {
            return static_cast<int>(value) == 0   ? "cpu"
                   : static_cast<int>(value) == 1 ? "template_cuda"
                                                  : "compatible_cuda";
        };
        runtime.backends(backendName(backends.features), backendName(backends.scoring));
        std::vector<std::vector<float>> libGrid4x4;
        if (templateCuda && cfg.analyze)
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
            buffers.resize(count, featBytes, N, templateCuda, cfg.analyze, pipeline);
            if (backends.scoring == ScoreBackend::CompatibleCuda)
            {
                buffers.scores.resize(size_t(count) * N);
            }
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
            if (templateCuda)
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
                                           cfg.benchmark ? &metrics.featureTimings : nullptr);
                    }
                    else if (cuda::extractFeaturesRaw(input, pieceCount, featW, featH,
                                                 buffers.lab.data() + piece * 3, allGrid[piece].data(),
                                                 allTiny[piece].data(), allEdge.data() + piece, allLBP[piece].data(),
                                                 workspace.get(), cfg.benchmark ? &metrics.featureTimings : nullptr) < 0)
                    {
                        {
                            std::ostringstream detail;
                            detail << "GPU feature extraction failed.";
                            runtime.fail("cuda_execution_failed", detail.str());
                        }
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
            else if (compatibleFeatures)
            {
                compatibleImages.resize(count);
                featureWorkers.run(count, [&](size_t local)
                {
                    const int ti = first + static_cast<int>(local);
                    const auto roi = target(cv::Rect((ti % tilesX) * cfg.tileW, (ti / tilesX) * cfg.tileH,
                                                    cfg.tileW, cfg.tileH));
                    cv::resize(roi, compatibleImages[local], cv::Size(featW, featH), 0, 0, cv::INTER_LINEAR);
                });
                compatible.extract(compatibleImages, compatibleValues, featureWorkers, {}, &compatibleTimings);
                m_compatibleFeatureImages += count;
                for (int local = 0; local < count; ++local)
                {
                    const auto &feature = compatibleValues[local];
                    allTL[local] = feature.lab[0];
                    allTA[local] = feature.lab[1];
                    allTB[local] = feature.lab[2];
                    allEdge[local] = feature.edge;
                    std::copy(feature.grid.begin(), feature.grid.end(), allGrid[local].data());
                    std::copy(feature.tiny.begin(), feature.tiny.end(), allTiny[local].data());
                    std::copy(feature.lbp.begin(), feature.lbp.end(), allLBP[local].data());
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
                                           metrics.opResizeNs += std::chrono::duration_cast<Ns>(t1 - t0).count();
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
                                           metrics.opLabNs += std::chrono::duration_cast<Ns>(t2 - t1).count();
                                       }
                                       allGrid[local] = computeGrid8x8FromLab(lab);
                                       auto t3 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           metrics.opGridNs += std::chrono::duration_cast<Ns>(t3 - t2).count();
                                       }
                                       cv::Mat gray;
                                       cv::cvtColor(roiNative, gray, cv::COLOR_BGR2GRAY);
                                       allTiny[local] = computeTinyImageFromGray(gray);
                                       auto t4 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           metrics.opTinyNs += std::chrono::duration_cast<Ns>(t4 - t3).count();
                                       }
                                       allEdge[local] = computeEdgeDensityFromGray(gray);
                                       auto t5 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           metrics.opEdgeNs += std::chrono::duration_cast<Ns>(t5 - t4).count();
                                       }
                                       allLBP[local] = computeLBPHistogramFromGray(gray);
                                       auto t6 = cfg.benchmark ? Clock::now() : Clock::time_point{};
                                       if (cfg.benchmark)
                                       {
                                           metrics.opLbpNs += std::chrono::duration_cast<Ns>(t6 - t5).count();
                                       }
                                   });
            }
            metrics.msFeat += Ms(Clock::now() - featureStart).count();
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
            metrics.msANNQuery += Ms(Clock::now() - queryStart).count();
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

            if (templateCuda && cfg.adaptiveWeights)
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
            templateCuda && totalTiles > batchRows && hostBudget >= scratch + pinnedBytes &&
            gpuMatchingBufferBytes(batchRows, featBytes, N, cfg.analyze, pipeline) <=
                (hostBudget - scratch - pinnedBytes) / 2;
        metrics.matchingPrefetch = prefetch;
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
            ++metrics.matchingBatches;
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
            auto selectExperimental = [&](int ti)
            {
                SelectionDiagnostic diagnostic;
                thread_local std::mt19937 rng(std::random_device{}());
                const int chosen = selection->choose(ti, cfg.topNrandom, m_fixedSeed ? seededRng : rng, diagnostic);
                if (m_selectionObserver)
                {
                    (*m_selectionObserver)[ti] = diagnostic;
                }
                if (chosen < 0)
                {
                    ++noCandidateCount;
                    return;
                }
                bestLibIdx[ti] = chosen;
                neighborhood.recordSelection(chosen, ti);
            };
            if (templateCuda)
            {
                const auto scoreStart = Clock::now();
                cuda::GpuTimings batchTimings;
                const bool scoredOnGpu = pipeline
                    ? cuda::scoreDeviceBatch(workspace->deviceView(0, count), allTL.data(), allTA.data(), allTB.data(),
                                             allEdge.data(), allIndices.data(), N, gpuLib, tileLabW.data(),
                                             tileGridW.data(), tileTinyW.data(), tileEdgeW.data(), tileLbpW.data(),
                                             scoreUsePenalty, allScores.data(), *scoreWorkspace,
                                             cfg.benchmark ? &batchTimings : nullptr)
                    : cuda::scoreBatch(count, allTL.data(), allTA.data(), allTB.data(), allGrid.data(), allTiny.data(),
                                      allEdge.data(), allLBP.data(), allIndices.data(), N, gpuLib, tileLabW.data(),
                                      tileGridW.data(), tileTinyW.data(), tileEdgeW.data(), tileLbpW.data(),
                                      scoreUsePenalty, allScores.data(), cfg.benchmark ? &batchTimings : nullptr,
                                      256ULL * 1024 * 1024, scoreWorkspace.get());
                if (!scoredOnGpu)
                {
                    {
                        std::ostringstream detail;
                        detail << "GPU scoring failed; mosaic was not generated.";
                        runtime.fail("cuda_execution_failed", detail.str(), "");
                    }
                    return false;
                }
                metrics.scoreTimings.allocationMs += batchTimings.allocationMs;
                metrics.scoreTimings.uploadMs += batchTimings.uploadMs;
                metrics.scoreTimings.computeMs += batchTimings.computeMs;
                metrics.scoreTimings.downloadMs += batchTimings.downloadMs;
                metrics.scoreTimings.waitMs += batchTimings.waitMs;
                metrics.scoreTimings.eventUploadMs += batchTimings.eventUploadMs;
                metrics.scoreTimings.eventComputeMs += batchTimings.eventComputeMs;
                metrics.scoreTimings.eventDownloadMs += batchTimings.eventDownloadMs;
                metrics.scoreTimings.uploadBytes += batchTimings.uploadBytes;
                metrics.scoreTimings.downloadBytes += batchTimings.downloadBytes;
                metrics.scoreTimings.pinnedBytes = std::max(metrics.scoreTimings.pinnedBytes, batchTimings.pinnedBytes);
                metrics.scoreTimings.peakTemporaryBytes =
                    std::max(metrics.scoreTimings.peakTemporaryBytes, batchTimings.peakTemporaryBytes);
                metrics.msGPUScore += Ms(Clock::now() - scoreStart).count();
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
                    if (selection)
                    {
                        selection->clear();
                        for (int j = 0; j < N; ++j)
                        {
                            selection->add(indices[j], scores[j]);
                        }
                        selectExperimental(ti);
                        continue;
                    }
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
                        neighborhood.applyPenalty(scores[j], libIdx, ti);
                    }
                    // Top-N 随机选取：从前 topN 个最佳候选中随机选择
                    // 8, 8 vs 4, 4 Grid 对比实验（--analyze 模式）
                    if (cfg.benchmark)
                    {
                        metrics.msNeighbor += Ms(Clock::now() - neighborStart).count();
                    }
                    if (cfg.analyze && validCount > 0)
                    {
                        BenchmarkTimer analysisTimer(cfg.benchmark, metrics.msAnalysis);
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
                        metrics.msSort += Ms(Clock::now() - sortStart).count();
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
                        BenchmarkTimer analysisTimer(cfg.benchmark, metrics.msAnalysis);
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
                    neighborhood.recordSelection(chosenLibIdx, ti);
                }

                metrics.msSelect += Ms(Clock::now() - selectionStart).count();
            }
            else
            {
                const bool gpuScoring = backends.scoring == ScoreBackend::CompatibleCuda;
                if (gpuScoring)
                {
                    const auto scoreStart = Clock::now();
                    compatibleScore.score(buffers, count, scoreUsePenalty, allScores.data(), &metrics.scoreTimings);
                    metrics.msGPUScore += Ms(Clock::now() - scoreStart).count();
                    m_compatibleScoreTiles += count;
                }
                // 分析仍读取原 CPU 特征；普通 GPU 评分无需重复读取所有候选。
                if (!gpuScoring || cfg.analyze)
                {
                    const auto readStart = Clock::now();
                    featureReader->prepare(allIndices);
                    const double readMs = Ms(Clock::now() - readStart).count();
                    metrics.msCPUFeatures += readMs;
                    metrics.msSelect += readMs;
                }
                const auto selectionStart = Clock::now();
                for (int local = 0; local < count; ++local)
                {
                    const int ti = first + local;
                    if (selection)
                    {
                        selection->clear();
                        for (int j = 0; j < N; ++j)
                        {
                            const size_t offset = static_cast<size_t>(local) * N + j;
                            const int index = allIndices[offset];
                            if (index >= 0)
                            {
                                selection->add(index, gpuScoring ? allScores[offset]
                                    : cpuBaseScore(buffers, local, allRecords[index], featureReader->get(index), 0));
                            }
                        }
                        selectExperimental(ti);
                        continue;
                    }
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
                        double s = gpuScoring ? allScores[size_t(local) * N + j]
                            : cpuBaseScore(buffers, local, r, featureReader->get(li), cfg.usePenalty);
                        BenchmarkTimer neighborTimer(cfg.benchmark, metrics.msNeighbor);
                        neighborhood.applyPenalty(s, li, ti);
                        scored.push_back({s, li});
                    }
                    if (cfg.benchmark)
                    {
                        // 候选评分包含原有邻域惩罚；排序和最终选图另计。
                        metrics.msCPUScore += Ms(Clock::now() - scoreStart).count();
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
                        metrics.msSort += Ms(Clock::now() - sortStart).count();
                    }
                    thread_local std::mt19937 rng2(std::random_device{}());
                    int pickIdx =
                        scored[std::uniform_int_distribution<int>(0, topN - 1)(m_fixedSeed ? seededRng : rng2)].second;
                    bestLibIdx[ti] = pickIdx;
                    if (cfg.analyze)
                    {
                        BenchmarkTimer analysisTimer(cfg.benchmark, metrics.msAnalysis);
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
                    neighborhood.recordSelection(pickIdx, ti);
                }

                metrics.msSelect += Ms(Clock::now() - selectionStart).count();
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
            metrics.matchingPeakBytes = std::max(metrics.matchingPeakBytes, selectionBytes + buffers.capacityBytes() + ahead.capacityBytes() +
                                                                temporary + pinnedBytes + compatible.hostBytes() +
                                                                idxs.capacity() * sizeof(int) +
                                                                scored.capacity() * sizeof(std::pair<double, int>) +
                                                                tileVector.capacity() * sizeof(float));
            first += count;
            runtime.advance("matching", first, totalTiles, "tiles", first == totalTiles);
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
            metrics.featureReadStats = featureReader->stats();
        }
        if (compatibleFeatures)
        {
            std::cout << "\n  Generic CUDA: " << compatibleTimings.images << " images, "
                      << compatibleTimings.batches << " batches; host/device buffers "
                      << compatible.hostBytes() << "/" << compatible.deviceBytes() << " bytes; preprocess "
                      << compatibleTimings.prepareMs << " ms, CPU completion " << compatibleTimings.cpuMs
                      << " ms, device upload/compute/download " << compatibleTimings.gpu.eventUploadMs << "/"
                      << compatibleTimings.gpu.eventComputeMs << "/" << compatibleTimings.gpu.eventDownloadMs
                      << " ms" << std::endl;
        }
        if (backends.scoring == ScoreBackend::CompatibleCuda)
        {
            std::cout << "\n  Compatible CUDA scoring: " << m_compatibleScoreTiles << " tiles; library "
                      << compatibleScore.libraryBytes() << " bytes, combined device temporary "
                      << m_compatibleTemporaryBytes << " bytes; library upload " << metrics.libraryTimings.uploadMs
                      << " ms, score packing/copy " << metrics.scoreTimings.uploadMs + metrics.scoreTimings.downloadMs
                      << " ms, device upload/kernel/download " << metrics.scoreTimings.eventUploadMs << "/"
                      << metrics.scoreTimings.eventComputeMs << "/" << metrics.scoreTimings.eventDownloadMs << " ms"
                      << std::endl;
        }
        if (!hasCandidates)
        {
            {
                std::ostringstream detail;
                detail << "No candidates within --l-range; increase the brightness range.";
                runtime.fail("no_candidates", detail.str(), "lRange");
            }
            return false;
        }
    } // 索引、缓存、线程池及大批次缓冲在贴图/编码之前释放。
    metrics.matchingWallMs = Ms(Clock::now() - matchingStart).count();
    runtime.metric("selectionBufferBytes", selectionBytes);
    if (m_selectedObserver)
    {
        *m_selectedObserver = bestLibIdx;
    }
    if (m_matchOnly)
    {
        // 特征研究止于选图；不读取素材像素、不生成输出，也不累计图库使用次数。
        releaseGpuLib();
        const int selected = static_cast<int>(std::count_if(bestLibIdx.begin(), bestLibIdx.end(),
                                                          [](int index) { return index >= 0; }));
        runtime.output(tilesX, tilesY, "selection", selected, totalTiles - selected);
        runtime.metric("totalMs", Ms(Clock::now() - tStart).count());
        runtime.metric("matchingMs", metrics.matchingWallMs);
        runtime.metric("featureMs", metrics.msFeat);
        runtime.metric("annQueryMs", metrics.msANNQuery);
        runtime.metric("selectionMs", metrics.msSelect);
        runtime.metric("matchingBufferPeakBytes", metrics.matchingPeakBytes);
        runtime.metric("featureCachePeakBytes", metrics.featureReadStats.peakBytes);
        runtime.metric("deviceTemporaryBytes", m_compatibleTemporaryBytes);
        runtime.stage("finalizing");
        return selected == totalTiles || runtime.fail("no_candidates", "some tiles have no eligible candidates");
    }
    std::cout << " done" << std::endl;
    if (templateCuda && cfg.adaptiveWeights)
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
    if (templateCuda && cfg.analyze && totalTiles > noCandidateCount)
    {
        std::cout << "  Grid 8x8 experiment: Top1 differ=" << top1Differ << "/" << totalTiles - noCandidateCount
                  << "\n";
    }
    cntGrid = cntTiny = cntEdge = cntLBP = totalTiles;
    if (noCandidateCount)
    {
        std::cout << "  " << noCandidateCount << " tiles had no candidates\n";
        runtime.emit("warning", "no_candidates", "some tiles had no candidates and retained the existing background",
                     "lRange", {}, noCandidateCount);
    }
    if (!cfg.analyze && cfg.overlayStrength == 0)
    {
        target.release();
    }

    releaseGpuLib();
    if (!writeIndexedMosaicOutput(outputConfig, outputPlan, allRecords, bestLibIdx,
                                  adjustColor, metrics.outputStats, target, &runtime))
    {
        return false;
    }
    matched = metrics.outputStats.matched;
    loadFail = metrics.outputStats.failed;
    metrics.msPlace = metrics.outputStats.placementMs;
    metrics.msEncode = metrics.outputStats.encodingMs;
    std::cout << "Mosaic saved: " << metrics.outputStats.path << " (" << matched << " / " << totalTiles
              << " tiles, loadFail=" << loadFail << ")" << std::endl;
    if (!metrics.outputStats.previewPath.empty())
    {
        std::cout << "Preview saved: " << metrics.outputStats.previewPath << std::endl;
    }
    if (metrics.outputStats.failed > 0)
    {
        runtime.emit("warning", "output_tile_missing",
                     "Some output tiles could not be loaded; background was retained.", {}, {},
                     metrics.outputStats.failed);
    }
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
        BenchmarkTimer analysisTimer(cfg.benchmark, metrics.msAnalysis);
        runAnalysis(metrics.outputStats.path);
    }

    releaseGpuLib();

#ifdef _WIN32
#endif

    // 运行分析报告（如有 --analyze）
    if (cfg.benchmark)
    {
        const double totalMs = Ms(Clock::now() - tStart).count();
        detail::printMosaicBenchmark(metrics, {outputConfig, backends, totalTiles, N, m_compatibleTemporaryBytes, totalMs},
                                     "single");
        runtime.metric("totalMs", totalMs);
        runtime.metric("matchingMs", metrics.matchingWallMs);
        runtime.metric("featureMs", metrics.msFeat);
        runtime.metric("annQueryMs", metrics.msANNQuery);
        runtime.metric("selectionMs", metrics.msSelect);
        runtime.metric("placementMs", metrics.msPlace);
        runtime.metric("encodingMs", metrics.msEncode);
        runtime.metric("analysisMs", metrics.msAnalysis);
        runtime.metric("matchingBufferPeakBytes", metrics.matchingPeakBytes);
        runtime.metric("featureCachePeakBytes", metrics.featureReadStats.peakBytes);
        runtime.metric("deviceTemporaryBytes", m_compatibleTemporaryBytes);
    }
    runtime.stage("finalizing");
    return true;
}

} // namespace mosaicraft
