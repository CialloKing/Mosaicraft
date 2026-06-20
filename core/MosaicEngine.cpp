#include "MosaicEngine.h"
#include "BigTiffWriter.h"
#include "Database.h"
#include "DeepZoomWriter.h"
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
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace mosaicraft
{

// ============================================================
// 特征缓存
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
// 局部颜色校正：随机微调饱和度与亮度，减少马赛克重复感
// 在 HSV 空间操作，H 通道不变，S/V 通道在 [1-strength, 1+strength] 范围内随机缩放
// ============================================================
// 局部颜色微调：在 LAB 空间仅微调 L（亮度）通道
// LAB 感知均匀，仅调 L 不碰 AB → 颜色不变、无摩尔纹
// L 范围：[-strength, +strength] 偏正（避免变暗感知）
static void adjustColor(cv::Mat& img, double strength)
{
    cv::Mat lab;
    cv::cvtColor(img, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels(3);
    cv::split(lab, channels);
    // channels[0]=L, [1]=A, [2]=B
    // L 因子：[-s, +s] 偏正，仅调亮度
    double lFactor = 1.0 + ((rand() % 1001 - 300) / 1000.0) * strength;
    channels[0] = channels[0] * lFactor;
    cv::merge(channels, lab);
    cv::cvtColor(lab, img, cv::COLOR_Lab2BGR);
}

// ============================================================
// 生成
// ============================================================
bool MosaicEngine::generate(const std::string& targetPath,
                             const std::string& dbPath,
                             const std::string& outputPath,
                             const Config& config)
{
    // 运行时检测 CUDA；无 GPU 则静默退化
    Config cfg = config;
    if (cfg.useGpu && !cuda::isCudaAvailable())
    {
        cfg.useGpu = false;
    }

    // Benchmark 计时
    using Clock = std::chrono::steady_clock;
    using Ms = std::chrono::duration<double, std::milli>;
    auto tStart = Clock::now();
    auto tLast  = tStart;
    double msFeat = 0, msANNBuild = 0, msGPUScore = 0, msSelect = 0, msPlace = 0;
    double msPrep = 0;  // DB加载 + GPU library构建（仅GPU路径）

    // 特征提取操作级 profile（纳秒精度的原子累加器）
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

    // 指定输出尺寸时，先缩放目标图（仅改变 tile 数量，输出 tile 始终原生分辨率）
    if (cfg.outW > 0 && cfg.outH > 0)
    {
        cv::Mat resized;
        cv::resize(target, resized, cv::Size(cfg.outW, cfg.outH), 0, 0, cv::INTER_AREA);
        target = resized;
        std::cout << "Target resized to: " << cfg.outW << "x" << cfg.outH << std::endl;
    }

    // --upscale：放大原图获取更多 tile（同输出分辨率，更高密度）
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

    // 提取特征目录（供 FeaturePack / ANN 持久化使用）
    std::string featDirCache;
    auto allRecords = db.allRecords();  // 全库记录，在 GPU 路径中按索引取
    dbCount = static_cast<int>(allRecords.size());

    // 从首条记录提取特征目录
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

    // ——— 图库特征常驻 GPU（多线程并行加载 tiny/LBP 文件） ———
    cuda::GpuLibrary gpuLib;
    if (cfg.useGpu && cuda::isCudaAvailable())
    {
        std::vector<double>  h_lab(dbCount * 3);
        std::vector<float>   h_grid(dbCount * 192);
        std::vector<uint8_t> h_tiny(dbCount * 256);
        std::vector<double>  h_edge(dbCount);
        std::vector<float>   h_lbp(dbCount * 256);
        std::vector<int>     h_use(dbCount);

        // 打包标量特征（无需 I/O，单线程即可）
        for (int i = 0; i < dbCount; ++i)
        {
            const auto& rec = allRecords[i];
            h_lab[i*3+0] = rec.avgL; h_lab[i*3+1] = rec.avgA; h_lab[i*3+2] = rec.avgB;
            if (rec.grid4x4.size() == 192)
                std::memcpy(&h_grid[i*192], rec.grid4x4.data(), 192*sizeof(float));
            h_edge[i] = rec.edgeDensity;
            h_use[i] = rec.useCount;
        }

        // 尝试加载二进制特征缓存（tiny.bin + lbp.bin）
        // 缓存有效时用 2 次 fread 替代 50K 次文件 I/O
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
            // 缓存不存在或失效 → 回退到多线程逐文件读取
            std::cout << "  (feature cache miss, reading individual files)" << std::endl;
            int nUploadThreads = std::thread::hardware_concurrency();
            if (nUploadThreads < 2) nUploadThreads = 2;
            if (nUploadThreads > 16) nUploadThreads = 16;  // 磁盘 I/O 线程过多反而退化
            std::vector<std::thread> uploadWorkers;
            for (int t = 0; t < nUploadThreads; ++t)
            {
                uploadWorkers.emplace_back([&, t]() {
                    FeatureCache cache;  // 每个线程独立缓存，避免加锁
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

            // 回退完成，顺带构建二进制缓存（下次启动即可命中）
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

    // 向上取整分 tile，边缘镜像填充补全
    int tilesX = (target.cols + cfg.tileW - 1) / cfg.tileW;
    int tilesY = (target.rows + cfg.tileH - 1) / cfg.tileH;

    // 输出 tile 使用原生分辨率（180×320）
    // 单图模式下超 65500px 检查
    int outTileW = cfg.nativeTileW;
    int outTileH = cfg.nativeTileH;
    const int MAX_DIM = 65500;
    if (!cfg.tiledOutput && (tilesX * outTileW > MAX_DIM || tilesY * outTileH > MAX_DIM))
    {
        if (cfg.outputFormat == "jpg" && cfg.formatExplicit)
        {
            // 显式指定 jpg 超限 → 等比缩放输出 tile 至安全范围
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
            // 未显式指定格式，默认 jpg 超限 → 自动切 tiff
            cfg.outputFormat = "tiff";
            std::cout << "  (auto-switched to TIFF: output exceeds JPEG 65500px limit)" << std::endl;
        }
        else if (cfg.outputFormat != "tiff")
        {
            // 非 jpg 非 tiff 超限 → 自动切 tiled
            cfg.tiledOutput = true;
            std::cout << "  (auto-switched to tiled: output exceeds 65500px encoder limit)" << std::endl;
        }
    }

    // WebP 限制 16383px — 等比缩放，与显式 JPG 逻辑一致
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

    int outW = tilesX * outTileW;
    int outH = tilesY * outTileH;

    // 边缘补全（按目标 tile 尺寸算 pad，不是输出 tile 尺寸）
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

    // ——— 多线程预计算所有 tile 特征 ———
    int totalTiles = tilesX * tilesY;

    // --analyze: 匹配质量分析数据容器
    std::vector<double> analyzeScores;
    std::vector<int>    analyzeImageIds;
    std::vector<double> analyzeLabD, analyzeGridD, analyzeEdgeD;
    std::vector<double> analyzeGaps;      // winner-runnerUp 分数差
    std::vector<int>    analyzeRanks;     // winner 在候选排序中的位置(1-based)
    std::vector<int>    analyzeAnnRanks;  // winner 在 ANN Top200 中的位置(0=最优)
    std::vector<int>    analyzeCat;       // 0=Smooth, 1=Edge, 2=Texture, 3=Normal
    double analyzeGridCellSum[64] = {0};   // 每个 cell 的距离累计（用于贡献分析）

    int N = cfg.candidates;  // 候选数（GPU 路径下用于 benchmark）

    // Benchmark 报告 lambda（在结束时调用；必须在 totalTiles/N 之后定义）
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

    // Phase D 降采样 4×4 对比用（8×8→4×4）

    int nThreads = std::thread::hardware_concurrency();
    if (nThreads < 2) nThreads = 2;

    // 准备阶段计时：DB加载、GPU library构建到此结束
    auto tPreFeat = Clock::now();
    msPrep = Ms(tPreFeat - tLast).count();
    tLast = tPreFeat;

    std::atomic<int> featDone{0};
    std::vector<std::thread> featWorkers;
    for (int t = 0; t < nThreads; ++t) {
        featWorkers.emplace_back([&, t]() {
            using Ns = std::chrono::nanoseconds;
            for (int idx = t; idx < totalTiles; idx += nThreads) {
                int ty = idx / tilesX, tx = idx % tilesX;
                cv::Mat roi = target(cv::Rect(tx*cfg.tileW, ty*cfg.tileH, cfg.tileW, cfg.tileH));
                // 将 ROI 缩放到图库原生分辨率，确保特征与库图片同尺度可比
                cv::Mat roiNative;
                auto t0 = Clock::now();
                cv::resize(roi, roiNative, cv::Size(180, 320), 0, 0, cv::INTER_LINEAR);
                auto t1 = Clock::now(); opResizeNs += std::chrono::duration_cast<Ns>(t1 - t0).count();
                cv::Mat lab; cv::cvtColor(roiNative, lab, cv::COLOR_BGR2Lab);
                cv::Scalar m = cv::mean(lab);
                allTL[idx]=m[0]; allTA[idx]=m[1]; allTB[idx]=m[2];
                auto t2 = Clock::now(); opLabNs += std::chrono::duration_cast<Ns>(t2 - t1).count();
                allGrid[idx] = computeGrid8x8(roiNative);  // 8×8 Grid (192维)
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

    // Phase 0 计时
    auto tFeat = Clock::now();
    msFeat = Ms(tFeat - tLast).count();
    tLast = tFeat;

    int matched = 0;
    int loadFail = 0;
    int cntGrid = 0, cntMissGrid = 0;
    int cntTiny = 0, cntMissTiny = 0;
    int cntEdge = 0, cntMissEdge = 0;
    int cntLBP  = 0, cntMissLBP  = 0;

    // 邻域窗口自动：至少覆盖 2 行 tile（垂直邻域）和默认 300（水平邻域）
    if (cfg.neighborWindow <= 0)
        cfg.neighborWindow = std::max(300, tilesX * 2);

    // 滑动窗口 + 频率计数：允许少量重用但阻止聚类
    std::deque<int> recentIds;
    std::unordered_map<int, int> freqInWindow;

    // 权重归一化（所有 tile 共用）
    double wSum = cfg.labWeight + cfg.gridWeight + cfg.tinyWeight;
    if (cfg.edgeWeight > 0) wSum += cfg.edgeWeight;
    if (cfg.lbpWeight > 0)  wSum += cfg.lbpWeight;
    double nLabW = cfg.labWeight / wSum;
    double nGridW = cfg.gridWeight / wSum;
    double nTinyW = cfg.tinyWeight / wSum;
    double nEdgeW = cfg.edgeWeight / wSum;
    double nLbpW  = cfg.lbpWeight / wSum;
    N = cfg.candidates;

    // 每个 tile 最终选中的记录（GPU 路径预存，CPU 路径内联处理）
    std::vector<ImageRecord> bestRecords(totalTiles);
    std::vector<int> bestLibIdx(totalTiles, -1);

    // 单图输出时的大 Mat（分块模式不需要，声明在此以便跨分支使用）
    cv::Mat output;

    if (cfg.useGpu && gpuLib.count > 0)
    {
        // ════════════════════════════════════════════════════════
        // GPU 批量流水线：SQLite 预查 → 一次 GPU → 顺序选择 → 多线程贴图
        // ════════════════════════════════════════════════════════

        // —— Phase A: ANN 近似最近邻索引 ——
        // 优先加载持久化索引（build 时保存），不存在则构建并保存
        FeatureIndex annIndex;
        std::string annPath = featDirCache.empty() ? "lib.ann"
                             : (featDirCache + "/lib.ann");
        bool annLoaded = false;
        if (!featDirCache.empty())
        {
            std::cout << "  loading ANN index..." << std::flush;
            annLoaded = annIndex.load(annPath, 564, allRecords);
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
        }
        std::cout << " done" << std::endl;

        // Phase A 计时（ANN 构建 + 查询）
        auto tANN = Clock::now();
        msANNBuild = Ms(tANN - tLast).count();
        tLast = tANN;

        // —— Phase B: 扁平化 tile 特征（GPU 需要连续内存布局） ——
        std::vector<float>   flatGrid(totalTiles * 192);
        std::vector<uint8_t> flatTiny(totalTiles * 256);
        std::vector<float>   flatLBP(totalTiles * 256);
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            std::memcpy(&flatGrid[ti * 192], allGrid[ti].data(), 192 * sizeof(float));
            std::memcpy(&flatTiny[ti * 256], allTiny[ti].data(), 256);
            std::memcpy(&flatLBP[ti * 256], allLBP[ti].data(), 256 * sizeof(float));
        }

        // —— Phase C: 批量 GPU 评分 ——
        // 自适应权重：根据 tile 内容选择三档预设（实验选项 --adaptive-weights）
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
                    // Smooth: 提 LAB 颜色，保持 Grid（天空渐变需要空间结构）
                    tileLabW[ti] = 0.25;
                    tileGridW[ti] = 0.45;
                    tileTinyW[ti] = 0.20;
                    tileEdgeW[ti] = 0.05;
                    tileLbpW[ti] = 0.05;
                    cntSmooth++;
                }
                else if (e > 0.01)
                {
                    // Edge-heavy: 轮廓结构 > 颜色（阈值 0.01 适配 9×16 小 ROI）
                    tileLabW[ti] = 0.15;
                    tileGridW[ti] = 0.40;
                    tileTinyW[ti] = 0.25;
                    tileEdgeW[ti] = 0.15;
                    tileLbpW[ti] = 0.05;
                    cntEdge++;
                }
                else if (lbpEnt > 3.0)
                {
                    // Texture-heavy: 纹理 > 颜色
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
            // 收集分布统计以校准阈值
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

        // Phase C 计时
        auto tGPU = Clock::now();
        msGPUScore = Ms(tGPU - tLast).count();
        tLast = tGPU;

        // —— Phase D: 顺序选择 + 8×8 vs 降采样4×4 对比 ——
        // 当前 DB 存 8×8 Grid；对比时将 tile 和库图都降采样到 4×4 模拟旧版
        std::vector<std::vector<float>> libGrid4x4(dbCount);  // 8×8→4×4 降采样
        std::vector<std::vector<float>> tileGrid4x4(totalTiles);
        for (int i = 0; i < dbCount; ++i)
        {
            // 8×8 降采样到 4×4：每 2×2 cell 取平均
            tileGrid4x4_status: ;  // 用到时再算
        }
        // 预计算库图 4×4
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
        // 预计算 tile 4×4
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
        int noCandidateCount = 0;  // 诊断：统计无候选的 tile
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            double* scores = &allScores[ti * N];
            const int* indices = &allIndices[ti * N];
            // 统计有效候选数（排除 -1 填充）
            int validCount = 0;
            for (int j = 0; j < N; ++j)
                if (indices[j] >= 0) validCount++;
            if (validCount == 0)
            {
                noCandidateCount++;
                continue;
            }
            // 频率分级惩罚：1次轻罚(允许并排)、2次中罚、3+次重罚(防聚类)
            for (int j = 0; j < N; ++j)
            {
                int libIdx = indices[j];
                if (libIdx < 0) continue;
                int imgId = allRecords[libIdx].id;
                auto it = freqInWindow.find(imgId);
                int cnt = (it != freqInWindow.end()) ? it->second : 0;
                if (cnt >= 3)      { scores[j] += cfg.neighborPenalty; }
                else if (cnt == 2) { scores[j] += cfg.neighborPenalty * 0.4; }
                else if (cnt == 1) { scores[j] += cfg.neighborPenalty * 0.1; }
            }
            // Top-N 随机选择（topN 不超过有效候选数）
            // —— 8×8 vs 降采样4×4 对比 ——
            if (validCount > 0)
            {
                double best4 = 1e30, best8 = 1e30;
                int best4idx = -1, best8idx = -1;
                for (int j = 0; j < N; ++j)
                {
                    if (indices[j] < 0) continue;
                    // GPU scores 已含 8×8 grid；4×4 分数 = 减去 8×8 贡献 + 4×4 贡献
                    double grid8d = gridDistance8x8(allGrid[ti], allRecords[indices[j]].grid4x4);
                    double grid4d = gridDistance(tileGrid4x4[ti], libGrid4x4[indices[j]]);
                    double score4 = scores[j] - nGridW * grid8d + nGridW * grid4d;
                    double score8 = scores[j];  // GPU 已用 8×8
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
            int rankPos = rand() % topN;       // 选中位置 0-based，即 rank-1
            int pick = idxs[rankPos];
            int chosenLibIdx = indices[pick];
            bestLibIdx[ti] = chosenLibIdx;
            bestRecords[ti] = allRecords[chosenLibIdx];
            // --analyze: 记录选中 tile 的特征距离（不含邻域惩罚，纯匹配质量）
            if (cfg.analyze)
            {
                const auto& rec = allRecords[chosenLibIdx];
                double labD  = labDistance(allTL[ti], allTA[ti], allTB[ti], rec.avgL, rec.avgA, rec.avgB);
                double gridD = gridDistance8x8(allGrid[ti], rec.grid4x4);
                double edgeD = std::abs(allEdge[ti] - rec.edgeDensity);
                double totalW = cfg.labWeight + cfg.gridWeight + cfg.tinyWeight + cfg.edgeWeight + cfg.lbpWeight;
                double featScore = (cfg.labWeight*labD + cfg.gridWeight*gridD + cfg.edgeWeight*edgeD) / totalW;
                analyzeScores.push_back(featScore);
                analyzeImageIds.push_back(rec.id);
                analyzeLabD.push_back(labD);
                analyzeGridD.push_back(gridD);
                analyzeEdgeD.push_back(edgeD);

                // Top-K Gap: winner vs true best（含惩罚的原始分数差）
                double winnerScore = scores[pick];
                double gap = 0.0;
                if (validCount >= 2)
                {
                    if (rankPos == 0)  // winner 即最优
                        gap = scores[idxs[1]] - winnerScore;
                    else               // 最优未被选中
                        gap = scores[idxs[0]] - winnerScore;  // 负值=winner更差
                }
                analyzeGaps.push_back(gap);
                analyzeRanks.push_back(rankPos + 1);  // 1-based rank in sorted Top-N

                // ANN rank: winner 在 ANN 查询结果中的位置 (0=最优)
                int annRank = -1;
                for (int j = 0; j < N; ++j)
                {
                    if (allIndices[ti * N + j] == chosenLibIdx) { annRank = j; break; }
                }
                analyzeAnnRanks.push_back(annRank);

                // 分类：与自适应权重相同的统计量
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

                // Grid 8×8 每 cell 贡献：累加选中对的 cell LAB 距离
                for (int ci = 0; ci < 64; ++ci)
                {
                    int off = ci * 3;
                    double dl = allGrid[ti][off] / 255.0 - rec.grid4x4[off] / 255.0;
                    double da = allGrid[ti][off+1] / 255.0 - rec.grid4x4[off+1] / 255.0;
                    double db = allGrid[ti][off+2] / 255.0 - rec.grid4x4[off+2] / 255.0;
                    analyzeGridCellSum[ci] += std::sqrt(dl*dl + da*da + db*db);
                }
            }
            // 维护滑动窗口和频率计数
            int chosenId = bestRecords[ti].id;
            recentIds.push_back(chosenId);
            freqInWindow[chosenId]++;
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
        // 8×8 Grid 对比统计
        if (totalTiles > 0)
        {
            int validTiles = totalTiles - noCandidateCount;
            if (validTiles > 0)
            {
                std::cout << "\n  Grid 8x8 experiment: "
                          << "Top1 differ=" << top1Differ << "/" << validTiles
                          << " (" << std::fixed << std::setprecision(1)
                          << (100.0 * top1Differ / validTiles) << "%)";
            }
        }
        std::cout << " done" << std::endl;

        // Phase D 计时
        auto tSelect = Clock::now();
        msSelect = Ms(tSelect - tLast).count();
        tLast = tSelect;

        // —— Phase E: 贴图 ——
        int nThreads = std::thread::hardware_concurrency();
        if (nThreads < 2) nThreads = 2;

        if (cfg.tiledOutput)
        {
            // 分块输出：每 tile 独立文件，无尺寸限制，无需大 Mat
            std::error_code ec;
            std::string level0Dir = outputPath + "_files/0";
            std::filesystem::create_directories(level0Dir, ec);
            std::cout << "  writing tiles (" << nThreads << " threads)..."
                      << std::flush;
            std::atomic<int> tileDone{0};
            std::atomic<int> tileFail{0};
            std::vector<std::thread> tileWorkers;
            ImageCache imgCache;  // 线程安全缓存
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
                        // DZI 格式: {name}_files/{level}/{col}_{row}.jpg
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
            if (gpuLib.count > 0) cuda::freeLibrary(gpuLib);

            if (cfg.deepZoom)
            {
                std::cout << "  building pyramid levels..." << std::endl;
                DeepZoomWriter::buildPyramid(level0Dir, outTileW, outTileH,
                                             tilesX, tilesY, cfg.jpegQuality);
            }

            // 贴图计时
            msPlace = Ms(Clock::now() - tLast).count();
            printBenchmark("tiled");
            return true;
        }

        // 单图输出
        output = cv::Mat(outH, outW, CV_8UC3, cv::Scalar(64, 64, 64));
        std::cout << "  placing tiles (" << nThreads << " threads)..."
                  << std::flush;
        std::atomic<int> placeDone{0};
        std::atomic<int> placeFail{0};
        std::atomic<int> placeNoCand{0};  // 无效候选导致的失败
        std::atomic<int> placeLoadErr{0}; // 文件读取失败
        std::vector<std::thread> placeWorkers;
        ImageCache imgCache;  // 线程安全缓存，避免重复 imread
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
                    // 每个线程写不重叠的 ROI，无需加锁
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
        // ════════════════════════════════════════════════════════
        // CPU 路径（逐 tile 顺序处理，保留原有逻辑）
        // ════════════════════════════════════════════════════════
        FeatureCache cache;
        output = cv::Mat(outH, outW, CV_8UC3, cv::Scalar(64, 64, 64));

        for (int ty = 0; ty < tilesY; ++ty)
        {
            for (int tx = 0; tx < tilesX; ++tx)
            {
                int ti = ty * tilesX + tx;
                double tL = allTL[ti], tA = allTA[ti], tB = allTB[ti];
                const auto& tileGrid = allGrid[ti];
                const auto& tileTiny = allTiny[ti];
                double tileEdge = allEdge[ti];
                const auto& tileLBP = allLBP[ti];

                bool uGrid = true, uTiny = true, uEdge = true, uLBP = true;

                auto candidates = db.queryByLRange(tL - cfg.lRange,
                                                    tL + cfg.lRange,
                                                    cfg.candidates);
                if (candidates.empty()) { continue; }
                const int nC = static_cast<int>(candidates.size());
                int bestIdx = 0;
                double bestScore = 1e30;
                for (int i = 0; i < nC; ++i)
                {
                    const auto& rec = candidates[i];
                    double labD = labDistance(tL, tA, tB, rec.avgL, rec.avgA, rec.avgB);
                    double gridD = gridDistance(tileGrid, rec.grid4x4);
                    bool hasGrid = (gridD < 1e5);
                    double tinyD = 1.0; bool hasTiny = false;
                    if (!rec.tinyPath.empty()) {
                        auto* td = cache.loadTiny(rec.id, rec.tinyPath);
                        if (td) { tinyD = tinyMSE(tileTiny, *td); hasTiny = true; }
                    }
                    double edgeD = std::abs(tileEdge - rec.edgeDensity);
                    bool hasEdge = true;
                    double lbpD = 1.0; bool hasLBP = false;
                    if (!rec.histPath.empty()) {
                        auto* ld = cache.loadLBP(rec.id, rec.histPath);
                        if (ld) { lbpD = lbpDistance(tileLBP, *ld); hasLBP = true; }
                    }
                    double wSumCs = cfg.labWeight;
                    if (hasGrid) wSumCs += cfg.gridWeight;
                    if (hasTiny) wSumCs += cfg.tinyWeight;
                    if (hasEdge) wSumCs += cfg.edgeWeight;
                    if (hasLBP)  wSumCs += cfg.lbpWeight;
                    double score = (cfg.labWeight/wSumCs)*labD;
                    if (hasGrid) score += (cfg.gridWeight/wSumCs)*gridD;
                    if (hasTiny) score += (cfg.tinyWeight/wSumCs)*tinyD;
                    if (hasEdge) score += (cfg.edgeWeight/wSumCs)*edgeD;
                    if (hasLBP)  score += (cfg.lbpWeight/wSumCs)*lbpD;
                    score += rec.useCount * cfg.usePenalty;
                    if (score < bestScore) {
                        bestScore = score; bestIdx = i;
                        uGrid = hasGrid; uTiny = hasTiny;
                        uEdge = hasEdge; uLBP = hasLBP;
                    }
                }

                if (uGrid) cntGrid++; else cntMissGrid++;
                if (uTiny) cntTiny++; else cntMissTiny++;
                if (uEdge) cntEdge++; else cntMissEdge++;
                if (uLBP)  cntLBP++;  else cntMissLBP++;

                const auto& bestRec = candidates[static_cast<std::size_t>(bestIdx)];
                cv::Mat matchImg = imreadUnicode(bestRec.filePath, cv::IMREAD_COLOR);
                if (matchImg.empty()) { loadFail++; continue; }

                cv::Mat resized;
                cv::resize(matchImg, resized, cv::Size(outTileW, outTileH),
                           0, 0, cv::INTER_AREA);
                if (cfg.colorAdjust) { adjustColor(resized, cfg.colorStrength); }
                resized.copyTo(output(cv::Rect(tx * outTileW, ty * outTileH,
                                              outTileW, outTileH)));
                matched++;

                // 每 100 个 tile 或每行末尾输出进度
                if (tx == tilesX - 1 || ti % 100 == 0)
                {
                    int done = ti + 1;
                    std::cout << "\r  " << done << " / " << totalTiles
                              << "  (" << (100 * done / totalTiles) << "%)"
                              << std::flush;
                }
            }
        }
    }

    std::cout << std::endl;

    // 检测输出格式
    std::string fmt = cfg.outputFormat;
    if (fmt == "jpg" || fmt.empty())
    {
        // 尝试从 outputPath 扩展名推断
        auto dotPos = outputPath.rfind('.');
        if (dotPos != std::string::npos)
        {
            std::string ext = outputPath.substr(dotPos + 1);
            if (ext == "png" || ext == "PNG") fmt = "png";
            else if (ext == "webp" || ext == "WEBP") fmt = "webp";
            else if (ext == "tiff" || ext == "tif" || ext == "TIFF" || ext == "TIF") fmt = "tiff";
        }
    }
    if (fmt != "jpg" && fmt != "png" && fmt != "webp" && fmt != "tiff") fmt = "jpg";

    // 写入输出
    if (fmt == "tiff")
    {
        // BigTiffWriter 内部逐行 BGR→RGB，无需全图 cvtColor
        BigTiffWriter tiff(outputPath, outW, outH);
        if (!tiff.writeMat(output.data, static_cast<int>(output.step)))
        {
            std::cerr << "ERROR: BigTiffWriter failed" << std::endl;
            return false;
        }
        tiff.close();
    }
    else
    {
        std::vector<int> writeParams;
        if (fmt == "jpg")
            writeParams = {cv::IMWRITE_JPEG_QUALITY, cfg.jpegQuality};
        else if (fmt == "png")
            writeParams = {cv::IMWRITE_PNG_COMPRESSION, 3};
        else if (fmt == "webp")
            writeParams = {cv::IMWRITE_WEBP_QUALITY, cfg.jpegQuality};

        if (!imwriteUnicode(outputPath, output, writeParams))
        {
            std::cerr << "ERROR: Cannot write output: " << outputPath << std::endl;
            return false;
        }
    }

    std::cout << "Mosaic saved: " << outputPath
              << "  (" << matched << " / " << totalTiles << " tiles"
              << (loadFail > 0 ? ", loadFail=" + std::to_string(loadFail) : "")
              << ")"
              << std::endl;
    std::cout << "  Features used:"
              << " grid=" << cntGrid << "/" << (cntGrid + cntMissGrid)
              << " tiny=" << cntTiny << "/" << (cntTiny + cntMissTiny)
              << " edge=" << cntEdge << "/" << (cntEdge + cntMissEdge)
              << " lbp=" << cntLBP << "/" << (cntLBP + cntMissLBP)
              << std::endl;

    // —— 匹配质量分析报告 ——
    if (cfg.analyze && !analyzeScores.empty())
    {
        int n = static_cast<int>(analyzeScores.size());
        // 分数统计
        std::vector<double> sortedScores = analyzeScores;
        std::sort(sortedScores.begin(), sortedScores.end());
        double scoreMean = 0, scoreMin = 1e30, scoreMax = 0;
        for (double s : analyzeScores) { scoreMean += s; if (s < scoreMin) scoreMin = s; if (s > scoreMax) scoreMax = s; }
        scoreMean /= n;
        double scoreP50 = sortedScores[n/2];
        double scoreP90 = sortedScores[n*9/10];
        double scoreP99 = sortedScores[n*99/100];

        // 特征贡献（仅 LAB/Grid/Edge 可用内存数据）
        double labSum = 0, gridSum = 0, edgeSum = 0;
        double labW = cfg.labWeight, gridW = cfg.gridWeight, edgeW = cfg.edgeWeight;
        // 归一化权重（与 scoring 一致）
        double totalW = labW + gridW + cfg.tinyWeight + edgeW + cfg.lbpWeight;
        labW /= totalW; gridW /= totalW; edgeW /= totalW;
        for (int i = 0; i < n; ++i)
        {
            labSum  += labW  * analyzeLabD[i];
            gridSum += gridW * analyzeGridD[i];
            edgeSum += edgeW * analyzeEdgeD[i];
        }
        double contribTotal = labSum + gridSum + edgeSum;

        // 复用统计
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
        // Top-K Gap 统计
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
        // 候选排名分布（winner 在排序 Top-N 中的位置）
        if (!analyzeRanks.empty())
        {
            int rankBuckets[4] = {0, 0, 0, 0};  // 1, 2, 3, 4+
            for (int r : analyzeRanks)
            {
                if (r <= 3) rankBuckets[r-1]++;
                else rankBuckets[3]++;
            }
            double total = static_cast<double>(analyzeRanks.size());
            std::cout << "  Winner rank in TopN: #1=" << std::setprecision(1) << (rankBuckets[0]*100/total)
                      << "% #2=" << (rankBuckets[1]*100/total) << "% #3=" << (rankBuckets[2]*100/total) << "%\n";
        }
        // ANN 候选排名分布（winner 在 ANN 200 候选中的位置）
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
        // 分类分数
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
        // 最差匹配 tile 定位
        std::cout << "  Worst 5 tiles:\n";
        std::vector<std::pair<double,int>> worstIdx;
        for (int i = 0; i < n; ++i)
            worstIdx.push_back({analyzeScores[i], i});
        std::sort(worstIdx.rbegin(), worstIdx.rend());  // 降序：最差在前
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
        // Grid 8×8 cell 贡献分析（每 cell 的平均 LAB 距离，越小=越重要）
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
            }
        }
        std::cout << "  Reuse: unique=" << useCount.size() << "/" << n
                  << " ratio=" << std::setprecision(2) << (static_cast<double>(n)/useCount.size()) << "x\n";
        std::cout << "  Top 5 most used:\n";
        for (int i = 0; i < std::min(5, static_cast<int>(topUsed.size())); ++i)
            std::cout << "    id=" << topUsed[i].second << " : " << topUsed[i].first << " times\n";

        // 热力图
        std::string heatPath = outputPath;
        auto dotPos = heatPath.rfind('.');
        if (dotPos != std::string::npos)
            heatPath = heatPath.substr(0, dotPos) + "_heatmap.png";
        else
            heatPath += "_heatmap.png";

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
                double t = (s - sMin) / sRange;  // 0=best, 1=worst
                // 绿(好)→黄→红(差)
                cv::Vec3b color;
                if (t < 0.5)  // 绿→黄
                    color = cv::Vec3b(0, static_cast<uchar>(255*t*2), static_cast<uchar>(255*(1-t*2)));
                else          // 黄→红
                    color = cv::Vec3b(0, static_cast<uchar>(255*(1-(t-0.5)*2)), static_cast<uchar>(255));
                cv::rectangle(heat,
                    cv::Rect(tx*4, ty*4, 4, 4), color, cv::FILLED);
            }
        }
        cv::imwrite(heatPath, heat);
        std::cout << "  Heatmap: " << heatPath << "\n";
    }

    if (gpuLib.count > 0) cuda::freeLibrary(gpuLib);

    // 贴图计时
    msPlace = Ms(Clock::now() - tLast).count();
    printBenchmark("single");
    return true;
}

} // namespace mosaicraft
