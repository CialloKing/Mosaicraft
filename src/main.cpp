#include "core/Database.h"
#include "core/Database.h"
#include "core/FeatureExtractor.h"
#include "core/FeaturePack.h"
#include "core/FeatureUtils.h"
#include "core/ImageNormalizer.h"
#include "core/MosaicEngine.h"
#include <unordered_set>
#include "core/UnicodeIO.h"
#include "core/UnicodeIO.h"
#include "compute/CudaBackend.h"
#ifdef MOSAICRAFT_CUDA
#include "compute/FeatureExtractorCuda.h"
#endif

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>

#include <ctime>

#include <algorithm>
#include <atomic>
#include <cctype>
#ifdef _WIN32
#include <io.h>
#include <conio.h>
#else
#include <unistd.h>
#endif
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>

// 退出码
#define EXIT_OK          0
#define EXIT_ERR_GENERAL 1
#define EXIT_ERR_DB      2
#define EXIT_ERR_MEMORY  3
#define EXIT_ERR_GPU     4

// 辅助：如果 -d 指向目录，自动查找目录内的 mosaicraft.db
static std::string resolveDbPath(const std::string& rawPath)
{
    std::error_code ec;
    if (std::filesystem::is_directory(rawPath, ec))
        return rawPath + "/mosaicraft.db";
    return rawPath;
}

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
static constexpr const char* VERSION = "1.12.1";

static void printHelp()
{
    std::cout << R"(Mosaicraft — Image mosaic generator

Usage:
  mosaicraft build    [options]    Build image database
  mosaicraft mosaic   [options]    Create mosaic from target image
  mosaicraft inspect  [options]    Inspect image features
  mosaicraft db-stats [options]    Show database statistics
  mosaicraft db-purge [options]    Remove orphan records
  mosaicraft db-health [options]   Database health diagnostics
  mosaicraft db-usage [options]    Image usage statistics

Build options:
  -i, --input  <dir>     Source image directory (required)
  -o, --output <dir>     Output directory (default: library, DB stored inside)
  -d, --db     <path>    Database path (default: <output>/mosaicraft.db)
  -t, --threads <n>      Worker threads (default: auto)
      --normalize-size <WxH>  Normalized image size (default: 180x320)
      --append           Append mode: add new images without rebuilding
  -r, --recursive        Scan subdirectories for source images
      --normalize-only   Only normalize images, don't build database

Mosaic options:
  -i, --input  <path>    Target image to mosaicify (required)
  -d, --db     <path>    Database path (default: <output>/mosaicraft.db)
  -o, --output <path>    Output path or directory (default: output/output.jpg)
      --tile-w     <n>   Tile width in pixels (default: 9)
      --tile-h     <n>   Tile height in pixels (default: 16)
      --candidates <n>   ANN query candidates (default: 150)
      --format     <ext> jpg, png, webp, tiff (default: from extension)
      --quality    <n>   JPEG/WebP quality 1-100 (default: 100)
      --png-level  <n>   PNG compression 1-9 (default: 1=fastest)
      --write-mode <m>   Write mode: auto/stream/batch (default: auto)
      --out-w      <n>   Target output width in pixels
      --out-h      <n>   Target output height in pixels
      --upscale    <n>   Upscale target nX before tiling
      --output-tile <WxH>  Output tile pixel size (default: 180x320)
      --l-range     <f>  L brightness search range (default: 20)
      --topn-random <n>  Top-N random selection (default: 10)
      --neighbor-window <n>  Neighborhood window (default: 0=auto)
      --neighbor-penalty <f> Base penalty value (default: 100)
      --tiled            Output tiles as separate files (no size limit)
      --deepzoom         Generate Deep Zoom pyramid + HTML viewer
      --color-adjust     Enable per-tile color jitter (off by default)
      --color-strength <v>  Color jitter intensity (default: 0.04)
      --adaptive-weights  Enable adaptive feature weights
      --lab-weight   <w> LAB distance weight (default: 0.20)
      --grid-weight  <w> Grid 8x8 distance weight (default: 0.45)
      --tiny-weight  <w> Tiny feature weight (default: 0.25)
      --edge-weight  <w> Edge density weight (default: 0.05)
      --lbp-weight   <w> LBP histogram weight (default: 0.05)
      --penalty      <w> Reuse penalty per use (default: 0.01)
      --analyze          Generate quality analysis report
      --benchmark        Show phase timing
      --cpu              Force CPU, no GPU acceleration

Output modes (choose one):
  -o, --output <path>   Single image output (default)
      --tiled            Tile set output (one file per tile)
      --deepzoom         Deep Zoom pyramid + HTML viewer (implies --tiled)

Common options:
  -h, --help             Show this help

Inspect options:
  -i, --input  <path>    Image to inspect (required)
  -d, --db     <path>    Database path (default: <output>/mosaicraft.db)

Exit status:
  0  success
  1  general error (bad path, invalid arguments)
  2  database error (locked, corrupt, or missing)
  3  memory / allocation failure
  4  GPU / CUDA error
)";
}

// ============================================================
// build 子命�?
// ============================================================
static int cmdBuild(int argc, char* argv[])
{
    std::string inputDir;
    std::string outputDir = "library";
    std::string dbPath = "mosaicraft.db";  // 哨兵值，见下方替换
    int threads = 0;
    bool appendMode = false;
    bool normOnly = false;
    bool forceMode = false;
    bool recursive = false;
    int normW = 180;
    int normH = 320;

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
        else if (arg == "-r" || arg == "--recursive")
        {
            recursive = true;
        }
        else if (arg == "--normalize-size" && i + 1 < argc)
        {
            const char* val = argv[++i];
            const char* sep = strchr(val, 'x');
            if (sep && sep != val && *(sep+1) != '\0') {
                normW = std::max(1, std::atoi(val));
                normH = std::max(1, std::atoi(sep + 1));
            } else {
                std::cerr << "ERROR: --normalize-size expected WxH format (e.g. 180x320), got: " << val << std::endl;
                return 1;
            }
        }
        else if (arg == "--normalize-only")
        {
            normOnly = true;
        }
        else if (arg == "-y" || arg == "--yes" || arg == "--force")
        {
            forceMode = true;
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

    // 默认 DB 放在归一化目录内（自包含图库包）
    if (dbPath == "mosaicraft.db")
        dbPath = outputDir + "/mosaicraft.db";

    // 确保输出目录存在（DB 写入需要）
    std::error_code ec;
    fs::create_directories(outputDir, ec);

    // —����?建库 —����?
    Database db(resolveDbPath(dbPath));
    if (!db.isOpen())
    {
        std::cerr << "Cannot open database: " << dbPath << std::endl;
        return EXIT_ERR_DB;
    }

    if (!db.createTables())
    {
        std::cerr << "Cannot create tables." << std::endl;
        return 2;
    }

    // 非追加模式：如果数据库已有记录，提示确认
    if (!appendMode && !normOnly)
    {
        int existingCount = db.totalCount();
        if (existingCount > 0 && !forceMode)
        {
            // 非 TTY 环境且无 -y → 拒绝执行
#ifdef _WIN32
            bool isTTY = _isatty(_fileno(stdin));
#else
            bool isTTY = isatty(fileno(stdin));
#endif
            if (!isTTY)
            {
                std::cerr << "ERROR: DB has " << existingCount
                          << " records. Use -y to overwrite (non-interactive mode)." << std::endl;
                return 1;
            }
            std::cout << "Database has " << existingCount
                      << " records. Overwrite? [y/N] " << std::flush;
            char answer = static_cast<char>(std::getchar());
            if (answer != 'y' && answer != 'Y')
            {
                std::cout << "Aborted." << std::endl;
                return 0;
            }
        }
    }

    std::string featDir = outputDir + "/features";
    fs::create_directories(featDir, ec);

    // 收集图片文件
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
    // Phase 1 & 2: pipelined — normalize (CPU) + feature extraction (GPU) run concurrently
    auto tBuildStart = std::chrono::steady_clock::now();
    auto tNormStart = tBuildStart;
    std::chrono::steady_clock::time_point tGpuStart = tBuildStart, tCacheStart = tBuildStart, tNormEnd = tBuildStart;
    ImageNormalizer normalizer(normW, normH);
    std::vector<std::string> outPaths(files.size());
    std::vector<bool> okFlags(files.size(), false);
    std::atomic<int> normDone{0};
    int normThreads = static_cast<int>(std::thread::hardware_concurrency());
    if (normThreads < 2) normThreads = 2;

    if (normOnly) { std::cout << "Normalize only mode" << std::endl; }

    // Shared queue: normalizer threads produce, GPU thread consumes
    // Bounded to prevent memory explosion (16 producers vs 1 consumer)
    struct NormItem { cv::Mat img; std::string outPath; std::string stem; };
    std::queue<NormItem> normQueue;
    std::mutex queueMtx;
    std::condition_variable queueCV;
    std::condition_variable queueFullCV;  // producer waits when queue is full
    constexpr int MAX_QUEUE = 512;         // ~345 MB max (360x640x3 x 512)
    std::atomic<bool> normPhaseDone{false};
    std::atomic<int> gpuDone{0};

    FeatureExtractor extractor;
    bool gpuOk = false;
#ifdef MOSAICRAFT_CUDA
    gpuOk = cuda::isCudaAvailable();
#endif
    constexpr int GPU_BATCH = 256;
    int inserted = 0, skipped = 0;

    // GPU consumer thread (started before normalization so it can receive immediately)
    std::thread gpuThread;
    if (gpuOk && !normOnly) {
        std::cout << "Normalizing + Features: GPU (batch " << GPU_BATCH << ", pipelined)" << std::endl;
        gpuThread = std::thread([&]() {
            bool firstBatch = true;
            std::vector<cv::Mat> imgs; imgs.reserve(GPU_BATCH);
            std::vector<ImageRecord> recs; recs.reserve(GPU_BATCH);
            std::vector<std::string> stems; stems.reserve(GPU_BATCH);
            auto flush = [&]() {
                if (imgs.empty()) return;
                if (firstBatch) { tGpuStart = std::chrono::steady_clock::now(); firstBatch = false; }
                #ifdef MOSAICRAFT_CUDA
                int done = cuda::extractBatch(imgs, recs, featDir, stems);
#else
                int done = 0;
#endif
                if (done <= 0) {
                    for (size_t bi = 0; bi < imgs.size(); ++bi)
                        extractor.compute(imgs[bi], recs[bi], featDir, stems[bi]);
                }
                db.beginTransaction();
                for (auto& rec : recs) { if (db.insertImage(rec)) inserted++; else skipped++; }
                db.commitTransaction();
                gpuDone += static_cast<int>(imgs.size());
                int gd = gpuDone.load();
                if (gd % 2000 == 0 || gd == static_cast<int>(files.size()))
                    std::cout << "\r  gpu feature " << gd << "/" << files.size() << std::flush;
                imgs.clear(); recs.clear(); stems.clear();
            };
            while (true) {
                NormItem item;
                {
                    std::unique_lock<std::mutex> lk(queueMtx);
                    queueCV.wait(lk, [&]{ return !normQueue.empty() || normPhaseDone.load(); });
                    if (normQueue.empty() && normPhaseDone.load()) { flush(); break; }
                    if (normQueue.empty()) continue;
                    item = std::move(normQueue.front());
                    normQueue.pop();
                    lk.unlock();
                    queueFullCV.notify_one();  // wake blocked producers
                }
                if (item.img.empty()) continue;
                std::string hash = hashMat(item.img);
                if (db.existsByHash(hash)) { fs::remove(u8path(item.outPath)); skipped++; continue; }
                ImageRecord rec;
                rec.filePath = item.outPath; rec.fileHash = hash;
                rec.srcWidth = item.img.cols; rec.srcHeight = item.img.rows;
                rec.aspectRatio = (rec.srcHeight > 0) ? (double)rec.srcWidth / rec.srcHeight : 0.0;
                rec.fileSize = fs::file_size(u8path(item.outPath));
                std::string ext = pathToUtf8(u8path(item.outPath).extension());
                if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
                rec.format = ext;
                imgs.push_back(item.img); recs.push_back(rec); stems.push_back(item.stem);
                if ((int)imgs.size() >= GPU_BATCH) flush();
            }
        });
    } else if (!normOnly) {
        std::cout << "Features: CPU" << std::endl;
    }

    // Producer threads: normalize images
    std::error_code equivEc;
    bool inputIsOutput = false;
    try { inputIsOutput = std::filesystem::equivalent(inputDir, outputDir, equivEc); }
    catch (...) { inputIsOutput = false; }

    if (!inputIsOutput)
    {
        if (!gpuOk || normOnly) std::cout << "Normalizing (" << normThreads << " threads)..." << std::endl;
        std::atomic<size_t> nextFileIdx{0};
        std::atomic<size_t> nextNameIdx = appendMode
            ? std::atomic<size_t>(db.totalCount())
            : std::atomic<size_t>(0);
        std::vector<std::thread> workers;
        for (int t = 0; t < normThreads; ++t) {
            workers.emplace_back([&]() {
                for (;;) {
                    size_t fi = nextFileIdx.fetch_add(1);
                    if (fi >= files.size()) break;
                    const std::string& inPath = files[fi];
                    std::string ext = pathToUtf8(u8path(inPath).extension());
                    char name[64];
                    size_t nameIdx = nextNameIdx.fetch_add(1);
                    snprintf(name, sizeof(name), "%06zu%s", nameIdx, ext.c_str());
                    fs::path outPath = fs::path(outputDir) / name;
                    outPaths[fi] = pathToUtf8(outPath);
                    try {
                        cv::Mat result = normalizer.processToMat(inPath);
                        if (result.empty()) continue;
                        // 使用较低压缩率减少体积：JPEG q90, PNG level 1
                        std::string ext = pathToUtf8(outPath.extension());
                        std::vector<int> writeParams;
                        if (ext == ".jpg" || ext == ".jpeg")
                            writeParams = {cv::IMWRITE_JPEG_QUALITY, 90};
                        else if (ext == ".png")
                            writeParams = {cv::IMWRITE_PNG_COMPRESSION, 9};
                        imwriteUnicode(pathToUtf8(outPath), result, writeParams);
                        okFlags[fi] = true;
                        if (gpuOk && !normOnly) {
                            std::unique_lock<std::mutex> lk(queueMtx);
                            queueFullCV.wait(lk, [&]{ return normQueue.size() < MAX_QUEUE || normPhaseDone.load(); });
                            normQueue.push({result.clone(), pathToUtf8(outPath), pathToUtf8(outPath.stem())});
                            lk.unlock();
                            queueCV.notify_one();
                        }
                    } catch (...) {}
                    int d = ++normDone;
                    if (d % 200 == 0 || d == (int)files.size())
                        std::cout << "\r  normalize " << d << "/" << files.size() << "                    " << std::flush;
                }
            });
        }
        for (auto& w : workers) w.join();
        tNormEnd = std::chrono::steady_clock::now();
        std::cout << std::endl;
    }
    else
    {
        outPaths = files;
        normDone = static_cast<int>(files.size());
        for (size_t i = 0; i < files.size(); ++i) okFlags[i] = true;
    }

    // Signal GPU thread that normalization is done
    normPhaseDone.store(true);
    queueCV.notify_all();
    if (gpuOk && !normOnly) {
        gpuThread.join();
        std::cout << "  GPU done: " << gpuDone.load() << std::endl;
    }

    // CPU fallback: process files that were not sent to GPU
    if (!gpuOk && !normOnly)
    {
        std::cout << "Features: CPU" << std::endl;
        for (size_t i = 0; i < files.size(); ++i) {
            if (!okFlags[i]) { skipped++; continue; }
            const std::string& outPath = outPaths[i];
            try {
                cv::Mat img = imreadUnicode(outPath, cv::IMREAD_COLOR);
                if (img.empty()) { skipped++; continue; }
                std::string hash = hashMat(img);
                if (db.existsByHash(hash)) { fs::remove(u8path(outPath)); skipped++; continue; }
                ImageRecord rec;
                rec.filePath = outPath; rec.fileHash = hash;
                rec.srcWidth = img.cols; rec.srcHeight = img.rows;
                rec.aspectRatio = (rec.srcHeight > 0) ? (double)rec.srcWidth / rec.srcHeight : 0.0;
                rec.fileSize = fs::file_size(u8path(outPath));
                std::string ext = pathToUtf8(u8path(outPath).extension());
                if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
                rec.format = ext;
                extractor.compute(img, rec, featDir, pathToUtf8(u8path(outPath).stem()));
                if (db.insertImage(rec)) inserted++; else skipped++;
            } catch (...) { skipped++; }
            if ((i+1) % 2000 == 0 || i+1 == files.size()) {
                std::cout << "\r  features " << (i+1) << "/" << files.size()
                          << " (ok:" << inserted << " skip:" << skipped << ")" << std::flush;
            }
        }
        std::cout << std::endl;
    }

    if (normOnly) {
        std::cout << "Normalize only: " << (normDone.load()) << " images written to " << outputDir << std::endl;
        return 0;
    }

    std::cout << std::endl;
    std::cout << "Done: " << inserted << " images indexed, "
              << db.totalCount() << " total in database." << std::endl;

    // 分阶段耗时报告
    auto tNow = std::chrono::steady_clock::now();
    using Ms = std::chrono::milliseconds;
    auto normMs = std::chrono::duration_cast<Ms>(tNormEnd - tNormStart).count();
    auto gpuMs = std::chrono::duration_cast<Ms>(tNow - tGpuStart).count();
    auto totalMs = std::chrono::duration_cast<Ms>(tNow - tBuildStart).count();
    std::cout << "  Phase timing: normalize " << std::fixed << std::setprecision(1) << normMs/1000.0
              << "s | GPU features " << gpuMs/1000.0 << "s";
    if (totalMs > 0 && inserted > 0)
        std::cout << " | throughput " << static_cast<int>(inserted * 1000.0 / totalMs) << " img/s";
    std::cout << std::endl;

    // 构建二进制特征缓存
    tCacheStart = std::chrono::steady_clock::now();
    std::cout << "Building feature cache..." << std::flush;
    if (FeaturePack::buildCache(featDir, db.allRecords()))
        std::cout << " done" << std::endl;
    else
        std::cout << " failed" << std::endl;
    auto cacheSec = std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now() - tCacheStart).count() / 1000.0;
    std::cout << "  Cache build: " << std::fixed << std::setprecision(1) << cacheSec << "s" << std::endl;

    // 记录建库时使用的归一化尺寸，供 mosaic 自适应
    db.setMeta("feature_w", std::to_string(normW));
    db.setMeta("feature_h", std::to_string(normH));
    std::cout << "Feature resolution: " << normW << "x" << normH << std::endl;

    return 0;
}

// ============================================================
// mosaic 子命�?
// ============================================================
static int cmdMosaic(int argc, char* argv[])
{
    std::string inputPath;
    std::string dbPath = "library/mosaicraft.db";
    std::string outputPath = "output/output.jpg";
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
            std::string mode = argv[++i];
            if (mode == "auto" || mode == "stream" || mode == "batch")
                cfg.writeMode = mode;
            else {
                std::cerr << "ERROR: --write-mode must be auto|stream|batch, got: " << mode << std::endl;
                return 1;
            }
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
        else if (arg == "--neighbor-window" && i + 1 < argc)
        {
            cfg.neighborWindow = std::atoi(argv[++i]);
        }
        else if (arg == "--neighbor-penalty" && i + 1 < argc)
        {
            cfg.neighborPenalty = std::atof(argv[++i]);
        }
        else if ((arg == "-U" || arg == "--upscale") && i + 1 < argc)
        {
            cfg.upscale = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "--output-tile" && i + 1 < argc)
        {
            // 格式: 180x320
            const char* val = argv[++i];
            const char* sep = strchr(val, 'x');
            if (sep && sep != val && *(sep+1) != '\0') {
                cfg.nativeTileW = std::max(1, std::atoi(val));
                cfg.nativeTileH = std::max(1, std::atoi(sep + 1));
            } else {
                std::cerr << "ERROR: --output-tile expected WxH format (e.g. 180x320), got: " << val << std::endl;
                return 1;
            }
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
            std::cout << "  -d, --db <path>       Database (default: <output>/mosaicraft.db)" << std::endl;
            std::cout << "  -o, --output <path>   Output path (default: output/output.jpg)" << std::endl;
            std::cout << "  --tile-w <n>          Tile width (default: 9, min: 4)" << std::endl;
            std::cout << "  --tile-h <n>          Tile height (default: 16, min: 4)" << std::endl;
            std::cout << "  --candidates <n>      ANN query candidates (default: 150)" << std::endl;
            std::cout << "  --format <ext>        jpg / png / webp / tiff (default: from extension)" << std::endl;
            std::cout << "  --quality <n>         JPEG/WebP quality 1-100 (default: 100)" << std::endl;
            std::cout << "  --png-level <n>       PNG compression 1-9 (default: 1=fastest)" << std::endl;
            std::cout << "  --write-mode <m>      auto / stream / batch (default: auto)" << std::endl;
            std::cout << "  --out-w <n>           Target output width (0=auto)" << std::endl;
            std::cout << "  --out-h <n>           Target output height (0=auto)" << std::endl;
            std::cout << "  --upscale <n>         Upscale target nX before tiling" << std::endl;
            std::cout << "  --output-tile <WxH>   Output tile pixel size (default: 180x320)" << std::endl;
            std::cout << "  --l-range <f>         L brightness search range (default: 20)" << std::endl;
            std::cout << "  --topn-random <n>     Pick from top-N for variety (default: 10)" << std::endl;
            std::cout << "  --neighbor-window <n> Neighborhood window (default: 0=auto)" << std::endl;
            std::cout << "  --neighbor-penalty <f> Base penalty value (default: 100)" << std::endl;
            std::cout << "  --lab-weight <f>      LAB weight (default: 0.20)" << std::endl;
            std::cout << "  --grid-weight <f>     Grid weight (default: 0.45)" << std::endl;
            std::cout << "  --tiny-weight <f>     TinyImage weight (default: 0.25)" << std::endl;
            std::cout << "  --edge-weight <f>     Edge density weight (default: 0.05)" << std::endl;
            std::cout << "  --lbp-weight <f>      LBP histogram weight (default: 0.05)" << std::endl;
            std::cout << "  --penalty <f>         Use-count penalty (default: 0.01)" << std::endl;
            std::cout << "  --adaptive-weights    Enable adaptive feature weights" << std::endl;
            std::cout << "  --color-adjust        Enable per-tile color jitter (off by default)" << std::endl;
            std::cout << "  --color-strength <f>  Color jitter intensity (default: 0.04)" << std::endl;
            std::cout << "  --analyze             Generate quality report + heatmap + worst tiles" << std::endl;
            std::cout << "  --benchmark           Print per-phase timing breakdown" << std::endl;
            std::cout << "  --cpu                 Force CPU (no GPU)" << std::endl;
            std::cout << "  --tiled               Output tiles as separate files (no size limit)" << std::endl;
            std::cout << "  --deepzoom            Generate Deep Zoom pyramid + HTML viewer" << std::endl;
            std::cout << std::endl;
            std::cout << "  Output modes (choose one): -o (single), --tiled, --deepzoom" << std::endl;
            std::cout << "  Exit status: 0=success 1=param/IO 2=database 3=memory 4=GPU" << std::endl;
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

    // 输出模式互斥检测：单图输出不能同时指定分块/金字塔
    if (cfg.tiledOutput || cfg.deepZoom)
    {
        // 分块模式可以指定 -o 作为输出目录前缀
        // 不报错，但提醒用户
        if (!outputPath.empty() && outputPath.find('_') == std::string::npos)
            std::cout << "  Note: --tiled/--deepzoom outputs to " << outputPath << "_files/" << std::endl;
    }

    MosaicEngine engine;
    bool ok = engine.generate(inputPath, resolveDbPath(dbPath), outputPath, cfg);

    return ok ? 0 : 1;
}

// ============================================================
// inspect 子命�?�?特征诊断
// ============================================================
static int cmdInspect(int argc, char* argv[])
{
    std::string imagePath;
    std::string dbPath = "library/mosaicraft.db";

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

    // 加载并归丢�化图�?
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

    // 查�??数据�?
    Database db(resolveDbPath(dbPath));
    if (db.isOpen())
    {
        int total = db.totalCount();
        auto candidates = db.queryIdsByLRange(tL - 20, tL + 20, 200, false);
        std::cout << "\nDatabase: " << total << " images" << std::endl;
        std::cout << "L-range [" << (tL - 20) << ", " << (tL + 20) << "]: "
                  << candidates.size() << " candidates"
                  << (total > 0 ? " (" + std::to_string(100 * candidates.size() / total) + "%)" : "")
                  << std::endl;

        // 统�?? L 分布（从 allRecords�?
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

        // 覆盖�?
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
// db-stats 子命令：输出图库统�??信息
// ============================================================
static int cmdDbStats(int argc, char* argv[])
{
    std::string dbPath = "library/mosaicraft.db";

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-d" || arg == "--db") && i + 1 < argc)
            dbPath = argv[++i];
    }

    Database db(resolveDbPath(dbPath));
    if (!db.isOpen())
    {
        std::cerr << "ERROR: Cannot open database: " << dbPath << std::endl;
        return EXIT_ERR_DB;
    }

    auto all = db.allRecords();
    int total = static_cast<int>(all.size());
    if (total == 0)
    {
        std::cout << "Database is empty." << std::endl;
        return 0;
    }

    std::string fw = db.getMeta("feature_w");
    std::string fh = db.getMeta("feature_h");
    if (!fw.empty() && !fh.empty())
        std::cout << "Normalized size: " << fw << "x" << fh << std::endl;

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

    // �??��分布
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

    // �??��直方�?(8 bins: 0-32, 32-64, 64-96, 96-128, 128-160, 160-192, 192-224, 224-256)
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

    // 覆盖缺口分析（基于已有统计量�?
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
static int cmdDbPurge(int argc, char* argv[])
{
    std::string dbPath = "library/mosaicraft.db";
    bool forceMode = false;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-d" || arg == "--db") && i + 1 < argc)
            dbPath = argv[++i];
        else if (arg == "-y" || arg == "--yes" || arg == "--force")
            forceMode = true;
    }

    Database db(resolveDbPath(dbPath));
    if (!db.isOpen()) { std::cerr << "ERROR: Cannot open DB" << std::endl; return 2; }

    auto all = db.allRecords();
    int total = static_cast<int>(all.size());
    int removed = 0;

    // 先检查有多少孤儿记录，再决定是否删除
    std::vector<int> orphanIds;
    for (const auto& r : all)
    {
        if (!r.filePath.empty() && !std::filesystem::exists(u8path(r.filePath)))
            orphanIds.push_back(r.id);
    }
    removed = static_cast<int>(orphanIds.size());

    if (removed == 0)
    {
        std::cout << "No orphan records found." << std::endl;
        return 0;
    }

    if (!forceMode)
    {
#ifdef _WIN32
        bool isTTY = _isatty(_fileno(stdin));
#else
        bool isTTY = isatty(fileno(stdin));
#endif
        if (!isTTY)
        {
            std::cerr << "ERROR: " << removed << " orphan records. Use -y to purge." << std::endl;
            return 1;
        }
        std::cout << "Found " << removed << " orphan records. Purge? [y/N] " << std::flush;
        char answer = static_cast<char>(std::getchar());
        if (answer != 'y' && answer != 'Y') { std::cout << "Aborted." << std::endl; return 0; }
    }

    // 确认后才执行删除
    for (int id : orphanIds)
    {
        // 找到对应记录以删除关联文件
        for (const auto& r : all)
        {
            if (r.id != id) continue;
            if (!r.tinyPath.empty() && std::filesystem::exists(u8path(r.tinyPath)))
                std::filesystem::remove(u8path(r.tinyPath));
            if (!r.histPath.empty() && std::filesystem::exists(u8path(r.histPath)))
                std::filesystem::remove(u8path(r.histPath));
            break;
        }
        db.removeImage(id);
    }

    std::cout << "Purged " << removed << " / " << total << " orphan records." << std::endl;
    if (removed > 0)
        std::cout << "  Note: delete normalized/features/*.bin and lib.ann, then re-run mosaic to rebuild caches." << std::endl;
    return 0;
}

// ============================================================
// db-usage ������鿴ͼƬʹ��ͳ��
// ============================================================
static int cmdDbUsage(int argc, char* argv[])
{
    std::string dbPath = "library/mosaicraft.db";
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

    Database db(resolveDbPath(dbPath));
    if (!db.isOpen()) { std::cerr << "ERROR: Cannot open DB" << std::endl; return EXIT_ERR_DB; }

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

    // --unused: �г���δʹ�õ�ͼƬ
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
                if (unused < 50)  // ֻ��ʾǰ50��
                    std::cout << "  unused: id=" << r.id << "  " << r.filePath << "\n";
                unused++;
            }
        }
        std::cout << "\n  Total unused: " << unused << " / " << allRecs.size()
                  << " (" << std::fixed << std::setprecision(1) << (100.0*unused/allRecs.size()) << "%)\n";
        if (unused > 0 && exportDir.empty())
            std::cout << "  Tip: use --export <dir> to export used images\n";
    }

    // ��������ȫ�� tile ʹ�������򣬸��ƹ�һ��ͼ������Ŀ¼
    if (!exportDir.empty())
    {
        auto allRecs = db.allRecords();
        std::unordered_map<int, std::string> pathMap;
        for (const auto& r : allRecs) pathMap[r.id] = r.filePath;

        auto allUsed = db.topUsedImages(999999);  // ȫ��
        // �� total_tiles �������У�topUsedImages ���ص��ǰ� total_runs ������Ҫ�����ţ�
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
// db-health �����ͼ�⽡�������
// ============================================================
static int cmdDbHealth(int argc, char* argv[])
{
    std::string dbPath = "library/mosaicraft.db";
    for (int i = 2; i < argc; ++i)
        if ((std::string(argv[i]) == "-d" || std::string(argv[i]) == "--db") && i + 1 < argc)
            dbPath = argv[++i];

    Database db(resolveDbPath(dbPath));
    if (!db.isOpen()) { std::cerr << "ERROR: Cannot open DB" << std::endl; return EXIT_ERR_DB; }

    auto all = db.allRecords();
    int total = static_cast<int>(all.size());
    if (total == 0) { std::cout << "Database is empty.\n"; return 0; }

    std::cout << "=== Database Health ===\n\n";
    std::cout << "Images: " << total << "\n";

    // ������ ���ȸ��� ������
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

    // ������ ɫ�򸲸� ������
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

    // ������ ʹ��ͳ�� ������
    auto used = db.topUsedImages(999999);
    int usedCount = static_cast<int>(used.size());
    int unusedCount = total - usedCount;

    std::cout << "\n--- Usage ---\n";
    std::cout << "  Used:   " << usedCount << " (" << std::setprecision(1) << (100.0*usedCount/total) << "%)\n";
    std::cout << "  Unused: " << unusedCount << " (" << (100.0*unusedCount/total) << "%)\n";
    if (unusedCount > total / 2)
        std::cout << "  WARN: >50% images never used - consider pruning\n";

    // ������ �ȵ㼯�ж� ������
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

    // ������ ���� ������
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

    // 交互式终端退出前暂停，双击exe时不闪退
    struct AutoPause {
        ~AutoPause() {
#ifdef _WIN32
            if (_isatty(_fileno(stdin))) {
                std::cout << std::endl << "Press any key to exit..." << std::flush;
                _getch();
            }
#else
            if (isatty(fileno(stdin))) {
                std::cout << std::endl << "Press any key to exit..." << std::flush;
                getchar();
            }
#endif
        }
    } autoPause;

    // Windows: argv ʹ��ϵͳ ANSI ���루����ϵͳΪ GBK����תΪ UTF-8 ԭ���滻
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
