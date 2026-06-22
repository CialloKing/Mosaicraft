#include "core/Database.h"
#include "core/Database.h"
#include "core/FeatureExtractor.h"
#include "core/FeaturePack.h"
#include "core/FeatureUtils.h"
#include "core/ImageNormalizer.h"
#include "core/MosaicEngine.h"
#include "core/UnicodeIO.h"
#include "compute/CudaBackend.h"
#include "compute/FeatureExtractorCuda.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace mosaicraft;

// ============================================================
// 工具
// ============================================================

static std::string hashMat(const cv::Mat& mat)
{
    const uint8_t* data = mat.data;
    const std::size_t len = mat.total() * mat.elemSize();

    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL;

    std::uint64_t hash = FNV_OFFSET;
    for (std::size_t i = 0; i < len; ++i)
    {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= FNV_PRIME;
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

// 用法
static void printHelp()
{
    std::cout << R"(Mosaicraft — Image mosaic generator

Usage:
  mosaicraft build   [options]    Build image database
  mosaicraft mosaic  [options]    Create mosaic from target image
  mosaicraft inspect [options]    Inspect image features / database coverage
  mosaicraft db-stats [options]   Show database statistics
  mosaicraft db-purge [options]   Remove orphan records (after deleting images)

Build options:
  -i, --input  <dir>     Source image directory (required)
  -o, --output <dir>     Output directory for normalized images (default: normalized)
  -d, --db     <path>    Database path (default: mosaicraft.db)
  -t, --threads <n>      Worker threads (default: auto)

Mosaic options:
  -i, --input  <path>    Target image to mosaicify (required)
  -d, --db     <path>    Database path (default: mosaicraft.db)
  -o, --output <path>    Output path or directory (default: mosaic.jpg)
  --tile-w     <n>       Tile width in pixels (default: 45)
  --tile-h     <n>       Tile height in pixels (default: 80)
  --tiled                Output tiles as separate files (no size limit)
  --deepzoom             Generate Deep Zoom pyramid (compatible with OpenSeadragon)
  --quality    <n>       JPEG/WebP quality 1-100 (default: 95)
  --format     <ext>     jpg, png, webp, tiff (default: from output extension)
  --cpu                  Force CPU (no GPU)

Common options:
  -h, --help             Show this help

Inspect options:
  -i, --input  <path>    Image to inspect (required)
  -d, --db     <path>    Database path (default: mosaicraft.db)
)";
}

// ============================================================
// build 子命令
// ============================================================
static int cmdBuild(int argc, char* argv[])
{
    std::string inputDir;
    std::string outputDir = "normalized";
    std::string dbPath = "mosaicraft.db";
    int threads = 0;
    bool appendMode = false;
    bool normOnly = false;

    // 解析参数
    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-i" || arg == "--input") && i + 1 < argc)
        {
            inputDir = argv[++i];
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc)
        {
            outputDir = argv[++i];
        }
        else if ((arg == "-d" || arg == "--db") && i + 1 < argc)
        {
            dbPath = argv[++i];
        }
        else if ((arg == "-t" || arg == "--threads") && i + 1 < argc)
        {
            threads = std::atoi(argv[++i]);
            if (threads < 0)
            {
                threads = 0;
            }
        }
        else if (arg == "--append")
        {
            appendMode = true;
        }
        else if (arg == "--normalize-only")
        {
            normOnly = true;
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: mosaicraft build -i <dir> [-o <dir>] [-d <db>] [-t <n>] [--append] [--normalize-only]" << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << std::endl;
            return 1;
        }
    }

    if (inputDir.empty())
    {
        std::cerr << "ERROR: --input is required for build." << std::endl;
        return 1;
    }

    // ——— 建库 ———
    Database db(dbPath);
    if (!db.isOpen())
    {
        std::cerr << "Cannot open database: " << dbPath << std::endl;
        return 1;
    }

    if (!db.createTables())
    {
        std::cerr << "Cannot create tables." << std::endl;
        return 1;
    }

    std::error_code ec;
    fs::create_directories(outputDir, ec);
    std::string featDir = outputDir + "/features";
    fs::create_directories(featDir, ec);

    // 收集图片文件
    const std::vector<std::string> exts = {
        ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tiff", ".tif"
    };

    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(inputDir, ec))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string ext = entry.path().extension().string();
        for (auto& c : ext)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        bool supported = false;
        for (const auto& e : exts)
        {
            if (ext == e)
            {
                supported = true;
                break;
            }
        }
        if (supported)
        {
            files.push_back(pathToUtf8(entry.path()));
        }
    }

    std::sort(files.begin(), files.end());
    std::cout << "Found " << files.size() << " image(s) in " << inputDir << std::endl;

    if (files.empty())
    {
        std::cerr << "No supported images found." << std::endl;
        return 1;
    }

    // ——— Phase 1: 多线程并行归一化 ———
    ImageNormalizer normalizer(180, 320);
    std::vector<std::string> outPaths(files.size());
    std::vector<bool> okFlags(files.size(), false);
    std::atomic<int> normDone{0};
    int normThreads = static_cast<int>(std::thread::hardware_concurrency());
    if (normThreads < 2) normThreads = 2;

    std::cout << "Normalizing (" << normThreads << " threads)..." << std::endl;

    std::vector<std::thread> workers;
    std::atomic<size_t> nextIdx{0};
    for (int t = 0; t < normThreads; ++t) {
        workers.emplace_back([&]() {
            for (;;) {
                size_t i = nextIdx.fetch_add(1);
                if (i >= files.size()) break;
                const std::string& inPath = files[i];
                std::string ext = pathToUtf8(u8path(inPath).extension());
                char name[64];
                snprintf(name, sizeof(name), "%06zu%s", i, ext.c_str());
                fs::path outPath = fs::path(outputDir) / name;
                outPaths[i] = pathToUtf8(outPath);
                try {
                    if (normalizer.process(inPath, pathToUtf8(outPath)))
                        okFlags[i] = true;
                } catch (...) {}
                int d = ++normDone;
                if (d % 200 == 0 || d == (int)files.size())
                    std::cout << "\r  normalize " << d << "/" << files.size() << std::flush;
            }
        });
    }
    for (auto& w : workers) w.join();
    std::cout << std::endl;

    // ——— Phase 2: 哈希 + 特征提取 + 入库 ———
    if (normOnly)
    {
        std::cout << "Normalize only: " << (normDone.load()) << " images written to " << outputDir << std::endl;
        return 0;
    }
    FeatureExtractor extractor;
    bool gpuOk = cuda::isCudaAvailable();
    constexpr int GPU_BATCH = 32;
    if (gpuOk) std::cout << "Features: GPU (batch " << GPU_BATCH << ")" << std::endl;
    else       std::cout << "Features: CPU" << std::endl;

    int inserted = 0, skipped = 0;
    std::vector<cv::Mat> gpuImgs; gpuImgs.reserve(GPU_BATCH);
    std::vector<ImageRecord> gpuRecs; gpuRecs.reserve(GPU_BATCH);
    std::vector<std::string> gpuStems; gpuStems.reserve(GPU_BATCH);

    auto flushGpu = [&]() {
        if (gpuImgs.empty()) return;
        int done = cuda::extractBatch(gpuImgs, gpuRecs, featDir, gpuStems);
        if (done <= 0) {
            // GPU 批量失败，逐张 CPU 重试
            for (size_t bi = 0; bi < gpuImgs.size(); ++bi)
                extractor.compute(gpuImgs[bi], gpuRecs[bi], featDir, gpuStems[bi]);
        }
        db.beginTransaction();
        for (auto& rec : gpuRecs) {
            if (db.insertImage(rec)) inserted++; else skipped++;
        }
        db.commitTransaction();
        gpuImgs.clear(); gpuRecs.clear(); gpuStems.clear();
    };

    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!okFlags[i]) { skipped++; continue; }
        const std::string& outPath = outPaths[i];
        try {
            cv::Mat img = imreadUnicode(outPath, cv::IMREAD_COLOR);
            if (img.empty()) { skipped++; continue; }
            std::string hash = hashMat(img);
            if (db.existsByHash(hash)) { fs::remove(u8path(outPath)); skipped++; continue; }

            ImageRecord rec;
            rec.filePath = outPath;
            rec.fileHash = hash;
            rec.srcWidth = img.cols; rec.srcHeight = img.rows;
            rec.aspectRatio = (rec.srcHeight > 0) ? (double)rec.srcWidth / rec.srcHeight : 0.0;
            rec.fileSize = fs::file_size(u8path(outPath));
            std::string ext = pathToUtf8(u8path(outPath).extension());
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            rec.format = ext;

            if (gpuOk) {
                gpuImgs.push_back(img); gpuRecs.push_back(rec);
                gpuStems.push_back(pathToUtf8(u8path(outPath).stem()));
                if ((int)gpuImgs.size() >= GPU_BATCH) flushGpu();
            } else {
                extractor.compute(img, rec, featDir, pathToUtf8(u8path(outPath).stem()));
                if (db.insertImage(rec)) inserted++; else skipped++;
            }
        } catch (...) { skipped++; }
        if ((i+1) % 200 == 0 || i+1 == files.size()) {
            if (gpuOk) flushGpu();
            std::cout << "\r  " << (i+1) << "/" << files.size()
                      << " (ok:" << inserted << " skip:" << skipped << ")" << std::flush;
        }
    }
    if (gpuOk) flushGpu();

    std::cout << std::endl;
    std::cout << "Done: " << inserted << " images indexed, "
              << db.totalCount() << " total in database." << std::endl;

    // 构建二进制特征缓存（消除 50K 小文件 I/O）
    std::cout << "Building feature cache..." << std::flush;
    if (FeaturePack::buildCache(featDir, db.allRecords()))
        std::cout << " done" << std::endl;
    else
        std::cout << " failed" << std::endl;

    return 0;
}

// ============================================================
// mosaic 子命令
// ============================================================
static int cmdMosaic(int argc, char* argv[])
{
    std::string inputPath;
    std::string dbPath = "mosaicraft.db";
    std::string outputPath = "mosaic.jpg";
    MosaicEngine::Config cfg;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-i" || arg == "--input") && i + 1 < argc)
        {
            inputPath = argv[++i];
        }
        else if ((arg == "-d" || arg == "--db") && i + 1 < argc)
        {
            dbPath = argv[++i];
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc)
        {
            outputPath = argv[++i];
        }
        else if (arg == "--tile-w" && i + 1 < argc)
        {
            cfg.tileW = std::max(4, std::atoi(argv[++i]));
        }
        else if (arg == "--tile-h" && i + 1 < argc)
        {
            cfg.tileH = std::max(4, std::atoi(argv[++i]));
        }
        else if (arg == "--out-w" && i + 1 < argc)
        {
            cfg.outW = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "--out-h" && i + 1 < argc)
        {
            cfg.outH = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "--lab-weight" && i + 1 < argc)
        {
            cfg.labWeight = std::atof(argv[++i]);
        }
        else if (arg == "--grid-weight" && i + 1 < argc)
        {
            cfg.gridWeight = std::atof(argv[++i]);
        }
        else if (arg == "--tiny-weight" && i + 1 < argc)
        {
            cfg.tinyWeight = std::atof(argv[++i]);
        }
        else if (arg == "--edge-weight" && i + 1 < argc)
        {
            cfg.edgeWeight = std::atof(argv[++i]);
        }
        else if (arg == "--lbp-weight" && i + 1 < argc)
        {
            cfg.lbpWeight = std::atof(argv[++i]);
        }
        else if (arg == "--penalty" && i + 1 < argc)
        {
            cfg.usePenalty = std::atof(argv[++i]);
        }
        else if (arg == "--l-range" && i + 1 < argc)
        {
            cfg.lRange = std::atof(argv[++i]);
        }
        else if (arg == "--candidates" && i + 1 < argc)
        {
            cfg.candidates = std::max(10, std::atoi(argv[++i]));
        }
        else if (arg == "--topn-random" && i + 1 < argc)
        {
            int v = std::atoi(argv[++i]);
            cfg.topNrandom = std::max(1, v);
        }
        else if (arg == "--format" && i + 1 < argc)
        {
            cfg.outputFormat = argv[++i];
            cfg.formatExplicit = true;
        }
        else if (arg == "--cpu")
        {
            cfg.useGpu = false;
        }
        else if (arg == "--quality" && i + 1 < argc)
        {
            int q = std::atoi(argv[++i]);
            cfg.jpegQuality = std::max(1, std::min(100, q));
        }
        else if (arg == "--tiled")
        {
            cfg.tiledOutput = true;
        }
        else if (arg == "--deepzoom")
        {
            cfg.tiledOutput = true;   // deepzoom 隐含 tiled
            cfg.deepZoom = true;
        }
        else if (arg == "--no-color-adjust")
        {
            cfg.colorAdjust = false;
        }
        else if (arg == "--upscale" && i + 1 < argc)
        {
            cfg.upscale = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "--output-tile" && i + 2 < argc)
        {
            cfg.nativeTileW = std::max(1, std::atoi(argv[++i]));
            cfg.nativeTileH = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "--color-adjust")
        {
            cfg.colorAdjust = true;
        }
        else if (arg == "--adaptive-weights")
        {
            cfg.adaptiveWeights = true;
        }
        else if (arg == "--analyze")
        {
            cfg.analyze = true;
        }
        else if (arg == "--color-strength" && i + 1 < argc)
        {
            double v = std::atof(argv[++i]);
            cfg.colorStrength = std::max(0.0, std::min(0.5, v));
        }
        else if (arg == "--benchmark")
        {
            cfg.benchmark = true;
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: mosaicraft mosaic -i <image> -d <db> [options]" << std::endl;
            std::cout << "  -i, --input <path>    Target image (required)" << std::endl;
            std::cout << "  -d, --db <path>       Database (default: mosaicraft.db)" << std::endl;
            std::cout << "  -o, --output <path>   Output path (default: mosaic.jpg)" << std::endl;
            std::cout << "  --tile-w <n>          Tile width (default: 9, min: 4)" << std::endl;
            std::cout << "  --tile-h <n>          Tile height (default: 16, min: 4)" << std::endl;
            std::cout << "  --out-w <n>           Output width in pixels (0=auto)" << std::endl;
            std::cout << "  --out-h <n>           Output height in pixels (0=auto)" << std::endl;
            std::cout << "  --lab-weight <f>      LAB weight (default: 0.20)" << std::endl;
            std::cout << "  --grid-weight <f>     Grid weight (default: 0.45)" << std::endl;
            std::cout << "  --tiny-weight <f>     TinyImage weight (default: 0.25)" << std::endl;
            std::cout << "  --edge-weight <f>     Edge density weight (default: 0.05)" << std::endl;
            std::cout << "  --lbp-weight <f>      LBP histogram weight (default: 0.05)" << std::endl;
            std::cout << "  --penalty <f>         Use-count penalty (default: 0.01)" << std::endl;
            std::cout << "  --l-range <f>         L brightness search range (default: 20)" << std::endl;
            std::cout << "  --candidates <n>      Coarse candidates per tile (default: 200)" << std::endl;
            std::cout << "  --topn-random <n>     Pick from top-N (1=best, >1=varied, default: 1)" << std::endl;
            std::cout << "  --quality <n>         JPEG/WebP quality 1-100 (default: 95)" << std::endl;
            std::cout << "  --format <ext>        Output format: jpg, png, webp, tiff (default: from extension)" << std::endl;
            std::cout << "  --cpu                 Force CPU (no GPU)" << std::endl;
            std::cout << "  --tiled               Output tiles as separate files (no size limit)" << std::endl;
            std::cout << "  --deepzoom            Generate Deep Zoom pyramid + .dzi + HTML viewer" << std::endl;
            std::cout << "                        (best with --out-w/h to limit tile count)" << std::endl;
            std::cout << "  --color-adjust        Enable per-tile brightness/saturation jitter" << std::endl;
            std::cout << "  --color-strength <f>  Color jitter intensity 0-0.5 (default: 0.10)" << std::endl;
            std::cout << "  --benchmark           Print per-phase timing breakdown" << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << std::endl;
            return 1;
        }
    }

    if (inputPath.empty())
    {
        std::cerr << "ERROR: --input is required for mosaic." << std::endl;
        return 1;
    }

    MosaicEngine engine;
    bool ok = engine.generate(inputPath, dbPath, outputPath, cfg);

    return ok ? 0 : 1;
}

// ============================================================
// inspect 子命令 — 特征诊断
// ============================================================
static int cmdInspect(int argc, char* argv[])
{
    std::string imagePath;
    std::string dbPath = "mosaicraft.db";

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-i" || arg == "--input") && i + 1 < argc)
            imagePath = argv[++i];
        else if ((arg == "-d" || arg == "--db") && i + 1 < argc)
            dbPath = argv[++i];
    }

    if (imagePath.empty())
    {
        std::cerr << "Usage: mosaicraft inspect -i <image> [-d <db>]" << std::endl;
        return 1;
    }

    // 加载并归一化图像
    cv::Mat img = imreadUnicode(imagePath, cv::IMREAD_COLOR);
    if (img.empty())
    {
        std::cerr << "ERROR: Cannot read: " << imagePath << std::endl;
        return 1;
    }
    std::cout << "Image: " << imagePath << " (" << img.cols << "x" << img.rows << ")" << std::endl;

    cv::Mat native;
    cv::resize(img, native, cv::Size(180, 320), 0, 0, cv::INTER_LINEAR);

    // 提取特征
    cv::Mat lab;
    cv::cvtColor(native, lab, cv::COLOR_BGR2Lab);
    cv::Scalar m = cv::mean(lab);
    double tL = m[0], tA = m[1], tB = m[2];

    auto grid = computeGrid4x4(native);
    auto tiny = computeTinyImage(native);
    double edge = computeEdgeDensity(native);
    auto lbp = computeLBPHistogram(native);

    // 显示
    std::cout << "\nAvgLAB:  L=" << std::fixed << std::setprecision(1) << tL
              << "  A=" << tA << "  B=" << tB << std::endl;
    std::cout << "Edge density: " << std::setprecision(4) << edge << std::endl;
    std::cout << "LBP entropy:  " << std::setprecision(4);
    double lbpEntropy = 0.0;
    for (float v : lbp) { if (v > 0) lbpEntropy -= v * std::log2(v); }
    std::cout << lbpEntropy << std::endl;

    // 查询数据库
    Database db(dbPath);
    if (db.isOpen())
    {
        int total = db.totalCount();
        auto candidates = db.queryIdsByLRange(tL - 20, tL + 20, 200, false);
        std::cout << "\nDatabase: " << total << " images" << std::endl;
        std::cout << "L-range [" << (tL - 20) << ", " << (tL + 20) << "]: "
                  << candidates.size() << " candidates"
                  << " (" << std::setprecision(1) << (100.0 * candidates.size() / total) << "% of library)"
                  << std::endl;

        // 统计 L 分布（从 allRecords）
        auto all = db.allRecords();
        double minL = 255, maxL = 0, sumL = 0;
        for (const auto& r : all)
        {
            if (r.avgL < minL) minL = r.avgL;
            if (r.avgL > maxL) maxL = r.avgL;
            sumL += r.avgL;
        }
        std::cout << "Library L range: [" << std::setprecision(1) << minL
                  << ", " << maxL << "]  avg=" << (sumL / total) << std::endl;

        // 覆盖率
        int dark = 0, mid = 0, bright = 0;
        for (const auto& r : all)
        {
            if (r.avgL < 30) dark++;
            else if (r.avgL < 70) mid++;
            else bright++;
        }
        std::cout << "Distribution: dark=" << dark << " (" << (100.0*dark/total)
                  << "%)  mid=" << mid << " (" << (100.0*mid/total)
                  << "%)  bright=" << bright << " (" << (100.0*bright/total) << "%)" << std::endl;
    }

    return 0;
}

// ============================================================
// db-stats 子命令：输出图库统计信息
// ============================================================
static int cmdDbStats(int argc, char* argv[])
{
    std::string dbPath = "mosaicraft.db";

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-d" || arg == "--db") && i + 1 < argc)
            dbPath = argv[++i];
    }

    Database db(dbPath);
    if (!db.isOpen())
    {
        std::cerr << "ERROR: Cannot open database: " << dbPath << std::endl;
        return 1;
    }

    auto all = db.allRecords();
    int total = static_cast<int>(all.size());
    if (total == 0)
    {
        std::cout << "Database is empty." << std::endl;
        return 0;
    }

    // LAB 分布
    double minL=255, maxL=0, sumL=0, sumA=0, sumB=0;
    double minA=255, maxA=0, minB=255, maxB=0;
    for (const auto& r : all)
    {
        if (r.avgL < minL) minL = r.avgL; if (r.avgL > maxL) maxL = r.avgL;
        if (r.avgA < minA) minA = r.avgA; if (r.avgA > maxA) maxA = r.avgA;
        if (r.avgB < minB) minB = r.avgB; if (r.avgB > maxB) maxB = r.avgB;
        sumL += r.avgL; sumA += r.avgA; sumB += r.avgB;
    }

    // 亮度分布
    int dark=0, mid=0, bright=0;
    for (const auto& r : all)
    {
        if (r.avgL < 30) dark++;
        else if (r.avgL < 70) mid++;
        else bright++;
    }

    // Grid 维度
    int gridDim = all[0].grid4x4.size();

    std::cout << "=== Database Statistics ===\n";
    std::cout << "  Images: " << total << "\n";
    std::cout << "  Grid dim: " << gridDim << " (" << (gridDim/3) << " cells)\n";
    std::cout << "  LAB: L[" << std::fixed << std::setprecision(1) << minL
              << "," << maxL << "] avg=" << (sumL/total)
              << "  A[" << minA << "," << maxA << "] avg=" << (sumA/total)
              << "  B[" << minB << "," << maxB << "] avg=" << (sumB/total) << "\n";
    std::cout << "  Brightness: dark=" << dark << " (" << (100.0*dark/total)
              << "%) mid=" << mid << " (" << (100.0*mid/total)
              << "%) bright=" << bright << " (" << (100.0*bright/total) << "%)\n";

    // 亮度直方图 (8 bins: 0-32, 32-64, 64-96, 96-128, 128-160, 160-192, 192-224, 224-256)
    int hist[8] = {0};
    for (const auto& r : all)
    {
        int b = static_cast<int>(r.avgL) / 32;
        if (b < 0) b = 0; if (b > 7) b = 7;
        hist[b]++;
    }
    std::cout << "  L-histogram:\n";
    for (int i = 0; i < 8; ++i)
    {
        int lo = i * 32, hi = (i == 7) ? 255 : (i + 1) * 32 - 1;
        std::string bar(hist[i] * 50 / total, '#');
        std::cout << "    " << std::setw(3) << lo << "-" << std::setw(3) << hi
                  << " | " << bar << " " << hist[i] << "\n";
    }

    // 覆盖缺口分析（基于已有统计量）
    std::cout << "  Coverage gaps:";
    int gaps = 0;
    if (dark + mid < total / 100)   { std::cout << " dark(" << (dark+mid) << ")"; gaps++; }
    if (minA > 110.0)               { std::cout << " green-biased"; gaps++; }
    if (maxA < 145.0)               { std::cout << " red-deficient"; gaps++; }
    if (minB > 110.0)               { std::cout << " blue-biased"; gaps++; }
    if (maxB < 145.0)               { std::cout << " yellow-deficient"; gaps++; }
    if (gaps == 0)                  { std::cout << " none significant"; }
    std::cout << "\n";

    return 0;
}

// ============================================================
// db-purge 子命令：清除归一化目录中不存在的孤儿记录
// ============================================================
static int cmdDbPurge(int argc, char* argv[])
{
    std::string dbPath = "mosaicraft.db";

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-d" || arg == "--db") && i + 1 < argc)
            dbPath = argv[++i];
    }

    Database db(dbPath);
    if (!db.isOpen()) { std::cerr << "ERROR: Cannot open DB" << std::endl; return 1; }

    auto all = db.allRecords();
    int total = static_cast<int>(all.size());
    int removed = 0;
    for (const auto& r : all)
    {
        if (!r.filePath.empty() && !std::filesystem::exists(r.filePath))
        {
            // 同步删除孤儿特征文件
            if (!r.tinyPath.empty() && std::filesystem::exists(r.tinyPath))
                std::filesystem::remove(r.tinyPath);
            if (!r.histPath.empty() && std::filesystem::exists(r.histPath))
                std::filesystem::remove(r.histPath);
            db.removeImage(r.id);
            removed++;
        }
    }

    std::cout << "Purged " << removed << " / " << total << " orphan records." << std::endl;
    if (removed > 0)
        std::cout << "  Note: delete normalized/features/*.bin and lib.ann, then re-run mosaic to rebuild caches." << std::endl;
    return 0;
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printHelp();
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "build")
    {
        return cmdBuild(argc, argv);
    }
    else if (cmd == "mosaic")
    {
        return cmdMosaic(argc, argv);
    }
    else if (cmd == "inspect")
    {
        return cmdInspect(argc, argv);
    }
    else if (cmd == "db-stats")
    {
        return cmdDbStats(argc, argv);
    }
    else if (cmd == "db-purge")
    {
        return cmdDbPurge(argc, argv);
    }
    else if (cmd == "-h" || cmd == "--help")
    {
        printHelp();
        return 0;
    }
    else
    {
        std::cerr << "Unknown command: " << cmd << std::endl;
        std::cerr << "Use 'mosaicraft --help' for usage." << std::endl;
        return 1;
    }
}
