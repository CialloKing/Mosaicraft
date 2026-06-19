#include "MosaicEngine.h"
#include "Database.h"
#include "UnicodeIO.h"
#include "compute/CudaBackend.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace mosaicraft
{

// ============================================================
// 工具
// ============================================================

static double labDistance(double l1, double a1, double b1,
                           double l2, double a2, double b2)
{
    double dl = l1 - l2;
    double da = a1 - a2;
    double db = b1 - b2;
    return std::sqrt(dl * dl + da * da + db * db) / 100.0;  // 归一化到 0~1，与其他特征同尺度
}

static std::vector<float> computeGrid4x4(const cv::Mat& bgr)
{
    const int gridRows = 4, gridCols = 4;
    const int cellH = bgr.rows / gridRows, cellW = bgr.cols / gridCols;
    std::vector<float> grid;
    grid.reserve(48);
    for (int r = 0; r < gridRows; ++r)
    {
        for (int c = 0; c < gridCols; ++c)
        {
            cv::Mat cell = bgr(cv::Rect(c * cellW, r * cellH, cellW, cellH));
            cv::Mat cellLab;
            cv::cvtColor(cell, cellLab, cv::COLOR_BGR2Lab);
            cv::Scalar m = cv::mean(cellLab);
            grid.push_back(static_cast<float>(m[0]));
            grid.push_back(static_cast<float>(m[1]));
            grid.push_back(static_cast<float>(m[2]));
        }
    }
    return grid;
}

static double gridDistance(const std::vector<float>& a,
                            const std::vector<float>& b)
{
    if (a.size() != 48 || b.size() != 48) { return 1e6; }
    double sum = 0.0;
    for (std::size_t i = 0; i < 16; ++i)
    {
        std::size_t idx = i * 3;
        double dl = a[idx] - b[idx];
        double da = a[idx + 1] - b[idx + 1];
        double db = a[idx + 2] - b[idx + 2];
        sum += std::sqrt(dl * dl + da * da + db * db);
    }
    return sum / 16.0 / 100.0;  // 归一化到 0~1
}

static std::vector<uint8_t> computeTinyImage(const cv::Mat& bgr)
{
    cv::Mat gray, tiny;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::resize(gray, tiny, cv::Size(16, 16), 0, 0, cv::INTER_AREA);
    std::vector<uint8_t> result(256);
    std::memcpy(result.data(), tiny.data, 256);
    return result;
}

static double tinyMSE(const std::vector<uint8_t>& a,
                       const std::vector<uint8_t>& b)
{
    if (a.size() != 256 || b.size() != 256) { return 1.0; }
    double sum = 0.0;
    for (std::size_t i = 0; i < 256; ++i)
    {
        double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sum += diff * diff;
    }
    return sum / (256.0 * 255.0 * 255.0);
}

static double computeEdgeDensity(const cv::Mat& bgr)
{
    cv::Mat gray, edges;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::Canny(gray, edges, 60, 120);
    return static_cast<double>(cv::countNonZero(edges)) / gray.total();
}

static std::vector<float> computeLBPHistogram(const cv::Mat& bgr)
{
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

    std::vector<float> hist(256, 0.0f);

    for (int y = 1; y < gray.rows - 1; ++y)
    {
        const uint8_t* rowUp   = gray.ptr<uint8_t>(y - 1);
        const uint8_t* rowCur  = gray.ptr<uint8_t>(y);
        const uint8_t* rowDown = gray.ptr<uint8_t>(y + 1);

        for (int x = 1; x < gray.cols - 1; ++x)
        {
            uint8_t center = rowCur[x];
            uint8_t code = 0;
            code |= ((rowUp[x - 1]   >= center) ? 1 : 0) << 0;
            code |= ((rowUp[x]       >= center) ? 1 : 0) << 1;
            code |= ((rowUp[x + 1]   >= center) ? 1 : 0) << 2;
            code |= ((rowCur[x + 1]  >= center) ? 1 : 0) << 3;
            code |= ((rowDown[x + 1] >= center) ? 1 : 0) << 4;
            code |= ((rowDown[x]     >= center) ? 1 : 0) << 5;
            code |= ((rowDown[x - 1] >= center) ? 1 : 0) << 6;
            code |= ((rowCur[x - 1]  >= center) ? 1 : 0) << 7;
            hist[code] += 1.0f;
        }
    }

    float sum = 0.0f;
    for (float v : hist) { sum += v; }
    if (sum > 0.0f)
    {
        for (float& v : hist) { v /= sum; }
    }
    return hist;
}

static double lbpDistance(const std::vector<float>& a,
                           const std::vector<float>& b)
{
    if (a.size() != 256 || b.size() != 256) { return 1.0; }
    double sum = 0.0;
    for (std::size_t i = 0; i < 256; ++i)
    {
        sum += std::abs(a[i] - b[i]);
    }
    return sum / 2.0;
}

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

        // 多线程并行读取 tiny 和 LBP 文件（主要 I/O 瓶颈）
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

    // 输出 tile 使用原生分辨率（180×320），保证每个 tile 可观可辨
    // 但如果总输出超过 JPEG 65500px 硬限制，自动等比缩减
    int outTileW = cfg.nativeTileW;
    int outTileH = cfg.nativeTileH;
    const int MAX_DIM = 65500;
    if (tilesX * outTileW > MAX_DIM || tilesY * outTileH > MAX_DIM)
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
    std::vector<double> allTL(totalTiles), allTA(totalTiles), allTB(totalTiles);
    std::vector<std::vector<float>> allGrid(totalTiles);
    std::vector<std::vector<uint8_t>> allTiny(totalTiles);
    std::vector<double> allEdge(totalTiles);
    std::vector<std::vector<float>> allLBP(totalTiles);

    int nThreads = std::thread::hardware_concurrency();
    if (nThreads < 2) nThreads = 2;
    std::atomic<int> featDone{0};
    std::vector<std::thread> featWorkers;
    for (int t = 0; t < nThreads; ++t) {
        featWorkers.emplace_back([&, t]() {
            for (int idx = t; idx < totalTiles; idx += nThreads) {
                int ty = idx / tilesX, tx = idx % tilesX;
                cv::Mat roi = target(cv::Rect(tx*cfg.tileW, ty*cfg.tileH, cfg.tileW, cfg.tileH));
                // 将 ROI 缩放到图库原生分辨率，确保特征与库图片同尺度可比
                cv::Mat roiNative;
                cv::resize(roi, roiNative, cv::Size(cfg.nativeTileW, cfg.nativeTileH), 0, 0, cv::INTER_LINEAR);
                cv::Mat lab; cv::cvtColor(roiNative, lab, cv::COLOR_BGR2Lab);
                cv::Scalar m = cv::mean(lab);
                allTL[idx]=m[0]; allTA[idx]=m[1]; allTB[idx]=m[2];
                allGrid[idx] = computeGrid4x4(roiNative);
                allTiny[idx] = computeTinyImage(roiNative);
                allEdge[idx] = computeEdgeDensity(roiNative);
                allLBP[idx] = computeLBPHistogram(roiNative);
                int d = ++featDone;
                if (d % 500 == 0 || d == totalTiles)
                    std::cout << "\r  features " << d << "/" << totalTiles << std::flush;
            }
        });
    }
    for (auto& w : featWorkers) w.join();
    std::cout << std::endl;

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
    const int N = cfg.candidates;

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

        // —— Phase A: 内存二分查找（allRecords 已在内存，无需 SQLite） ——
        // 建立 avgL 排序索引，O(log n) 查找替代 65000+ 次 SQL 查询
        std::cout << "  building L-index (" << dbCount << " images)..." << std::flush;
        std::vector<std::pair<double, int>> lIndex;  // (avgL, libIdx)
        lIndex.reserve(dbCount);
        for (int i = 0; i < dbCount; ++i)
            lIndex.emplace_back(allRecords[i].avgL, i);
        std::sort(lIndex.begin(), lIndex.end());
        std::cout << " done" << std::endl;

        std::cout << "  collecting candidates..." << std::flush;
        std::vector<int> allIndices(totalTiles * N, -1);
        for (int ti = 0; ti < totalTiles; ++ti)
        {
            double tL = allTL[ti];
            double lo = tL - cfg.lRange;
            double hi = tL + cfg.lRange;

            // 逐步扩大范围直到命中（处理极端亮/暗 tile）
            while (true)
            {
                auto itLo = std::lower_bound(lIndex.begin(), lIndex.end(), lo,
                    [](const auto& p, double v) { return p.first < v; });
                auto itHi = std::upper_bound(itLo, lIndex.end(), hi,
                    [](double v, const auto& p) { return v < p.first; });

                int count = static_cast<int>(itHi - itLo);
                if (count > 0)
                {
                    int take = std::min(count, N);
                    // 从命中区间均匀采样，避免偏向某一亮度
                    double step = static_cast<double>(count) / take;
                    for (int j = 0; j < take; ++j)
                        allIndices[ti * N + j] = itLo[static_cast<int>(j * step)].second;
                    break;
                }
                // 扩大搜索范围（对称扩展）
                double expand = hi - lo;
                lo = (lo - expand < 0.0) ? 0.0 : lo - expand;
                hi = (hi + expand > 255.0) ? 255.0 : hi + expand;
                if (lo <= 0.0 && hi >= 255.0) break;  // 全库无匹配
            }
        }
        std::cout << " done" << std::endl;

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

        // —— Phase E: 贴图 ——
        int nThreads = std::thread::hardware_concurrency();
        if (nThreads < 2) nThreads = 2;

        if (cfg.tiledOutput)
        {
            // 分块输出：每 tile 独立文件，无尺寸限制，无需大 Mat
            std::error_code ec;
            std::filesystem::create_directories(outputPath, ec);
            std::cout << "  writing tiles to " << outputPath << "/ (" << nThreads << " threads)..."
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
                        snprintf(fname, sizeof(fname), "%s/tile_%04d_%04d.jpg",
                                 outputPath.c_str(), ty, tx);
                        imwriteUnicode(fname, r, {cv::IMWRITE_JPEG_QUALITY, 95});
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
            std::cout << "Tiles written: " << matched << " / " << totalTiles;
            if (loadFail > 0) std::cout << "  (failed: " << loadFail << ")";
            std::cout << std::endl;
            if (gpuLib.count > 0) cuda::freeLibrary(gpuLib);
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

    if (!imwriteUnicode(outputPath, output, {cv::IMWRITE_JPEG_QUALITY, 100}))
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
    return true;
}

} // namespace mosaicraft
