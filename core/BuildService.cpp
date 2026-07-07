#include "BuildService.h"

#include "Database.h"
#include "FeatureExtractor.h"
#include "FeaturePack.h"
#include "FeatureUtils.h"
#include "ImageNormalizer.h"
#include "UnicodeIO.h"
#include "compute/CudaBackend.h"
#ifdef MOSAICRAFT_CUDA
#include "compute/FeatureExtractorCuda.h"
#endif

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mosaicraft
{

namespace fs = std::filesystem;

namespace
{

constexpr int EXIT_ERR_DB = 2;

static std::string hashMatForBuild(const cv::Mat& mat)
{
    if (mat.empty()) return "0";
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
    return std::to_string(hash);
}

static bool stdinIsTty()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

} // namespace

ServiceResult BuildService::run(const BuildRequest& request) const
{
    if (request.inputDir.empty())
    {
        return ServiceResult::failure(1, "inputDir is required");
    }

    std::string outputDir = request.outputDir.empty() ? "library" : request.outputDir;
    std::string dbPath = request.dbPath.empty() ? "mosaicraft.db" : request.dbPath;
    if (dbPath == "mosaicraft.db")
    {
        dbPath = outputDir + "/mosaicraft.db";
    }

    std::error_code ec;
    fs::create_directories(u8path(outputDir), ec);
    if (ec)
    {
        return ServiceResult::failure(1, "cannot create output directory: " + outputDir);
    }

    Database db(resolveDbPathForService(dbPath));
    if (!db.isOpen())
    {
        return ServiceResult::failure(EXIT_ERR_DB, "cannot open database: " + dbPath);
    }

    if (!db.createTables())
    {
        return ServiceResult::failure(EXIT_ERR_DB, "cannot create database tables");
    }

    if (!request.appendMode && !request.normalizeOnly)
    {
        int existingCount = db.totalCount();
        if (existingCount > 0 && !request.forceMode)
        {
            if (!request.allowPrompt || !stdinIsTty())
            {
                return ServiceResult::failure(
                    1,
                    "DB has " + std::to_string(existingCount) +
                    " records. Use forceMode to overwrite.");
            }
            std::cout << "Database has " << existingCount
                      << " records. Overwrite? [y/N] " << std::flush;
            char answer = static_cast<char>(std::getchar());
            if (answer != 'y' && answer != 'Y')
            {
                return ServiceResult::success("aborted");
            }
        }
    }

    std::string featDir = outputDir + "/features";
    fs::create_directories(u8path(featDir), ec);

    const std::vector<std::string> exts = {
        ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tiff", ".tif"
    };

    std::vector<std::string> files;
    auto addEntry = [&](const auto& entry) {
        if (!entry.is_regular_file()) return;
        std::string ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool supported = false;
        for (const auto& e : exts) {
            if (ext == e) {
                supported = true;
                break;
            }
        }
        if (supported) files.push_back(pathToUtf8(entry.path()));
    };

    if (request.recursive)
    {
        for (const auto& entry : fs::recursive_directory_iterator(u8path(request.inputDir), ec))
        {
            addEntry(entry);
        }
    }
    else
    {
        for (const auto& entry : fs::directory_iterator(u8path(request.inputDir), ec))
        {
            addEntry(entry);
        }
    }

    std::sort(files.begin(), files.end());
    std::cout << "Found " << files.size() << " image(s) in " << request.inputDir << std::endl;

    if (files.empty())
    {
        return ServiceResult::failure(1, "no supported images found");
    }

    auto tBuildStart = std::chrono::steady_clock::now();
    auto tNormStart = tBuildStart;
    std::chrono::steady_clock::time_point tGpuStart = tBuildStart;
    std::chrono::steady_clock::time_point tCacheStart = tBuildStart;
    std::chrono::steady_clock::time_point tNormEnd = tBuildStart;
    ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight);
    std::vector<std::string> outPaths(files.size());
    std::vector<bool> okFlags(files.size(), false);
    std::atomic<int> normDone{0};
    int normThreads = request.threads > 0
        ? std::max(1, request.threads)
        : static_cast<int>(std::thread::hardware_concurrency());
    if (request.threads <= 0 && normThreads < 2) normThreads = 2;

    if (request.normalizeOnly)
    {
        std::cout << "Normalize only mode" << std::endl;
    }

    struct NormItem { cv::Mat img; std::string outPath; std::string stem; };
    std::queue<NormItem> normQueue;
    std::mutex queueMtx;
    std::condition_variable queueCV;
    std::condition_variable queueFullCV;
    constexpr int MAX_QUEUE = 512;
    std::atomic<bool> normPhaseDone{false};
    std::atomic<int> gpuDone{0};

    FeatureExtractor extractor;
    bool gpuOk = false;
#ifdef MOSAICRAFT_CUDA
    gpuOk = cuda::isCudaAvailable();
#endif
    constexpr int GPU_BATCH = 256;
    int inserted = 0;
    int skipped = 0;

    std::thread gpuThread;
    if (gpuOk && !request.normalizeOnly)
    {
        std::cout << "Normalizing + Features: GPU (batch " << GPU_BATCH << ", pipelined)" << std::endl;
        gpuThread = std::thread([&]() {
            bool firstBatch = true;
            std::vector<cv::Mat> imgs; imgs.reserve(GPU_BATCH);
            std::vector<ImageRecord> recs; recs.reserve(GPU_BATCH);
            std::vector<std::string> stems; stems.reserve(GPU_BATCH);
            auto flush = [&]() {
                if (imgs.empty()) return;
                if (firstBatch) {
                    tGpuStart = std::chrono::steady_clock::now();
                    firstBatch = false;
                }
#ifdef MOSAICRAFT_CUDA
                int done = cuda::extractBatch(imgs, recs, featDir, stems);
#else
                int done = 0;
#endif
                if (done <= 0) {
                    for (size_t bi = 0; bi < imgs.size(); ++bi) {
                        extractor.compute(imgs[bi], recs[bi], featDir, stems[bi]);
                    }
                }
                else if (done != static_cast<int>(imgs.size())) {
                    for (size_t bi = static_cast<size_t>(std::max(0, done)); bi < imgs.size(); ++bi) {
                        extractor.compute(imgs[bi], recs[bi], featDir, stems[bi]);
                    }
                }
                db.beginTransaction();
                for (size_t bi = 0; bi < recs.size(); ++bi) {
                    if (recs[bi].filePath.empty()) continue;
                    if (db.insertImage(recs[bi])) inserted++;
                    else skipped++;
                }
                db.commitTransaction();
                gpuDone += static_cast<int>(imgs.size());
                int gd = gpuDone.load();
                if (gd % 2000 == 0 || gd == static_cast<int>(files.size())) {
                    std::cout << "\r  gpu feature " << gd << "/" << files.size() << std::flush;
                }
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
                    queueFullCV.notify_one();
                }
                if (item.img.empty()) continue;
                std::string hash = hashMatForBuild(item.img);
                if (db.existsByHash(hash)) {
                    std::error_code removeEc;
                    fs::remove(u8path(item.outPath), removeEc);
                    skipped++;
                    continue;
                }
                ImageRecord rec;
                rec.filePath = item.outPath;
                rec.fileHash = hash;
                rec.srcWidth = item.img.cols;
                rec.srcHeight = item.img.rows;
                rec.aspectRatio = (rec.srcHeight > 0) ? (double)rec.srcWidth / rec.srcHeight : 0.0;
                std::error_code sizeEc;
                rec.fileSize = static_cast<int64_t>(fs::file_size(u8path(item.outPath), sizeEc));
                if (sizeEc)
                {
                    std::cerr << "WARN: cannot stat output file: " << item.outPath
                              << " (" << sizeEc.message() << ")" << std::endl;
                    skipped++;
                    continue;
                }
                std::string ext = pathToUtf8(u8path(item.outPath).extension());
                if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
                rec.format = ext;
                imgs.push_back(item.img);
                recs.push_back(rec);
                stems.push_back(item.stem);
                if ((int)imgs.size() >= GPU_BATCH) flush();
            }
        });
    }
    else if (!request.normalizeOnly)
    {
        std::cout << "Features: CPU" << std::endl;
    }

    std::error_code equivEc;
    bool inputIsOutput = false;
    try {
        inputIsOutput = fs::equivalent(u8path(request.inputDir), u8path(outputDir), equivEc);
    } catch (...) {
        inputIsOutput = false;
    }

    if (!inputIsOutput)
    {
        if (!gpuOk || request.normalizeOnly) {
            std::cout << "Normalizing (" << normThreads << " threads)..." << std::endl;
        }
        std::atomic<size_t> nextFileIdx{0};
        std::atomic<size_t> nextNameIdx = request.appendMode
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
                    fs::path outPath = u8path(outputDir) / name;
                    outPaths[fi] = pathToUtf8(outPath);
                    try {
                        cv::Mat result = normalizer.processToMat(inPath);
                        if (result.empty()) continue;
                        std::string outExt = pathToUtf8(outPath.extension());
                        std::vector<int> writeParams;
                        if (outExt == ".jpg" || outExt == ".jpeg") {
                            writeParams = {cv::IMWRITE_JPEG_QUALITY, 90};
                        } else if (outExt == ".png") {
                            writeParams = {cv::IMWRITE_PNG_COMPRESSION, 9};
                        }
                        imwriteUnicode(pathToUtf8(outPath), result, writeParams);
                        okFlags[fi] = true;
                        if (gpuOk && !request.normalizeOnly) {
                            std::unique_lock<std::mutex> lk(queueMtx);
                            queueFullCV.wait(lk, [&]{ return normQueue.size() < MAX_QUEUE || normPhaseDone.load(); });
                            normQueue.push({result.clone(), pathToUtf8(outPath), pathToUtf8(outPath.stem())});
                            lk.unlock();
                            queueCV.notify_one();
                        }
                    } catch (...) {}
                    int d = ++normDone;
                    if (d % 200 == 0 || d == (int)files.size()) {
                        std::cout << "\r  normalize " << d << "/" << files.size()
                                  << "                    " << std::flush;
                    }
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

    normPhaseDone.store(true);
    queueCV.notify_all();
    if (gpuOk && !request.normalizeOnly)
    {
        gpuThread.join();
        std::cout << "  GPU done: " << gpuDone.load() << std::endl;
    }

    if (!gpuOk && !request.normalizeOnly)
    {
        std::cout << "Features: CPU" << std::endl;
        for (size_t i = 0; i < files.size(); ++i) {
            if (!okFlags[i]) { skipped++; continue; }
            const std::string& outPath = outPaths[i];
            try {
                cv::Mat img = imreadUnicode(outPath, cv::IMREAD_COLOR);
                if (img.empty()) { skipped++; continue; }
                std::string hash = hashMatForBuild(img);
                if (db.existsByHash(hash)) {
                    std::error_code removeEc;
                    fs::remove(u8path(outPath), removeEc);
                    if (removeEc)
                    {
                        std::cerr << "WARN: cannot remove duplicate output file: " << outPath
                                  << " (" << removeEc.message() << ")" << std::endl;
                    }
                    skipped++;
                    continue;
                }
                ImageRecord rec;
                rec.filePath = outPath;
                rec.fileHash = hash;
                rec.srcWidth = img.cols;
                rec.srcHeight = img.rows;
                rec.aspectRatio = (rec.srcHeight > 0) ? (double)rec.srcWidth / rec.srcHeight : 0.0;
                std::error_code sizeEc;
                rec.fileSize = static_cast<int64_t>(fs::file_size(u8path(outPath), sizeEc));
                if (sizeEc)
                {
                    std::cerr << "WARN: cannot stat output file: " << outPath
                              << " (" << sizeEc.message() << ")" << std::endl;
                    skipped++;
                    continue;
                }
                std::string ext = pathToUtf8(u8path(outPath).extension());
                if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
                rec.format = ext;
                extractor.compute(img, rec, featDir, pathToUtf8(u8path(outPath).stem()));
                if (db.insertImage(rec)) inserted++;
                else skipped++;
            } catch (...) {
                skipped++;
            }
            if ((i + 1) % 2000 == 0 || i + 1 == files.size()) {
                std::cout << "\r  features " << (i + 1) << "/" << files.size()
                          << " (ok:" << inserted << " skip:" << skipped << ")   " << std::flush;
            }
        }
        std::cout << std::endl;
    }

    if (request.normalizeOnly)
    {
        std::cout << "Normalize only: " << normDone.load()
                  << " images written to " << outputDir << std::endl;
        return ServiceResult::success("normalize completed");
    }

    std::cout << std::endl;
    std::cout << "Done: " << inserted << " images indexed, "
              << db.totalCount() << " total in database." << std::endl;

    auto tNow = std::chrono::steady_clock::now();
    using Ms = std::chrono::milliseconds;
    auto normMs = std::chrono::duration_cast<Ms>(tNormEnd - tNormStart).count();
    auto gpuMs = std::chrono::duration_cast<Ms>(tNow - tGpuStart).count();
    auto totalMs = std::chrono::duration_cast<Ms>(tNow - tBuildStart).count();
    std::cout << "  Phase timing: normalize " << std::fixed << std::setprecision(1) << normMs / 1000.0
              << "s | GPU features " << gpuMs / 1000.0 << "s";
    if (totalMs > 0 && inserted > 0) {
        std::cout << " | throughput " << static_cast<int>(inserted * 1000.0 / totalMs) << " img/s";
    }
    std::cout << std::endl;

    tCacheStart = std::chrono::steady_clock::now();
    std::cout << "Building feature cache..." << std::flush;
    if (FeaturePack::buildCache(featDir, db.allRecords())) {
        std::cout << " done" << std::endl;
    } else {
        std::cout << " failed" << std::endl;
    }
    auto cacheSec = std::chrono::duration_cast<Ms>(
        std::chrono::steady_clock::now() - tCacheStart).count() / 1000.0;
    std::cout << "  Cache build: " << std::fixed << std::setprecision(1) << cacheSec << "s" << std::endl;

    db.setMeta("feature_w", std::to_string(request.normalizeWidth));
    db.setMeta("feature_h", std::to_string(request.normalizeHeight));
    std::cout << "Feature resolution: " << request.normalizeWidth << "x" << request.normalizeHeight << std::endl;

    return ServiceResult::success("build completed");
}

} // namespace mosaicraft
