#include "MosaicEngine.h"
#include "Database.h"
#include "DeepZoomWriter.h"
#include "FeatureIndex.h"
#include "FeaturePack.h"
#include "FeatureUtils.h"
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
static void adjustColor(cv::Mat& img, double strength)
{
    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    std::vector<cv::Mat> channels(3);
    cv::split(hsv, channels);
    // channels[0]=H, [1]=S, [2]=V
    double sFactor = 1.0 + (rand() % 2001 - 1000) / 1000.0 * strength;
    double vFactor = 1.0 + (rand() % 2001 - 1000) / 1000.0 * strength;
    channels[1] = channels[1] * sFactor;  // 饱和度
    channels[2] = channels[2] * vFactor;  // 亮度
    cv::merge(channels, hsv);
    cv::cvtColor(hsv, img, cv::COLOR_HSV2BGR);
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

    // ——— 加载全库记录（GPU 索引 → ImageRecord 映射用） ———
    auto allRecords = db.allRecords();  // 全库记录，在 GPU 路径中按索引取
    dbCount = static_cast<int>(allRecords.size());

    // ——— 图库特征常驻 GPU（多线程并行加载 tiny/LBP 文件） ———
    cuda::GpuLibrary gpuLib;
    if (cfg.useGpu && cuda::isCudaAvailable())
    {
        std::vector<double>  h_lab(dbCount * 3);
        std::vector<float>   h_grid(dbCount * 48);
        std::vector<uint8_t> h_tiny(dbCount * 256);
        std::vector<double>  h_edge(dbCount);
        std::vector<float>   h_lbp(dbCount * 256);
        std::vector<int>     h_use(dbCount);

        // 打包标量特征（无需 I/O，单线程即可）
        for (int i = 0; i < dbCount; ++i)
        {
            const auto& rec = allRecords[i];
            h_lab[i*3+0] = rec.avgL; h_lab[i*3+1] = rec.avgA; h_lab[i*3+2] = rec.avgB;
            if (rec.grid4x4.size() == 48)
                std::memcpy(&h_grid[i*48], rec.grid4x4.data(), 48*sizeof(float));
            h_edge[i] = rec.edgeDensity;
            h_use[i] = rec.useCount;
        }

        // 尝试加载二进制特征缓存（tiny.bin + lbp.bin）
        // 缓存有效时用 2 次 fread 替代 50K 次文件 I/O
        bool cacheLoaded = false;
        std::string featDirCache;  // 特征目录（从首条记录提取，回退时复用）
        if (!allRecords.empty() && !allRecords[0].tinyPath.empty())
        {
            // 从首条记录的 tinyPath 提取 featDir
            std::string firstTiny = allRecords[0].tinyPath;
            // tinyPath 格式: "normalized/features/000042.tiny"
            auto slashPos = firstTiny.rfind('/');
            auto backslashPos = firstTiny.rfind('\\');
            auto dirEnd = (slashPos != std::string::npos && backslashPos != std::string::npos)
                ? std::max(slashPos, backslashPos)
                : (slashPos != std::string::npos ? slashPos : backslashPos);
            if (dirEnd != std::string::npos)
            {
                featDirCache = firstTiny.substr(0, dirEnd);
                std::vector<int> recordIds;
                recordIds.reserve(dbCount);
                for (const auto& r : allRecords)
                    recordIds.push_back(r.id);
                cacheLoaded = FeaturePack::tryLoad(featDirCache, recordIds, h_tiny, h_lbp);
            }
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
                      << (dbCount * (48*4+256+256*4) / 1024) << " KB)" << std::endl;
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
    // 单图模式下若总尺寸超过 JPEG 65500px 限制则自动等比缩减
    // 分块模式下不缩放（每 tile 独立文件，无尺寸限制）
    int outTileW = cfg.nativeTileW;
    int outTileH = cfg.nativeTileH;
    const int MAX_DIM = 65500;
    if (!cfg.tiledOutput && (tilesX * outTileW > MAX_DIM || tilesY * outTileH > MAX_DIM))
    {
        double scaleW = (tilesX * outTileW > MAX_DIM) ? static_cast<double>(MAX_DIM) / (tilesX * outTileW) : 1.0;
        double scaleH = (tilesY * outTileH > MAX_DIM) ? static_cast<double>(MAX_DIM) / (tilesY * outTileH) : 1.0;
        double scale = std::min(scaleW, scaleH);
        outTileW = std::max(1, static_cast<int>(outTileW * scale));
        outTileH = std::max(1, static_cast<int>(outTileH * scale));
        std::cout << "  (auto-scaled tile " << outTileW << "x" << outTileH
                  << " to fit JPEG 65500px limit)" << std::endl;
    }

    int outW = tilesX * outTileW;
    int outH = tilesY * outTileH;

    // 单图模式下，若输出 Mat 超过 1.5 GB 则自动回退到分块输出
    const int64_t MAX_SINGLE_PIXELS = 1500LL * 1024 * 1024 / 3;  // ~524M pixels
    if (!cfg.tiledOutput && static_cast<int64_t>(outW) * outH > MAX_SINGLE_PIXELS)
    {
        std::cout << "  (auto-switching to tiled: output would be "
                  << outW << "x" << outH << ", "
                  << (static_cast<int64_t>(outW) * outH * 3 / (1024*1024)) << " MB)"
                  << std::endl;
        cfg.tiledOutput = true;
    }

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
                cv::resize(roi, roiNative, cv::Size(cfg.nativeTileW, cfg.nativeTileH), 0, 0, cv::INTER_LINEAR);
                auto t1 = Clock::now(); opResizeNs += std::chrono::duration_cast<Ns>(t1 - t0).count();
                cv::Mat lab; cv::cvtColor(roiNative, lab, cv::COLOR_BGR2Lab);
                cv::Scalar m = cv::mean(lab);
                allTL[idx]=m[0]; allTA[idx]=m[1]; allTB[idx]=m[2];
                auto t2 = Clock::now(); opLabNs += std::chrono::duration_cast<Ns>(t2 - t1).count();
                allGrid[idx] = computeGrid4x4(roiNative);
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
        // HNSW 图结构索引，O(log n) 查询，恒定 200 候选（不受图库规模影响）
        std::cout << "  building ANN index (" << dbCount << " images)..." << std::flush;
        FeatureIndex annIndex;
        annIndex.build(allRecords);
        std::cout << " done" << std::endl;

        std::cout << "  collecting candidates..." << std::flush;
        std::vector<int> allIndices(totalTiles * N, -1);
        std::vector<float> tileVec;
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            buildTileVector(allTL[ti], allTA[ti], allTB[ti],
                            allGrid[ti], allTiny[ti], allEdge[ti], allLBP[ti],
                            tileVec);
            auto ids = annIndex.query(tileVec.data(), N);
            int nc = static_cast<int>(ids.size());
            for (int j = 0; j < nc; ++j)
                allIndices[ti * N + j] = ids[j];  // 已是 0-based 库索引
        }
        std::cout << " done" << std::endl;

        // Phase A 计时（ANN 构建 + 查询）
        auto tANN = Clock::now();
        msANNBuild = Ms(tANN - tLast).count();
        tLast = tANN;

        // —— Phase B: 扁平化 tile 特征（GPU 需要连续内存布局） ——
        std::vector<float>   flatGrid(totalTiles * 48);
        std::vector<uint8_t> flatTiny(totalTiles * 256);
        std::vector<float>   flatLBP(totalTiles * 256);
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            std::memcpy(&flatGrid[ti * 48], allGrid[ti].data(), 48 * sizeof(float));
            std::memcpy(&flatTiny[ti * 256], allTiny[ti].data(), 256);
            std::memcpy(&flatLBP[ti * 256], allLBP[ti].data(), 256 * sizeof(float));
        }

        // —— Phase C: 批量 GPU 评分（一次 kernel，消除逐 tile 启动开销） ——
        std::cout << "  GPU scoring " << totalTiles << " x " << N << "..."
                  << std::flush;
        std::vector<double> allScores(totalTiles * N, 1e30);
        cuda::scoreBatch(
            totalTiles,
            allTL.data(), allTA.data(), allTB.data(),
            flatGrid.data(), flatTiny.data(), allEdge.data(), flatLBP.data(),
            allIndices.data(), N,
            gpuLib,
            nLabW, nGridW, nTinyW, nEdgeW, nLbpW, cfg.usePenalty,
            allScores.data());
        std::cout << " done" << std::endl;

        // Phase C 计时
        auto tGPU = Clock::now();
        msGPUScore = Ms(tGPU - tLast).count();
        tLast = tGPU;

        // —— Phase D: 顺序选择（邻域去重 + Top-N 随机，依赖历史状态不可并行） ——
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
            std::vector<int> idxs(N);
            for (int j = 0; j < N; ++j) idxs[j] = j;
            int topN = std::min(cfg.topNrandom, std::min(N, validCount));
            std::partial_sort(idxs.begin(), idxs.begin() + topN, idxs.end(),
                [&](int a, int b) { return scores[a] < scores[b]; });
            int pick = idxs[rand() % topN];
            int chosenLibIdx = indices[pick];
            bestLibIdx[ti] = chosenLibIdx;
            bestRecords[ti] = allRecords[chosenLibIdx];
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
            for (int t = 0; t < nThreads; ++t) {
                tileWorkers.emplace_back([&, t]() {
                    char fname[512];
                    for (int ti = t; ti < totalTiles; ti += nThreads) {
                        int libIdx = bestLibIdx[ti];
                        if (libIdx < 0) { tileFail++; continue; }
                        int ty = ti / tilesX, tx = ti % tilesX;
                        cv::Mat m = imreadUnicode(bestRecords[ti].filePath, cv::IMREAD_COLOR);
                        if (m.empty()) { tileFail++; continue; }
                        cv::Mat r;
                        cv::resize(m, r, cv::Size(outTileW, outTileH), 0, 0, cv::INTER_AREA);
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
        for (int t = 0; t < nThreads; ++t)
        {
            placeWorkers.emplace_back([&, t]() {
                // 每个线程直接读文件 + cv::resize，无锁
                for (int ti = t; ti < totalTiles; ti += nThreads)
                {
                    int libIdx = bestLibIdx[ti];
                    if (libIdx < 0) { placeNoCand++; placeFail++; continue; }
                    const auto& rec = bestRecords[ti];
                    int ty = ti / tilesX, tx = ti % tilesX;

                    cv::Mat matchImg = imreadUnicode(rec.filePath, cv::IMREAD_COLOR);
                    if (matchImg.empty())
                    {
                        placeLoadErr++; placeFail++;
                        continue;
                    }

                    cv::Mat resized;
                    cv::resize(matchImg, resized, cv::Size(outTileW, outTileH),
                               0, 0, cv::INTER_AREA);
                    if (cfg.colorAdjust) { adjustColor(resized, cfg.colorStrength); }
                    // 每个线程写不重叠的 ROI，无需加锁
                    resized.copyTo(output(cv::Rect(tx * outTileW, ty * outTileH,
                                                  outTileW, outTileH)));

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
        }
    }
    if (fmt != "jpg" && fmt != "png" && fmt != "webp") fmt = "jpg";

    // 构建 imwrite 参数
    std::vector<int> writeParams;
    if (fmt == "jpg")
        writeParams = {cv::IMWRITE_JPEG_QUALITY, cfg.jpegQuality};
    else if (fmt == "png")
        writeParams = {cv::IMWRITE_PNG_COMPRESSION, 3};  // 3 = 速度和大小平衡
    else if (fmt == "webp")
        writeParams = {cv::IMWRITE_WEBP_QUALITY, cfg.jpegQuality};

    if (!imwriteUnicode(outputPath, output, writeParams))
    {
        std::cerr << "ERROR: Cannot write output: " << outputPath << std::endl;
        return false;
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
    if (gpuLib.count > 0) cuda::freeLibrary(gpuLib);

    // 贴图计时
    msPlace = Ms(Clock::now() - tLast).count();
    printBenchmark("single");
    return true;
}

} // namespace mosaicraft
