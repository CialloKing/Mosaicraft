#include "BuildService.h"

#include "Database.h"
#include "FeatureExtractor.h"
#include "FeaturePack.h"
#include "FeatureUtils.h"
#include "ImageNormalizer.h"
#include "LibraryColorOrder.h"
#include "RequestValidation.h"
#include "UnicodeIO.h"
#include "WorkerPool.h"
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
#include <set>
#include <exception>

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
    std::string validationError;
    if (!validateBuildRequest(request, validationError))
    {
        return ServiceResult::failure(1, validationError);
    }
    if (request.inputDir.empty())
    {
        return ServiceResult::failure(1, "inputDir is required");
    }
    std::lock_guard<std::mutex> pipelineLock(pipelineMutex());

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

    Database db(resolveDbPathForService(dbPath), Database::OpenMode::Build);
    if (!db.isOpen())
    {
        return ServiceResult::failure(EXIT_ERR_DB, "cannot open database: " + dbPath + "; " + db.error());
    }

    if (!db.createTables())
    {
        return ServiceResult::failure(EXIT_ERR_DB, "cannot create database tables");
    }

    std::string recoveryError;
    if (!recoverLibraryColorOrder(db, recoveryError))
    {
        return ServiceResult::failure(EXIT_ERR_DB, recoveryError);
    }
    if (!fs::is_directory(u8path(request.inputDir), ec))
    {
        return ServiceResult::failure(1, "cannot read input directory: " + request.inputDir);
    }

    if (!request.appendMode && !request.normalizeOnly)
    {
        int existingCount = db.totalCount();
        if (existingCount > 0 && !request.forceMode)
        {
            if (!request.allowPrompt || !stdinIsTty())
            {
                return ServiceResult::failure(1, "DB has " + std::to_string(existingCount) +
                                                     " records. Use appendMode or forceMode to continue building.");
            }
            std::cout << "Database has " << existingCount
                      << " records. Continue building and renumber the entire library by color? [y/N] " << std::flush;
            char answer = static_cast<char>(std::getchar());
            if (answer != 'y' && answer != 'Y')
            {
                return ServiceResult::success("aborted");
            }
        }
    }

    std::string featDir = outputDir + "/features";
    fs::create_directories(u8path(featDir), ec);
    auto existingRecords = db.allRecords();
    if (!existingRecords.empty() && !request.normalizeOnly)
    {
        if (db.getMeta("feature_w") != std::to_string(request.normalizeWidth) ||
            db.getMeta("feature_h") != std::to_string(request.normalizeHeight))
        {
            return ServiceResult::failure(1, "feature dimensions differ; use a new library directory/database");
        }
        FeatureExtractor refresh;
        for (auto record : existingRecords)
        {
            if (record.featureVersion == FeatureExtractor::featureVersion())
            {
                continue;
            }
            auto image = imreadUnicode(record.filePath, cv::IMREAD_COLOR);
            if (image.empty())
            {
                return ServiceResult::failure(1, "restore missing library files or run db-purge before rebuilding");
            }
            // 保留记录 ID、使用次数与原文件；刷新旧特征，同时修正旧 CUDA 的编码前哈希。
            record.fileHash = hashMatForBuild(image);
            refresh.compute(image, record, featDir, "id_" + std::to_string(record.id));
            if (!db.insertImage(record))
            {
                return ServiceResult::failure(2, "cannot refresh legacy features");
            }
        }
    }

    // 入库前持久化尺寸；首次建库被中断后，已有记录仍可用同样尺寸继续追加。
    if (!request.normalizeOnly &&
        (!db.setMeta("feature_w", std::to_string(request.normalizeWidth)) ||
         !db.setMeta("feature_h", std::to_string(request.normalizeHeight))))
    {
        return ServiceResult::failure(EXIT_ERR_DB, "cannot save library dimensions");
    }

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

    if (ec)
    {
        return ServiceResult::failure(1, "cannot scan input directory: " + ec.message());
    }
    std::sort(files.begin(), files.end());
    std::cout << "Found " << files.size() << " image(s) in " << request.inputDir << std::endl;

    if (files.empty() && (existingRecords.empty() || request.normalizeOnly))
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
    std::vector<uint8_t> okFlags(files.size(), 0);
    bool inputIsOutput = fs::equivalent(u8path(request.inputDir), u8path(outputDir), ec);
    ec.clear();
    // 新图片先用临时名字，避免失败文件和旧编号阻碍最终全库颜色排序。
    std::set<fs::path> reservedPaths;
    for (const auto& record : db.allRecords())
    {
        reservedPaths.insert(fs::absolute(u8path(record.filePath)).lexically_normal());
    }
    size_t nameIndex = 0;
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (inputIsOutput)
        {
            outPaths[i] = files[i];
            continue;
        }
        fs::path candidate;
        do
        {
            char name[64];
            auto extension = pathToUtf8(u8path(files[i]).extension());
            snprintf(name, sizeof(name), request.normalizeOnly ? "%06zu%s" : ".mosaicraft-new-%06zu%s",
                     nameIndex++, extension.c_str());
            candidate = u8path(outputDir) / name;
        } while (fs::exists(candidate) || reservedPaths.count(fs::absolute(candidate).lexically_normal()));
        reservedPaths.insert(fs::absolute(candidate).lexically_normal());
        outPaths[i] = pathToUtf8(candidate);
    }
    std::atomic<int> normDone{0};
    int normThreads = request.threads > 0
        ? std::max(1, request.threads)
        : static_cast<int>(std::thread::hardware_concurrency());
    if (request.threads <= 0 && normThreads < 2) normThreads = 2;

    if (request.normalizeOnly)
    {
        std::cout << "Normalize only mode" << std::endl;
    }

    struct NormItem
    {
        cv::Mat img;
        std::string outPath;
    };
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
    gpuOk = cuda::isCudaAvailable() && ((request.normalizeWidth == 180 && request.normalizeHeight == 320) ||
                                        (request.normalizeWidth == 320 && request.normalizeHeight == 180) ||
                                        (request.normalizeWidth == 360 && request.normalizeHeight == 640) ||
                                        (request.normalizeWidth == 640 && request.normalizeHeight == 360));
#endif
    constexpr int GPU_BATCH = 256;
    int inserted = 0;
    int skipped = 0;

    std::thread gpuThread;
    std::exception_ptr gpuError;
    if (gpuOk && !request.normalizeOnly)
    {
        std::cout << "Normalizing + Features: GPU (batch " << GPU_BATCH << ", pipelined)" << std::endl;
        gpuThread = std::thread(
            [&]()
            {
                try
                {
                    cuda::FeatureWorkspace featureWorkspace;
                    bool firstBatch = true;
                    std::set<std::string> pendingHashes;
                    std::vector<cv::Mat> imgs;
                    imgs.reserve(GPU_BATCH);
                    std::vector<ImageRecord> recs;
                    recs.reserve(GPU_BATCH);
                    std::vector<std::string> stems;
                    stems.reserve(GPU_BATCH);
                    auto flush = [&]()
                    {
                        if (imgs.empty())
                        {
                            return;
                        }
                        if (firstBatch)
                        {
                            tGpuStart = std::chrono::steady_clock::now();
                            firstBatch = false;
                        }
#ifdef MOSAICRAFT_CUDA
                        int done = cuda::extractBatch(imgs, recs, featDir, stems, &featureWorkspace);
#else
                        int done = 0;
#endif
                        if (done <= 0)
                        {
                            for (size_t bi = 0; bi < imgs.size(); ++bi)
                            {
                                extractor.compute(imgs[bi], recs[bi], featDir, stems[bi]);
                            }
                        }
                        else if (done != static_cast<int>(imgs.size()))
                        {
                            for (size_t bi = static_cast<size_t>(std::max(0, done)); bi < imgs.size(); ++bi)
                            {
                                extractor.compute(imgs[bi], recs[bi], featDir, stems[bi]);
                            }
                        }
                        if (!db.beginTransaction())
                        {
                            throw std::runtime_error("cannot begin build transaction");
                        }
                        for (size_t bi = 0; bi < recs.size(); ++bi)
                        {
                            if (recs[bi].filePath.empty())
                            {
                                continue;
                            }
                            if (db.insertImage(recs[bi]))
                            {
                                inserted++;
                            }
                            else
                            {
                                throw std::runtime_error("cannot insert image into library");
                            }
                        }
                        if (!db.commitTransaction())
                        {
                            throw std::runtime_error("cannot commit build transaction");
                        }
                        gpuDone += static_cast<int>(imgs.size());
                        int gd = gpuDone.load();
                        if (gd % 2000 == 0 || gd == static_cast<int>(files.size()))
                        {
                            std::cout << "\r  gpu feature " << gd << "/" << files.size() << std::flush;
                        }
                        imgs.clear();
                        recs.clear();
                        stems.clear();
                    };
                    while (true)
                    {
                        NormItem item;
                        {
                            std::unique_lock<std::mutex> lk(queueMtx);
                            queueCV.wait(lk,
                                         [&]
                                         {
                                             return !normQueue.empty() || normPhaseDone.load();
                                         });
                            if (normQueue.empty() && normPhaseDone.load())
                            {
                                flush();
                                break;
                            }
                            if (normQueue.empty())
                            {
                                continue;
                            }
                            item = std::move(normQueue.front());
                            normQueue.pop();
                            lk.unlock();
                            queueFullCV.notify_one();
                        }
                        if (item.img.empty())
                        {
                            continue;
                        }
                        std::string hash = hashMatForBuild(item.img);
                        if (db.existsByHash(hash) || !pendingHashes.insert(hash).second)
                        {
                            if (!inputIsOutput)
                            {
                                std::error_code removeEc;
                                fs::remove(u8path(item.outPath), removeEc);
                            }
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
                            std::cerr << "WARN: cannot stat output file: " << item.outPath << " (" << sizeEc.message()
                                      << ")" << std::endl;
                            skipped++;
                            continue;
                        }
                        std::string ext = pathToUtf8(u8path(item.outPath).extension());
                        if (!ext.empty() && ext[0] == '.')
                        {
                            ext = ext.substr(1);
                        }
                        rec.format = ext;
                        imgs.push_back(item.img);
                        recs.push_back(rec);
                        // 特征文件不跟随数字编号，追加重排也不会覆盖其他图片的特征。
                        stems.push_back("hash_" + hash);
                        if ((int)imgs.size() >= GPU_BATCH)
                        {
                            flush();
                        }
                    }
                }
                catch (...)
                {
                    // 后台异常必须回传；唤醒生产者后退出，避免队列满时永远等待。
                    gpuError = std::current_exception();
                    normPhaseDone.store(true);
                    queueFullCV.notify_all();
                }
            });
    }
    else if (!request.normalizeOnly)
    {
        std::cout << "Features: CPU" << std::endl;
    }

    if (!inputIsOutput)
    {
        if (!gpuOk || request.normalizeOnly) {
            std::cout << "Normalizing (" << normThreads << " threads)..." << std::endl;
        }
        std::atomic<size_t> nextFileIdx{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < normThreads; ++t) {
            workers.emplace_back([&]() {
                for (;;) {
                    size_t fi = nextFileIdx.fetch_add(1);
                    if (fi >= files.size()) break;
                    const std::string& inPath = files[fi];
                    fs::path outPath = u8path(outPaths[fi]);
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
                        if (!imwriteUnicode(pathToUtf8(outPath), result, writeParams))
                        {
                            continue;
                        }
                        okFlags[fi] = true;
                        if (gpuOk && !request.normalizeOnly) {
                            // JPEG 编解码会改变像素，两种后端都以落盘图像计算哈希和特征。
                            result = imreadUnicode(pathToUtf8(outPath), cv::IMREAD_COLOR);
                            if (result.empty())
                            {
                                okFlags[fi] = 0;
                                continue;
                            }
                            std::unique_lock<std::mutex> lk(queueMtx);
                            queueFullCV.wait(lk, [&]{ return normQueue.size() < MAX_QUEUE || normPhaseDone.load(); });
                            if (normPhaseDone.load())
                            {
                                break;
                            }
                            normQueue.push({result.clone(), pathToUtf8(outPath)});
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
        for (size_t i = 0; i < files.size(); ++i)
        {
            okFlags[i] = 1;
            if (gpuOk && !request.normalizeOnly)
            {
                auto image = imreadUnicode(files[i], cv::IMREAD_COLOR);
                std::unique_lock<std::mutex> lock(queueMtx);
                queueFullCV.wait(lock,
                                 [&]
                                 {
                                     return normQueue.size() < MAX_QUEUE || normPhaseDone.load();
                                 });
                if (normPhaseDone.load())
                {
                    break;
                }
                normQueue.push({image, files[i]});
                lock.unlock();
                queueCV.notify_one();
            }
        }
    }

    normPhaseDone.store(true);
    queueCV.notify_all();
    if (gpuOk && !request.normalizeOnly)
    {
        gpuThread.join();
        if (gpuError)
        {
            try
            {
                std::rethrow_exception(gpuError);
            }
            catch (const std::exception& error)
            {
                return ServiceResult::failure(1, error.what());
            }
        }
        std::cout << "  GPU done: " << gpuDone.load() << std::endl;
    }

    if (!gpuOk && !request.normalizeOnly)
    {
        tGpuStart = std::chrono::steady_clock::now();
        constexpr size_t pixelBudget = 64ULL * 1024 * 1024;
        const size_t pixelsPerImage = static_cast<size_t>(request.normalizeWidth) * request.normalizeHeight * 3;
        // 同目录输入的尺寸可能不同，单张处理保证超预算图片独占批次。
        const size_t batchLimit =
            inputIsOutput ? 1 : std::min<size_t>(256, std::max<size_t>(1, pixelBudget / pixelsPerImage));
        WorkerPool workers(request.threads > 0 ? static_cast<unsigned>(request.threads) : 0);
        std::cout << "Features: CPU (ordered batches, max " << batchLimit << " images, 64 MiB pixels)" << std::endl;
        for (size_t first = 0; first < files.size(); first += batchLimit)
        {
            const size_t count = std::min(batchLimit, files.size() - first);
            std::vector<cv::Mat> images(count);
            std::vector<ImageRecord> records(count);
            std::vector<uint8_t> ready(count, 0);
            std::vector<std::string> errors(count);
            workers.run(
                count,
                [&](size_t i)
                {
                    if (!okFlags[first + i])
                    {
                        return;
                    }
                    try
                    {
                        images[i] = imreadUnicode(outPaths[first + i], cv::IMREAD_COLOR);
                        if (!inputIsOutput && !images[i].empty() &&
                            (images[i].cols != request.normalizeWidth || images[i].rows != request.normalizeHeight))
                        {
                            throw std::runtime_error("normalized image dimensions changed before feature extraction");
                        }
                    }
                    catch (const std::exception &error)
                    {
                        errors[i] = error.what();
                        images[i].release();
                    }
                });
            std::set<std::string> pendingHashes;
            for (size_t i = 0; i < count; ++i)
            {
                if (images[i].empty())
                {
                    ++skipped;
                    continue;
                }
                const auto &outPath = outPaths[first + i];
                const auto hash = hashMatForBuild(images[i]);
                if (db.existsByHash(hash) || !pendingHashes.insert(hash).second)
                {
                    if (!inputIsOutput)
                    {
                        std::error_code removeError;
                        fs::remove(u8path(outPath), removeError);
                        if (removeError)
                        {
                            std::cerr << "WARN: cannot remove duplicate output file: " << outPath << std::endl;
                        }
                    }
                    images[i].release();
                    ++skipped;
                    continue;
                }
                auto &record = records[i];
                record.filePath = outPath;
                record.fileHash = hash;
                record.srcWidth = images[i].cols;
                record.srcHeight = images[i].rows;
                record.aspectRatio = static_cast<double>(record.srcWidth) / record.srcHeight;
                std::error_code sizeError;
                record.fileSize = static_cast<int64_t>(fs::file_size(u8path(outPath), sizeError));
                if (sizeError)
                {
                    images[i].release();
                    ++skipped;
                    continue;
                }
                record.format = pathToUtf8(u8path(outPath).extension());
                if (!record.format.empty())
                {
                    record.format.erase(0, 1);
                }
                ready[i] = 1;
            }
            workers.run(count,
                        [&](size_t i)
                        {
                            if (!ready[i])
                            {
                                return;
                            }
                            try
                            {
                                FeatureExtractor featureExtractor;
                                featureExtractor.compute(images[i], records[i], featDir, "hash_" + records[i].fileHash);
                            }
                            catch (const std::exception &error)
                            {
                                errors[i] = error.what();
                                ready[i] = 0;
                            }
                            images[i].release();
                        });
            const int batchCount = static_cast<int>(std::count(ready.begin(), ready.end(), uint8_t{1}));
            for (size_t i = 0; i < count; ++i)
            {
                if (!errors[i].empty())
                {
                    std::cerr << "WARN: feature extraction skipped " << outPaths[first + i] << ": " << errors[i]
                              << std::endl;
                }
                if (!records[i].filePath.empty() && !ready[i])
                {
                    ++skipped;
                }
            }
            if (batchCount > 0)
            {
                // SQL 只在调用线程执行；失败回滚整批，提交后才增加已入库数量。
                if (!db.beginTransaction())
                {
                    return ServiceResult::failure(2, "cannot begin CPU build transaction");
                }
                bool written = true;
                for (size_t i = 0; i < count; ++i)
                {
                    if (ready[i] && !db.insertImage(records[i]))
                    {
                        written = false;
                        break;
                    }
                }
                if (!written || !db.commitTransaction())
                {
                    db.rollbackTransaction();
                    return ServiceResult::failure(2, "cannot commit CPU build batch; transaction rolled back");
                }
                inserted += batchCount;
            }
            std::cout << "\r  features " << first + count << "/" << files.size() << " (ok:" << inserted
                      << " skip:" << skipped << ")   " << std::flush;
        }
        std::cout << std::endl;
    }

    if (request.normalizeOnly)
    {
        std::cout << "Normalize only: " << normDone.load()
                  << " images written to " << outputDir << std::endl;
        return ServiceResult::success("normalize completed");
    }
    if (inserted == 0 && db.totalCount() == 0)
    {
        return ServiceResult::failure(1, "no readable images could be indexed");
    }

    const auto tColorStart = std::chrono::steady_clock::now();
    ColorOrderResult colorResult;
    std::string colorError;
    if (!reorderLibraryByColor(db, outputDir, colorResult, colorError))
    {
        return ServiceResult::failure(1, "cannot reorder library by color: " + colorError);
    }
    const double colorSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tColorStart).count();
    std::cout << "Color order: " << colorResult.computed << " colors computed, "
              << colorResult.renamed << " images renamed (" << std::fixed << std::setprecision(3)
              << colorSeconds << "s)" << std::endl;

    std::cout << std::endl;
    std::cout << "Done: " << inserted << " images indexed, "
              << db.totalCount() << " total in database." << std::endl;

    auto tNow = std::chrono::steady_clock::now();
    using Ms = std::chrono::milliseconds;
    auto normMs = std::chrono::duration_cast<Ms>(tNormEnd - tNormStart).count();
    // 重排和特征提取分别计时，不能把大量文件重命名算作 GPU 特征耗时。
    auto gpuMs = std::chrono::duration_cast<Ms>(tColorStart - tGpuStart).count();
    auto totalMs = std::chrono::duration_cast<Ms>(tNow - tBuildStart).count();
    std::cout << "  Phase timing: normalize " << std::fixed << std::setprecision(1) << normMs / 1000.0
              << "s | " << (gpuOk ? "GPU" : "CPU") << " features " << gpuMs / 1000.0
              << "s | color ordering " << colorSeconds << "s";
    if (totalMs > 0 && inserted > 0) {
        std::cout << " | throughput " << static_cast<int>(inserted * 1000.0 / totalMs) << " img/s";
    }
    std::cout << std::endl;

    tCacheStart = std::chrono::steady_clock::now();
    std::cout << "Building feature cache..." << std::flush;
    FeaturePack::Stats cacheStats;
    if (FeaturePack::buildCache(featDir, db.allRecords(), &cacheStats))
    {
        FeaturePack::removeObsolete(featDir);
        std::cout << (cacheStats.reused ? " reused" : " done") << std::endl;
    }
    else
    {
        std::cout << " failed" << std::endl;
        return ServiceResult::failure(1, "cannot build feature cache after color ordering");
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
