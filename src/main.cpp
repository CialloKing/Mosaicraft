#include "core/Database.h"
#include "core/Database.h"
#include "core/FeatureExtractor.h"
#include "core/FeaturePack.h"
#include "core/FeatureUtils.h"
#include "core/ImageNormalizer.h"
#include "core/MosaicEngine.h"
#include <unordered_set>
#include "core/UnicodeIO.h"
#include "compute/CudaBackend.h"
#include "compute/FeatureExtractorCuda.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>

#include <ctime>

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
// 宸ュ叿
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

// 鐢ㄦ硶
static constexpr const char* VERSION = "1.10.0";

static void printHelp()
{
    std::cout << R"(Mosaicraft 鈥?Image mosaic generator

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
      --append           Append mode: add new images without rebuilding
  -r, --recursive        Scan subdirectories for source images
      --normalize-only   Only normalize images, don't build database

Mosaic options:
  -i, --input  <path>    Target image to mosaicify (required)
  -d, --db     <path>    Database path (default: mosaicraft.db)
  -o, --output <path>    Output path or directory (default: mosaic.jpg)
      --tile-w     <n>   Tile width in pixels (default: 9)
      --tile-h     <n>   Tile height in pixels (default: 16)
      --candidates <n>   ANN query candidates (default: 150)
      --format     <ext> jpg, png, webp, tiff (default: from extension, JPG→auto-scale over 65500px)
      --quality    <n>   JPEG/WebP quality 1-100 (default: 100)
      --png-level  <n>   PNG compression 1-9 (default: 1=fastest, 9=smallest)
      --write-mode <m>   Write mode: auto/stream/batch (default: auto, for PNG/TIFF)
      --out-w      <n>   Target output width in pixels
      --out-h      <n>   Target output height in pixels
      --upscale    <n>   Upscale target n× before tiling (more tiles, same res)
      --output-tile <w> <h>  Output tile pixel size (default: 180x320)
      --tiled            Output tiles as separate files (no size limit)
      --deepzoom         Generate Deep Zoom pyramid for OpenSeadragon
      --color-adjust      Enable LAB color adjustment (off by default)
      --color-strength <v>  Color adjustment strength (default: 0.04)
      --lab-weight   <w> LAB distance weight (default: 0.20)
      --grid-weight  <w> Grid 8×8 distance weight (default: 0.45)
      --tiny-weight  <w> Tiny feature weight (default: 0.25)
      --edge-weight  <w> Edge density weight (default: 0.05)
      --lbp-weight   <w> LBP histogram weight (default: 0.05)
      --penalty      <w> Reuse penalty per use (default: 0.01)
      --analyze          Generate quality analysis report
      --benchmark        Show phase timing
      --cpu              Force CPU, no GPU acceleration

Common options:
  -h, --help             Show this help

Inspect options:
  -i, --input  <path>    Image to inspect (required)
  -d, --db     <path>    Database path (default: mosaicraft.db)
)";
}

// ============================================================
// build 瀛愬懡浠?
// ============================================================
static int cmdBuild(int argc, char* argv[])
{
    std::string inputDir;
    std::string outputDir = "normalized";
    std::string dbPath = "mosaicraft.db";
    int threads = 0;
    bool appendMode = false;
    bool normOnly = false;
    bool recursive = false;

    // 瑙ｆ瀽鍙傛暟
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
        else if (arg == "-r" || arg == "--recursive")
        {
            recursive = true;
        }
        else if (arg == "--normalize-only")
        {
            normOnly = true;
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: mosaicraft build -i <dir> [-o <dir>] [-d <db>] [-t <n>] [--append] [-r] [--normalize-only]" << std::endl;
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

    // 鈥斺€斺€?寤哄簱 鈥斺€斺€?
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

    // 鏀堕泦鍥剧墖鏂囦欢
    const std::vector<std::string> exts = {
        ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tiff", ".tif"
    };

    std::vector<std::string> files;
    {
        auto addEntry = [&](const auto& entry) {
            if (!entry.is_regular_file()) return;
            std::string ext = entry.path().extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool supported = false;
            for (const auto& e : exts) { if (ext == e) { supported = true; break; } }
            if (supported) files.push_back(pathToUtf8(entry.path()));
        };
        if (recursive)
            for (const auto& entry : fs::recursive_directory_iterator(inputDir, ec)) addEntry(entry);
        else
            for (const auto& entry : fs::directory_iterator(inputDir, ec)) addEntry(entry);
    }

    std::sort(files.begin(), files.end());
    std::cout << "Found " << files.size() << " image(s) in " << inputDir << std::endl;

    if (files.empty())
    {
        std::cerr << "No supported images found." << std::endl;
        return 1;
    }

    // 鈥斺€斺€?Phase 1: 澶氱嚎绋嬪苟琛屽綊涓€鍖?鈥斺€斺€?
    ImageNormalizer normalizer(180, 320);
    std::vector<std::string> outPaths(files.size());
    std::vector<bool> okFlags(files.size(), false);
    std::atomic<int> normDone{0};
    int normThreads = static_cast<int>(std::thread::hardware_concurrency());
    if (normThreads < 2) normThreads = 2;

    // 当输入==输出时跳过归一化（文件已规整），仅重建索引
    std::error_code equivEc;
    bool inputIsOutput = false;
    try {
        inputIsOutput = std::filesystem::equivalent(inputDir, outputDir, equivEc);
    } catch (...) {
        inputIsOutput = false;
    }
    if (!inputIsOutput)
    {
        std::cout << "Normalizing (" << normThreads << " threads)..." << std::endl;
    }
    else
    {
        std::cout << "Skipping normalization (input == output)" << std::endl;
    }

    std::vector<std::thread> workers;
    if (!inputIsOutput)
    {
        std::atomic<size_t> nextFileIdx{0};   // 文件向量索引（从0递增）
        std::atomic<size_t> nextNameIdx = appendMode
            ? std::atomic<size_t>(db.totalCount())
            : std::atomic<size_t>(0);          // 输出文件编号（append时从已有末尾开始）
        for (int t = 0; t < normThreads; ++t) {
            workers.emplace_back([&]() {
                for (;;) {
                    size_t fi = nextFileIdx.fetch_add(1);  // 文件索引
                    if (fi >= files.size()) break;
                    const std::string& inPath = files[fi];
                    std::string ext = pathToUtf8(u8path(inPath).extension());
                    char name[64];
                    size_t nameIdx = nextNameIdx.fetch_add(1);  // 输出编号
                    snprintf(name, sizeof(name), "%06zu%s", nameIdx, ext.c_str());
                    fs::path outPath = fs::path(outputDir) / name;
                    outPaths[fi] = pathToUtf8(outPath);
                    try {
                        if (normalizer.process(inPath, pathToUtf8(outPath)))
                            okFlags[fi] = true;
                    } catch (...) {}
                    int d = ++normDone;
                    if (d % 200 == 0 || d == (int)files.size())
                        std::cout << "\r  normalize " << d << "/" << files.size() << std::flush;
                }
            });
        }
        for (auto& w : workers) w.join();
        std::cout << std::endl;
    }
    else
    {
        // input==output：文件已规整，直接使用原路径
        outPaths = files;
        normDone = static_cast<int>(files.size());
        for (size_t i = 0; i < files.size(); ++i) okFlags[i] = true;
    }

    // 鈥斺€斺€?Phase 2: 鍝堝笇 + 鐗瑰緛鎻愬彇 + 鍏ュ簱 鈥斺€斺€?
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
            // GPU 鎵归噺澶辫触锛岄€愬紶 CPU 閲嶈瘯
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

    // 鏋勫缓浜岃繘鍒剁壒寰佺紦瀛橈紙娑堥櫎 50K 灏忔枃浠?I/O锛?
    std::cout << "Building feature cache..." << std::flush;
    if (FeaturePack::buildCache(featDir, db.allRecords()))
        std::cout << " done" << std::endl;
    else
        std::cout << " failed" << std::endl;

    return 0;
}

// ============================================================
// mosaic 瀛愬懡浠?
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
        else if ((arg == "-W" || arg == "--out-w") && i + 1 < argc)
        {
            cfg.outW = std::max(1, std::atoi(argv[++i]));
        }
        else if ((arg == "-H" || arg == "--out-h") && i + 1 < argc)
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
        else if ((arg == "-f" || arg == "--format") && i + 1 < argc)
        {
            cfg.outputFormat = argv[++i];
            cfg.formatExplicit = true;
        }
        else if (arg == "--cpu")
        {
            cfg.useGpu = false;
        }
        else if ((arg == "-q" || arg == "--quality") && i + 1 < argc)
        {
            int q = std::atoi(argv[++i]);
            cfg.jpegQuality = std::max(1, std::min(100, q));
        }
        else if (arg == "--png-level" && i + 1 < argc)
        {
            int lvl = std::atoi(argv[++i]);
            cfg.pngCompressionLevel = std::max(1, std::min(9, lvl));
        }
        else if (arg == "--write-mode" && i + 1 < argc)
        {
            cfg.writeMode = argv[++i];
        }
        else if (arg == "--tiled")
        {
            cfg.tiledOutput = true;
        }
        else if (arg == "--deepzoom")
        {
            cfg.tiledOutput = true;   // deepzoom 闅愬惈 tiled
            cfg.deepZoom = true;
        }
        else if (arg == "--neighbor-window" && i + 1 < argc)
        {
            cfg.neighborWindow = std::atoi(argv[++i]);
        }
        else if ((arg == "-U" || arg == "--upscale") && i + 1 < argc)
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
            std::cout << "  --quality <n>         JPEG/WebP quality 1-100 (default: 100)" << std::endl;
            std::cout << "  --png-level <n>       PNG compression level 1-9 (default: 1=fastest)" << std::endl;
            std::cout << "  --write-mode <m>      Write mode: auto(内存自适应)/stream(低内存)/batch(全量)" << std::endl;
            std::cout << "  --format <ext>        Output format: jpg, png, webp, tiff (default: from extension)" << std::endl;
            std::cout << "  --cpu                 Force CPU (no GPU)" << std::endl;
            std::cout << "  --tiled               Output tiles as separate files (no size limit)" << std::endl;
            std::cout << "  --deepzoom            Generate Deep Zoom pyramid + .dzi + HTML viewer" << std::endl;
            std::cout << "                        (best with --out-w/h to limit tile count)" << std::endl;
            std::cout << "  --color-adjust        Enable per-tile brightness/saturation jitter" << std::endl;
            std::cout << "  --color-strength <f>  Color jitter intensity 0-0.5 (default: 0.04)" << std::endl;
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
// inspect 瀛愬懡浠?鈥?鐗瑰緛璇婃柇
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

    // 鍔犺浇骞跺綊涓€鍖栧浘鍍?
    cv::Mat img = imreadUnicode(imagePath, cv::IMREAD_COLOR);
    if (img.empty())
    {
        std::cerr << "ERROR: Cannot read: " << imagePath << std::endl;
        return 1;
    }
    std::cout << "Image: " << imagePath << " (" << img.cols << "x" << img.rows << ")" << std::endl;

    cv::Mat native;
    cv::resize(img, native, cv::Size(180, 320), 0, 0, cv::INTER_LINEAR);

    // 鎻愬彇鐗瑰緛
    cv::Mat lab;
    cv::cvtColor(native, lab, cv::COLOR_BGR2Lab);
    cv::Scalar m = cv::mean(lab);
    double tL = m[0], tA = m[1], tB = m[2];

    auto grid = computeGrid4x4(native);
    auto tiny = computeTinyImage(native);
    double edge = computeEdgeDensity(native);
    auto lbp = computeLBPHistogram(native);

    // 鏄剧ず
    std::cout << "\nAvgLAB:  L=" << std::fixed << std::setprecision(1) << tL
              << "  A=" << tA << "  B=" << tB << std::endl;
    std::cout << "Edge density: " << std::setprecision(4) << edge << std::endl;
    std::cout << "LBP entropy:  " << std::setprecision(4);
    double lbpEntropy = 0.0;
    for (float v : lbp) { if (v > 0) lbpEntropy -= v * std::log2(v); }
    std::cout << lbpEntropy << std::endl;

    // 鏌ヨ??鏁版嵁搴?
    Database db(dbPath);
    if (db.isOpen())
    {
        int total = db.totalCount();
        auto candidates = db.queryIdsByLRange(tL - 20, tL + 20, 200, false);
        std::cout << "\nDatabase: " << total << " images" << std::endl;
        std::cout << "L-range [" << (tL - 20) << ", " << (tL + 20) << "]: "
                  << candidates.size() << " candidates"
                  << (total > 0 ? " (" + std::to_string(100 * candidates.size() / total) + "%)" : "")
                  << std::endl;

        // 缁熻?? L 鍒嗗竷锛堜粠 allRecords锛?
        auto all = db.allRecords();
        double minL = 255, maxL = 0, sumL = 0;
        for (const auto& r : all)
        {
            if (r.avgL < minL) minL = r.avgL;
            if (r.avgL > maxL) maxL = r.avgL;
            sumL += r.avgL;
        }
        std::cout << "Library L range: [" << std::setprecision(1) << minL
                  << ", " << maxL << "]  avg=" << (total > 0 ? sumL/total : 0) << std::endl;

        // 瑕嗙洊鐜?
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
// db-stats 瀛愬懡浠わ細杈撳嚭鍥惧簱缁熻??淇℃伅
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

    // LAB 鍒嗗竷
    double minL=255, maxL=0, sumL=0, sumA=0, sumB=0;
    double minA=255, maxA=0, minB=255, maxB=0;
    for (const auto& r : all)
    {
        if (r.avgL < minL) minL = r.avgL; if (r.avgL > maxL) maxL = r.avgL;
        if (r.avgA < minA) minA = r.avgA; if (r.avgA > maxA) maxA = r.avgA;
        if (r.avgB < minB) minB = r.avgB; if (r.avgB > maxB) maxB = r.avgB;
        sumL += r.avgL; sumA += r.avgA; sumB += r.avgB;
    }

    // 浜??害鍒嗗竷
    int dark=0, mid=0, bright=0;
    for (const auto& r : all)
    {
        if (r.avgL < 30) dark++;
        else if (r.avgL < 70) mid++;
        else bright++;
    }

    // Grid 缁村害
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

    // 浜??害鐩存柟鍥?(8 bins: 0-32, 32-64, 64-96, 96-128, 128-160, 160-192, 192-224, 224-256)
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

    // 瑕嗙洊缂哄彛鍒嗘瀽锛堝熀浜庡凡鏈夌粺璁￠噺锛?
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
// db-purge 瀛愬懡浠わ細娓呴櫎褰掍竴鍖栫洰褰曚腑涓嶅瓨鍦ㄧ殑瀛ゅ効璁板綍
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
        if (!r.filePath.empty() && !std::filesystem::exists(u8path(r.filePath)))
        {
            // 鍚屾??鍒犻櫎瀛ゅ効鐗瑰緛鏂囦欢
            if (!r.tinyPath.empty() && std::filesystem::exists(u8path(r.tinyPath)))
                std::filesystem::remove(u8path(r.tinyPath));
            if (!r.histPath.empty() && std::filesystem::exists(u8path(r.histPath)))
                std::filesystem::remove(u8path(r.histPath));
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
// db-usage 子命令：查看图片使用统计
// ============================================================
static int cmdDbUsage(int argc, char* argv[])
{
    std::string dbPath = "mosaicraft.db";
    int limit = 50;
    std::string exportDir;
    bool showUnused = false;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-d" || arg == "--db") && i + 1 < argc)
            dbPath = argv[++i];
        else if (arg == "-n" && i + 1 < argc)
            limit = std::atoi(argv[++i]);
        else if (arg == "--unused")
            showUnused = true;
        else if ((arg == "--export" || arg == "-o") && i + 1 < argc)
            exportDir = argv[++i];
    }

    Database db(dbPath);
    if (!db.isOpen()) { std::cerr << "ERROR: Cannot open DB" << std::endl; return 1; }

    auto top = db.topUsedImages(limit);
    if (top.empty())
    {
        std::cout << "No usage data yet. Run a mosaic with --analyze to start tracking." << std::endl;
        return 0;
    }

    std::cout << "Top " << top.size() << " most used images (by mosaic runs):\n";
    std::cout << "  Rank  Image ID    Runs    Tiles\n";
    for (size_t i = 0; i < top.size(); ++i)
    {
        auto [id, runs, tiles] = top[i];
        std::cout << "  " << std::setw(4) << (i+1) << "  "
                  << std::setw(8) << id << "  "
                  << std::setw(6) << runs << "  "
                  << std::setw(8) << tiles << "\n";
    }

    // --unused: 列出从未使用的图片
    if (showUnused)
    {
        auto allRecs = db.allRecords();
        auto used = db.topUsedImages(999999);
        std::unordered_set<int> usedIds;
        for (const auto& [id, runs, tiles] : used) usedIds.insert(id);

        int unused = 0;
        for (const auto& r : allRecs)
        {
            if (!usedIds.count(r.id))
            {
                if (unused < 50)  // 只显示前50个
                    std::cout << "  unused: id=" << r.id << "  " << r.filePath << "\n";
                unused++;
            }
        }
        std::cout << "\n  Total unused: " << unused << " / " << allRecs.size()
                  << " (" << std::fixed << std::setprecision(1) << (100.0*unused/allRecs.size()) << "%)\n";
        if (unused > 0 && exportDir.empty())
            std::cout << "  Tip: use --export <dir> to export used images\n";
    }

    // 导出：按全局 tile 使用量排序，复制归一化图到导出目录
    if (!exportDir.empty())
    {
        auto allRecs = db.allRecords();
        std::unordered_map<int, std::string> pathMap;
        for (const auto& r : allRecs) pathMap[r.id] = r.filePath;

        auto allUsed = db.topUsedImages(999999);  // 全部
        // 按 total_tiles 降序排列（topUsedImages 返回的是按 total_runs 排序，需要重新排）
        std::sort(allUsed.begin(), allUsed.end(),
            [](const auto& a, const auto& b) {
                return std::get<2>(a) > std::get<2>(b);
            });

        std::filesystem::create_directories(exportDir);
        int exported = 0;
        for (size_t i = 0; i < allUsed.size(); ++i)
        {
            auto [id, runs, tiles] = allUsed[i];
            auto it = pathMap.find(id);
            if (it == pathMap.end()) continue;
            std::string srcPath = it->second;
            if (!std::filesystem::exists(srcPath)) continue;
            auto dotPos = srcPath.find_last_of('.');
            std::string ext = (dotPos != std::string::npos) ? srcPath.substr(dotPos) : "";
            char fname[512];
            snprintf(fname, sizeof(fname), "%s/rank%04zu_%druns_%dtiles_id%d%s",
                     exportDir.c_str(), i+1, runs, tiles, id, ext.c_str());
            std::filesystem::copy_file(srcPath, fname,
                std::filesystem::copy_options::overwrite_existing);
            exported++;
        }
        std::cout << "\n  Exported " << exported << " images to " << exportDir << "\n";
    }
    return 0;
}

// ============================================================
// db-health 子命令：图库健康度诊断
// ============================================================
static int cmdDbHealth(int argc, char* argv[])
{
    std::string dbPath = "mosaicraft.db";
    for (int i = 2; i < argc; ++i)
        if ((std::string(argv[i]) == "-d" || std::string(argv[i]) == "--db") && i + 1 < argc)
            dbPath = argv[++i];

    Database db(dbPath);
    if (!db.isOpen()) { std::cerr << "ERROR: Cannot open DB" << std::endl; return 1; }

    auto all = db.allRecords();
    int total = static_cast<int>(all.size());
    if (total == 0) { std::cout << "Database is empty.\n"; return 0; }

    std::cout << "=== Database Health ===\n\n";
    std::cout << "Images: " << total << "\n";

    // ——— 亮度覆盖 ———
    int dark = 0, mid = 0, bright = 0;
    for (const auto& r : all) {
        if (r.avgL < 50) dark++;
        else if (r.avgL < 150) mid++;
        else bright++;
    }
    double dPct = 100.0 * dark / total, mPct = 100.0 * mid / total, bPct = 100.0 * bright / total;

    std::cout << "\n--- Brightness Coverage ---\n";
    std::cout << "  Dark  (<50L):  " << std::fixed << std::setprecision(1) << dPct << "% (" << dark << ")\n";
    std::cout << "  Mid   (50-150L): " << mPct << "% (" << mid << ")\n";
    std::cout << "  Bright(>150L): " << bPct << "% (" << bright << ")\n";
    if (dPct < 5.0)  std::cout << "  WARN: Dark images severely underrepresented\n";
    if (mPct < 15.0) std::cout << "  WARN: Mid-tone images underrepresented\n";

    // ——— 色域覆盖 ———
    double minA = 255, maxA = 0, minB = 255, maxB = 0;
    for (const auto& r : all) {
        if (r.avgA < minA) minA = r.avgA; if (r.avgA > maxA) maxA = r.avgA;
        if (r.avgB < minB) minB = r.avgB; if (r.avgB > maxB) maxB = r.avgB;
    }
    std::cout << "\n--- Color Gamut ---\n";
    std::cout << "  A (green-red): " << std::setprecision(0) << minA << "-" << maxA << "\n";
    std::cout << "  B (blue-yellow): " << std::setprecision(0) << minB << "-" << maxB << "\n";
    if (minA > 115) std::cout << "  WARN: Lacking green-toned images\n";
    if (maxA < 140) std::cout << "  WARN: Lacking red/warm-toned images\n";
    if (minB > 115) std::cout << "  WARN: Lacking blue/cool-toned images\n";
    if (maxB < 140) std::cout << "  WARN: Lacking yellow-toned images\n";

    // ——— 使用统计 ———
    auto used = db.topUsedImages(999999);
    int usedCount = static_cast<int>(used.size());
    int unusedCount = total - usedCount;

    std::cout << "\n--- Usage ---\n";
    std::cout << "  Used:   " << usedCount << " (" << std::setprecision(1) << (100.0*usedCount/total) << "%)\n";
    std::cout << "  Unused: " << unusedCount << " (" << (100.0*unusedCount/total) << "%)\n";
    if (unusedCount > total / 2)
        std::cout << "  WARN: >50% images never used - consider pruning\n";

    // ——— 热点集中度 ———
    if (!used.empty()) {
        int64_t totalTiles = 0;
        for (const auto& [id, runs, tiles] : used) totalTiles += tiles;
        int top1pct = std::max(1, usedCount / 100);
        int64_t top1Tiles = 0;
        for (int i = 0; i < top1pct && i < usedCount; ++i)
            top1Tiles += std::get<2>(used[i]);
        std::cout << "\n--- Hotspot Concentration ---\n";
        std::cout << "  Top 1% (" << top1pct << " images): " << std::setprecision(1)
                  << (100.0*top1Tiles/totalTiles) << "% of all tiles\n";
        if (100.0*top1Tiles/totalTiles > 20.0)
            std::cout << "  WARN: Small subset dominates matching\n";
    }

    // ——— 建议 ———
    std::cout << "\n--- Recommendations ---\n";
    int recs = 0;
    if (dPct < 5.0)  { std::cout << "  + Add night / indoor / low-light photos\n"; recs++; }
    if (mPct < 15.0) { std::cout << "  + Add overcast / shadow / twilight scenes\n"; recs++; }
    if (minA > 115 || maxA < 140) { std::cout << "  + Diversify green-red color range\n"; recs++; }
    if (minB > 115 || maxB < 140) { std::cout << "  + Diversify blue-yellow color range\n"; recs++; }
    if (unusedCount > total / 2) { std::cout << "  + Run db-usage to identify dead weight\n"; recs++; }
    if (recs == 0) std::cout << "  Database looks well-balanced!\n";

    return 0;
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[])
{
    srand(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(reinterpret_cast<uintptr_t>(&argc)));

    // Windows: argv 使用系统 ANSI 编码（中文系统为 GBK），转为 UTF-8 原地替换
    static std::vector<std::string> utf8Args(argc);
    for (int i = 0; i < argc; ++i)
    {
        utf8Args[i] = localToUtf8(argv[i]);
        argv[i] = &utf8Args[i][0];
    }
    if (argc < 2)
    {
        printHelp();
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "-V" || cmd == "--version")
    {
        std::cout << "Mosaicraft " << VERSION << "\n";
        return 0;
    }
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
    else if (cmd == "db-usage")
    {
        return cmdDbUsage(argc, argv);
    }
    else if (cmd == "db-health")
    {
        return cmdDbHealth(argc, argv);
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
