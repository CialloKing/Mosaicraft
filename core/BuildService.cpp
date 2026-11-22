#include "BuildService.h"
#include "compute/PortableCompute.h"
#include "NormalizeService.h"
#include "ExecutionScope.h"

#include "BenchmarkTimer.h"
#include "Database.h"
#include "FeatureExtractor.h"
#include "FeatureUtils.h"
#include "ImageNormalizer.h"
#include "LibraryColorOrder.h"
#include "MosaicPlanning.h"
#include "RequestValidation.h"
#include "UnicodeIO.h"
#include "WorkerPool.h"
#include "compute/CudaBackend.h"
#include "compute/CompatibleFeatures.h"
#include "compute/TemplateFeatureSizes.h"
#ifdef MOSAICRAFT_CUDA
#include "compute/FeatureExtractorCuda.h"
#endif

#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
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

struct AppendBenchmark
{
    ImageNormalizer::LibraryTimings normalization;
    BenchmarkSample hash, sql, batchDedup, transaction, blobWrite, commit, producerWait, consumerWait;
    size_t sqlHits = 0, batchDuplicates = 0, normalizeSlots = 0, featureSlots = 0;

    void report(RuntimeContext &runtime, int threads) const
    {
        std::cout << "  Library append benchmark (overlapping worker times are not wall time):\n";
        const auto emit = [&](const char *key, double value)
        {
            runtime.metric(key, value);
            std::cout << "    " << key << ": " << value << '\n';
        };
        const auto timing = [&](const char *key, const BenchmarkSample &sample)
        {
            if (sample.calls)
            {
                emit(key, sample.ms);
            }
        };
        emit("threadsRequested", threads);
        if (normalizeSlots)
        {
            emit("normalizeWorkerSlots", normalizeSlots);
        }
        if (featureSlots)
        {
            emit("libraryFeatureWorkerSlots", featureSlots);
        }
        timing("normalizeWorkerMs", normalization.normalize);
        timing("assetEncodeWorkerMs", normalization.encode);
        timing("assetWriteWorkerMs", normalization.write);
        timing("pixelHashMs", hash);
        timing("sqlLookupMs", sql);
        timing("batchDedupMs", batchDedup);
        timing("batchTransactionMs", transaction);
        timing("batchBlobWriteMs", blobWrite);
        timing("batchCommitMs", commit);
        timing("normalizeQueueProducerWaitWorkerMs", producerWait);
        timing("normalizeQueueConsumerWaitMs", consumerWait);
        emit("sqlLookupCount", sql.calls);
        emit("sqlDuplicateCount", sqlHits);
        emit("batchDuplicateCount", batchDuplicates);
        emit("committedBatchCount", commit.calls);
    }
};

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

// 下一批只写已保留的临时素材名。若当前批提交失败，先收束线程再撤销尚未交接的文件。
struct PendingNormalized
{
    WorkerPool *pool;
    const std::vector<std::string> &paths;
    const std::vector<uint8_t> &saved;
    std::vector<cv::Mat> images;
    std::vector<ImageRecord> records;
    size_t first = 0;
    ~PendingNormalized()
    {
        if (!images.empty())
        {
            try
            {
                pool->wait();
            }
            catch (...)
            {
                // 主批已失败；仍须清理其他工作线程成功写出的暂存。
            }
            for (size_t i = 0; i < images.size(); ++i)
            {
                if (saved[first + i])
                {
                    std::error_code ec;
                    fs::remove(u8path(paths[first + i]), ec);
                }
            }
        }
    }
};

static bool stdinIsTty()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

} // namespace

static ServiceResult runBuild(BuildRequest request, RuntimeContext &runtime)
{
    runtime.stage("validating");
    if (request.normalizeOnly)
    {
        std::lock_guard<std::mutex> lock(pipelineMutex());
        ExecutionScope scope;
        return normalizeImages(request, runtime);
    }
    const auto commandStart = std::chrono::steady_clock::now();
    std::string validationError, validationField;
    if (!validateBuildRequest(request, validationError, &validationField))
    {
        return runtimeFailure(runtime, 1, validationError, "invalid_argument", validationField);
    }
    if (request.inputDir.empty())
    {
        return runtimeFailure(runtime, 1, "inputDir is required", "invalid_argument", "inputDir");
    }
    std::lock_guard<std::mutex> pipelineLock(pipelineMutex());
    ExecutionScope scope;

    auto output = u8path(request.outputDir.empty() ? "library" : request.outputDir);
    auto databasePath = request.dbPath.empty() ? output / "mosaicraft.db"
                                               : u8path(resolveDbPathForService(request.dbPath));
    const auto dbPath = pathToUtf8(fs::absolute(databasePath).lexically_normal());
    std::optional<Database> database;
    const auto openError = [&]()
    {
        return runtimeFailure(runtime, EXIT_ERR_DB, "cannot open database: " + dbPath + "; " + database->error(),
                              database->errorCode(), "dbPath", dbPath);
    };
    if (request.appendMode)
    {
        database.emplace(dbPath, Database::OpenMode::Recover);
        if (!database->isOpen())
        {
            return openError();
        }
        // 恢复、读取库尺寸和追加共用同一把独占锁，期间不能让其他任务看到暂存路径。
        std::string recoveryError;
        if (!recoverLibraryIngest(*database, recoveryError) || !recoverLibraryColorOrder(*database, recoveryError))
        {
            return runtimeFailure(runtime, EXIT_ERR_DB, recoveryError, "library_unavailable", "dbPath", dbPath);
        }
        if (!readLibraryDimensions(*database, request.normalizeWidth, request.normalizeHeight))
        {
            return runtimeFailure(runtime, EXIT_ERR_DB, "invalid library feature dimensions", "library_incompatible",
                                  "dbPath", dbPath);
        }
        output = u8path(database->path()).parent_path();
    }
    output = fs::weakly_canonical(fs::absolute(output));
    const auto outputDir = pathToUtf8(output);

    runtime.stage("scanning");
    std::vector<std::string> files;
    fs::path input;
    try
    {
        input = fs::canonical(u8path(request.inputDir));
        const auto collect = [&](const auto &entries)
        {
            for (const auto &entry : entries)
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                auto path = pathToUtf8(entry.path());
                if (!ImageNormalizer::libraryOutputExtension(path).empty())
                {
                    files.push_back(std::move(path));
                }
            }
        };
        if (request.recursive)
        {
            collect(fs::recursive_directory_iterator(input));
        }
        else
        {
            collect(fs::directory_iterator(input));
        }
    }
    catch (const fs::filesystem_error &error)
    {
        return runtimeFailure(runtime, 1, "cannot scan input directory: " + std::string(error.what()),
                              "input_unreadable", "inputDir", request.inputDir);
    }
    std::sort(files.begin(), files.end());
    std::cout << "Found " << files.size() << " image(s) in " << request.inputDir << std::endl;
    if (files.empty() && (!database || database->totalCount() == 0))
    {
        return runtimeFailure(runtime, 1, "no supported images found", "input_unreadable", "inputDir", request.inputDir);
    }

    std::error_code ec;
    const bool inputIsOutput = fs::equivalent(input, output, ec);
    const cv::Size requestedSize(request.normalizeWidth, request.normalizeHeight);
    std::set<fs::path> reservedPaths;
    const auto reserveExistingPaths = [&](Database &db)
    {
        for (const auto &record : db.metadataRecords(false))
        {
            reservedPaths.insert(fs::absolute(u8path(record.filePath)).lexically_normal());
        }
    };
    if (inputIsOutput)
    {
        if (!database && fs::exists(u8path(dbPath)))
        {
            database.emplace(dbPath, Database::OpenMode::Build);
            if (!database->isOpen())
            {
                return openError();
            }
        }
        if (database)
        {
            reserveExistingPaths(*database);
        }
        // 历史素材已入库。只检查新文件，避免重新导入旧格式或在检查失败前改名。
        auto next = files.begin();
        for (auto file = files.begin(); file != files.end(); ++file)
        {
            const auto path = u8path(*file);
            if (reservedPaths.count(path.lexically_normal()))
            {
                continue;
            }
            const auto image = ImageNormalizer::readLibrarySource(*file);
            if (image.empty())
            {
                runtime.emit("warning", "source_skipped", "cannot decode source or signature does not match",
                             "inputDir", *file);
                continue;
            }
            if (ImageNormalizer::libraryOutputExtension(*file) != pathToUtf8(path.extension()) ||
                image.size() != requestedSize)
            {
                return runtimeFailure(runtime, 1, "in-place library image requires conversion or resizing: " + *file +
                                      "; use a separate output directory", "invalid_argument", "inputDir", *file);
            }
            if (next != file)
            {
                *next = std::move(*file);
            }
            ++next;
        }
        files.erase(next, files.end());
    }

    ec.clear();
    fs::create_directories(output, ec);
    if (ec)
    {
        return runtimeFailure(runtime, 1, "cannot create output directory: " + outputDir, "output_write_failed",
                              "outputDir", outputDir);
    }
    if (!database)
    {
        database.emplace(dbPath, Database::OpenMode::Build);
        if (!database->isOpen())
        {
            return openError();
        }
    }
    Database &db = *database;
    BuildVisuals visuals;
    const auto existingCount = db.totalCount();
    if (!request.appendMode && !request.forceMode && existingCount > 0)
    {
        if (!request.allowPrompt || !stdinIsTty())
        {
            return runtimeFailure(runtime, 1, "library already exists; use library add or choose a new directory",
                                  "operation_failed", "dbPath", dbPath);
        }
        std::cout << "Database has " << existingCount
                  << " records. Continue building and renumber the entire library by color? [y/N] " << std::flush;
        const char answer = static_cast<char>(std::getchar());
        if (answer != 'y' && answer != 'Y')
        {
            return ServiceResult::success("aborted");
        }
    }
    if (!request.appendMode && !db.createTables(request.normalizeWidth, request.normalizeHeight))
    {
        return runtimeFailure(runtime, EXIT_ERR_DB, "cannot create database tables", "library_unavailable", "dbPath",
                              dbPath);
    }


    if (existingCount > 0 &&
        (db.getMeta("feature_w") != std::to_string(request.normalizeWidth) ||
         db.getMeta("feature_h") != std::to_string(request.normalizeHeight)))
    {
        return runtimeFailure(runtime, 1, "feature dimensions differ; use a new library directory/database",
                              "library_incompatible", "dbPath", dbPath);
    }

    // 入库前持久化尺寸；首次建库被中断后，已有记录仍可用同样尺寸继续追加。
    if (!db.setMeta("feature_w", std::to_string(request.normalizeWidth)) ||
        !db.setMeta("feature_h", std::to_string(request.normalizeHeight)))
    {
        return runtimeFailure(runtime, EXIT_ERR_DB, "cannot save library dimensions", "operation_failed", "");
    }

    auto tBuildStart = std::chrono::steady_clock::now();
    auto tNormStart = tBuildStart;
    std::chrono::steady_clock::time_point tGpuStart = tBuildStart;
    std::chrono::steady_clock::time_point tNormEnd = tBuildStart;
    std::chrono::steady_clock::duration batchNormalization{};
    ImageNormalizer normalizer(request.normalizeWidth, request.normalizeHeight);
    std::vector<std::string> outPaths(files.size());
    std::vector<uint8_t> okFlags(files.size(), 0);
    // 新图片先用临时名字，避免失败文件和旧编号阻碍最终全库颜色排序。
    if (!inputIsOutput)
    {
        reserveExistingPaths(db);
    }
    std::optional<LibraryIngest> ingest;
    if (!inputIsOutput)
    {
        ingest.emplace(db, output);
    }
    const auto publishBatch = [&](const std::vector<ImageRecord *> &records)
    {
        try
        {
            if (ingest)
            {
                ingest->publish(records);
            }
            return true;
        }
        catch (const std::exception &exception)
        {
            runtime.fail("library_write_failed", exception.what(), "dbPath", dbPath);
            return false;
        }
    };
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
            const auto extension = ImageNormalizer::libraryOutputExtension(files[i]);
            snprintf(name, sizeof(name), ".mosaicraft-new-%06zu%s",
                     nameIndex++, extension.c_str());
            candidate = u8path(outputDir) / name;
        } while (fs::exists(candidate) || reservedPaths.count(fs::absolute(candidate).lexically_normal()));
        reservedPaths.insert(fs::absolute(candidate).lexically_normal());
        outPaths[i] = pathToUtf8(ingest->staging() / candidate.filename());
    }
    std::atomic<int> normDone{0};
    std::atomic<int> normalizationProcessed{0};
    int normThreads = normalizationWorkerCount(request.threads);

    AppendBenchmark benchmark;
    auto *profile = request.benchmark ? &benchmark : nullptr;
    std::mutex singleWorkerBudget;
    const auto normalizeOne = [&](size_t index, cv::Mat *pixels, ImageNormalizer::LibraryTimings *timings = nullptr)
    {
        std::unique_lock<std::mutex> budget(singleWorkerBudget, std::defer_lock);
        if (request.threads == 1)
        {
            budget.lock();
        }
        const auto &input = files[index];
        const auto &output = outPaths[index];
        bool saved = false;
        try
        {
            saved = normalizer.processLibraryImage(input, output, request.encoding, pixels, timings);
            if (!saved)
            {
                runtime.emit("warning", "source_skipped", "normalization failed", "inputDir", input);
            }
        }
        catch (const std::exception &error)
        {
            runtime.emit("warning", "source_skipped", error.what(), "inputDir", input);
        }
        if (saved)
        {
            ++normDone;
            okFlags[index] = true;
        }
        const int processed = ++normalizationProcessed;
        runtime.advance("normalization", processed, files.size(), "images", processed == files.size());
        if (processed % 200 == 0 || processed == static_cast<int>(files.size()))
        {
            std::cout << "\r  normalize " << processed << "/" << files.size() << "                    " << std::flush;
        }
        return saved;
    };

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
    const auto waitForQueueSpace = [&](std::unique_lock<std::mutex> &lock, BenchmarkSample *timing)
    {
        const auto ready = [&]()
        {
            return normQueue.size() < MAX_QUEUE || normPhaseDone.load();
        };
        // 不计已就绪的检查或取锁；这里只观察队列背压。
        BenchmarkTimer timer(timing && !ready() ? timing : nullptr);
        queueFullCV.wait(lock, ready);
    };

    runtime.stage("preparing_images");
    FeatureExtractor extractor;
    compute::Device portableDevice;
    bool portableRequested = false;
    const bool autoVulkan = !request.normalizeOnly && compute::automaticVulkan(request.backend, request.device);
    if (!request.normalizeOnly && (autoVulkan || request.backend == BackendPolicy::Vulkan ||
                                   request.backend == BackendPolicy::Metal || request.device != "auto"))
    {
        try
        {
            portableDevice = compute::select(request.backend, request.device);
            portableRequested = portableDevice.backend == BackendPolicy::Vulkan || portableDevice.backend == BackendPolicy::Metal;
        }
        catch (const std::exception &error)
        {
            return runtimeFailure(runtime, 4, error.what(), "gpu_required", "device");
        }
    }
    const auto preflight = autoVulkan || portableRequested || request.normalizeOnly || request.backend == BackendPolicy::Cpu || request.device == "cpu"
        ? cuda::CudaAvailability{} : cuda::queryCudaAvailability();
    if (request.backend == BackendPolicy::Cuda && !preflight.available)
    {
        return runtimeFailure(runtime, 4, "CUDA was explicitly requested but is unavailable: " + preflight.reason,
                              "cuda_required", "backend");
    }
    if (preflight.available)
    {
        runtime.gpuDevice(preflight.pciBus);
    }
    if (!portableRequested && !request.normalizeOnly && request.backend != BackendPolicy::Cpu && request.device != "cpu" && !preflight.available)
    {
        const auto &reason = autoVulkan ? portableDevice.reason : preflight.reason;
        runtime.emit("warning", autoVulkan ? "vulkan_unavailable" : "cuda_unavailable", reason);
        runtime.backends("cpu", "none", reason);
        std::cout << (autoVulkan ? "Vulkan" : "CUDA") << " unavailable; using CPU before extraction: " << reason << std::endl;
    }
    runtime.computeIdentity(portableRequested ? portableDevice.id
        : preflight.available ? "cuda:" + std::to_string(preflight.device) : "cpu", "compatible",
        portableRequested ? "portable-v1" : preflight.available ? "cuda-v6" : "cpu-v6");
    bool gpuOk = false;
    bool usedCompatibleFeatures = false;
#ifdef MOSAICRAFT_CUDA
    gpuOk = preflight.available && cuda::hasTemplateFeatures(request.normalizeWidth, request.normalizeHeight);
#endif
    if (gpuOk && request.threads > 1)
    {
        // CUDA 管线的消费者也执行哈希和提交，给它保留一个 CPU 工作名额。
        normThreads = request.threads - 1;
    }
    runtime.backends(request.normalizeOnly ? "none"
                     : gpuOk               ? "template_cuda"
                                           : "cpu",
                     "none");
    constexpr int GPU_BATCH = 256;
    if (request.appendMode && !request.normalizeOnly)
    {
        visuals.before = summarizeColors(db.averageColors());
    }
    visuals.scanned = files.size();
    int inserted = 0;
    int skipped = 0;
    const auto pixelHash = [&](const cv::Mat &image)
    {
        BenchmarkTimer timer(profile ? &profile->hash : nullptr);
        return hashMatForBuild(image);
    };
    const auto isDuplicate = [&](const std::string &hash, std::set<std::string> &pendingHashes)
    {
        const bool existing = [&]()
        {
            BenchmarkTimer timer(profile ? &profile->sql : nullptr);
            return db.existsByHash(hash);
        }();
        if (existing)
        {
            if (profile)
            {
                ++profile->sqlHits;
            }
            return true;
        }
        // 保持短路：图库中已有的哈希不进入本批的集合。
        BenchmarkTimer timer(profile ? &profile->batchDedup : nullptr);
        const bool duplicate = !pendingHashes.insert(hash).second;
        if (profile && duplicate)
        {
            ++profile->batchDuplicates;
        }
        return duplicate;
    };
    const auto writeImage = [&](const ImageRecord &record, const TextureFeatures &texture)
    {
        BenchmarkTimer timer(profile ? &profile->blobWrite : nullptr);
        return db.insertImage(record, texture);
    };
    const auto commitBatch = [&]()
    {
        BenchmarkTimer timer(profile ? &profile->commit : nullptr);
        return db.advanceRevisions(true) && db.commitTransaction();
    };

    std::thread gpuThread;
    std::exception_ptr gpuError;
    if (gpuOk && !request.normalizeOnly)
    {
        if (profile)
        {
            profile->featureSlots = 1; // 主机端单消费者，不是 CUDA 执行单元数量。
        }
        std::cout << "Normalizing + Features: template CUDA (batch " << GPU_BATCH << ", pipelined)" << std::endl;
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
                        std::vector<TextureFeatures> textures(imgs.size());
#ifdef MOSAICRAFT_CUDA
                        int done = cuda::extractBatch(imgs, recs, textures, &featureWorkspace);
#else
                        int done = 0;
#endif
                        if (done != static_cast<int>(imgs.size()))
                        {
                            if (request.backend == BackendPolicy::Cuda)
                            {
                                throw std::runtime_error("required CUDA feature extraction did not complete");
                            }
                            runtime.emit("warning", "cuda_features_fallback",
                                         "CUDA extraction incomplete; completing features on CPU");
                            runtime.backends("cpu", "none", "CUDA extraction incomplete");
                        }
                        if (done <= 0)
                        {
                            for (size_t bi = 0; bi < imgs.size(); ++bi)
                            {
                                extractor.compute(imgs[bi], recs[bi], &textures[bi]);
                            }
                        }
                        else if (done != static_cast<int>(imgs.size()))
                        {
                            for (size_t bi = static_cast<size_t>(std::max(0, done)); bi < imgs.size(); ++bi)
                            {
                                extractor.compute(imgs[bi], recs[bi], &textures[bi]);
                            }
                        }
                        {
                            BenchmarkTimer timer(profile ? &profile->transaction : nullptr);
                            if (!db.beginImmediateTransaction())
                            {
                                runtime.fail("library_write_failed", "cannot begin build transaction", "dbPath", dbPath);
                                throw std::runtime_error("cannot begin build transaction");
                            }
                            std::vector<ImageRecord *> batch;
                            for (auto &record : recs)
                            {
                                if (!record.filePath.empty())
                                {
                                    batch.push_back(&record);
                                }
                            }
                            if (!publishBatch(batch))
                            {
                                db.rollbackTransaction();
                                throw std::runtime_error("cannot publish build batch");
                            }
                            for (size_t bi = 0; bi < recs.size(); ++bi)
                            {
                                if (recs[bi].filePath.empty())
                                {
                                    continue;
                                }
                                if (writeImage(recs[bi], textures[bi]))
                                {
                                    inserted++;
                                }
                                else
                                {
                                    db.rollbackTransaction();
                                    runtime.fail("library_write_failed", "cannot insert image into library", "dbPath", dbPath);
                                    throw std::runtime_error("cannot insert image into library");
                                }
                            }
                            if (!commitBatch())
                            {
                                db.rollbackTransaction();
                                runtime.fail("library_write_failed", "cannot commit build transaction", "dbPath", dbPath);
                                throw std::runtime_error("cannot commit build transaction");
                            }
                        }
                        gpuDone += static_cast<int>(imgs.size());
                        int gd = gpuDone.load();
                        runtime.advance("features", gd, -1, "images");
                        if (gd % 2000 == 0 || gd == static_cast<int>(files.size()))
                        {
                            std::cout << "\r  gpu feature " << gd << "/" << files.size() << std::flush;
                        }
                        imgs.clear();
                        recs.clear();
                    };
                    while (true)
                    {
                        NormItem item;
                        {
                            std::unique_lock<std::mutex> lk(queueMtx);
                            {
                                const auto ready = [&]()
                                {
                                    return !normQueue.empty() || normPhaseDone.load();
                                };
                                BenchmarkTimer timer(profile && !ready() ? &profile->consumerWait : nullptr);
                                queueCV.wait(lk, ready);
                            }
                            if (normQueue.empty() && normPhaseDone.load())
                            {
                                std::unique_lock<std::mutex> budget(singleWorkerBudget, std::defer_lock);
                                if (request.threads == 1)
                                {
                                    budget.lock();
                                }
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
                        std::unique_lock<std::mutex> budget(singleWorkerBudget, std::defer_lock);
                        if (request.threads == 1)
                        {
                            budget.lock();
                        }
                        std::string hash = pixelHash(item.img);
                        if (isDuplicate(hash, pendingHashes))
                        {
                            ++visuals.duplicates;
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
                            {
                                std::ostringstream detail;
                                detail << "cannot stat output file: " << item.outPath << " (" << sizeEc.message()
                                       << ")";
                                runtime.emit("warning", "source_skipped", detail.str());
                            }
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

    if (!inputIsOutput && (gpuOk || request.normalizeOnly))
    {
        if (!gpuOk || request.normalizeOnly)
        {
            std::cout << "Normalizing (" << normThreads << " threads)..." << std::endl;
        }
        std::atomic<size_t> nextFileIdx{0};
        std::vector<std::thread> workers;
        std::vector<ImageNormalizer::LibraryTimings> timings(profile ? normThreads : 0);
        std::vector<BenchmarkSample> waits(profile ? normThreads : 0);
        for (int t = 0; t < normThreads; ++t)
        {
            workers.emplace_back([&, t]()
            {
                for (;;)
                {
                    size_t fi = nextFileIdx.fetch_add(1);
                    if (fi >= files.size())
                    {
                        break;
                    }
                    cv::Mat result;
                    const bool needPixels = gpuOk && !request.normalizeOnly;
                    if (normalizeOne(fi, needPixels ? &result : nullptr, profile ? &timings[t] : nullptr) && needPixels)
                    {
                        try
                        {
                            std::unique_lock<std::mutex> lock(queueMtx);
                            waitForQueueSpace(lock, profile ? &waits[t] : nullptr);
                            if (normPhaseDone.load())
                            {
                                break;
                            }
                            normQueue.push({std::move(result), outPaths[fi]});
                            lock.unlock();
                            queueCV.notify_one();
                        }
                        catch (const std::exception &error)
                        {
                            runtime.emit("warning", "source_skipped", error.what(), "inputDir", files[fi]);
                        }
                    }
                }
            });
        }
        for (auto& w : workers)
        {
            w.join();
        }
        if (profile)
        {
            profile->normalizeSlots = workers.size();
            for (size_t i = 0; i < timings.size(); ++i)
            {
                profile->normalization.merge(timings[i]);
                profile->producerWait.merge(waits[i]);
            }
        }
        tNormEnd = std::chrono::steady_clock::now();
        std::cout << std::endl;
    }
    else if (inputIsOutput)
    {
        outPaths = files;
        normDone = static_cast<int>(files.size());
        runtime.advance("normalization", files.size(), files.size(), "images", true);
        for (size_t i = 0; i < files.size(); ++i)
        {
            if (gpuOk && !request.normalizeOnly)
            {
                cv::Mat image;
                {
                    std::unique_lock<std::mutex> budget(singleWorkerBudget, std::defer_lock);
                    if (request.threads == 1)
                    {
                        budget.lock();
                    }
                    image = ImageNormalizer::readLibrarySource(files[i]);
                }
                if (image.empty())
                {
                    {
                        std::ostringstream detail;
                        detail << "cannot decode library image: " << files[i];
                        runtime.emit("warning", "source_skipped", detail.str());
                    }
                    continue;
                }
                if (image.size() != requestedSize)
                {
                    runtime.emit("warning", "source_skipped", "image dimensions changed after in-place validation",
                                 "inputDir", files[i]);
                    continue;
                }
                std::unique_lock<std::mutex> lock(queueMtx);
                waitForQueueSpace(lock, profile ? &profile->producerWait : nullptr);
                if (normPhaseDone.load())
                {
                    break;
                }
                normQueue.push({std::move(image), files[i]});
                lock.unlock();
                queueCV.notify_one();
            }
            okFlags[i] = 1;
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
                return runtimeFailure(runtime, 4, error.what(), "cuda_execution_failed", "");
            }
        }
        std::cout << "  GPU done: " << gpuDone.load() << std::endl;
        runtime.advance("features", gpuDone.load(), gpuDone.load(), "images", true);
    }

    if (!gpuOk && !request.normalizeOnly)
    {
        tGpuStart = std::chrono::steady_clock::now();
        constexpr size_t pixelBudget = 64ULL * 1024 * 1024;
        const size_t pixelsPerImage = static_cast<size_t>(request.normalizeWidth) * request.normalizeHeight * 3;
        // 同目录输入的尺寸可能不同，单张处理保证超预算图片独占批次。
        size_t batchLimit =
            inputIsOutput ? 1 : std::min<size_t>(256, std::max<size_t>(1, pixelBudget / pixelsPerImage));
        const int slots = request.threads ? request.threads : std::min(8, normThreads);
        const bool overlap = MOSAICRAFT_TRIAL_BUILD_AHEAD && !inputIsOutput && !portableRequested &&
                             slots >= 4 && pixelsPerImage <= pixelBudget / 2 && files.size() > batchLimit;
        if (overlap)
        {
            batchLimit = std::min(batchLimit, pixelBudget / 2 / pixelsPerImage);
        }
        const int producers = overlap ? slots / 2 : normThreads;
        WorkerPool workers(overlap ? slots - producers - 1 : request.threads);
        std::unique_ptr<WorkerPool> normalizers;
        if (!inputIsOutput)
        {
            normalizers = std::make_unique<WorkerPool>(static_cast<unsigned>(producers));
        }
        compute::FeatureWorkspace portableFeatures(portableDevice);
        cuda::CompatibleFeatureWorkspace compatible;
        cuda::CompatibleTimings compatibleTimings;
        const bool compatibleRequested = preflight.available &&
            !cuda::hasTemplateFeatures(request.normalizeWidth, request.normalizeHeight);
        const bool portableReady = portableRequested && portableFeatures.prepare(request.normalizeWidth,
            request.normalizeHeight, static_cast<int>(batchLimit), pixelBudget, 256ULL * 1024 * 1024);
        if (portableRequested && !portableReady)
        {
            if (!autoVulkan)
            {
                return runtimeFailure(runtime, 4, "required GPU feature resources unavailable", "gpu_required", "backend");
            }
            // 此时尚未提取特征或提交批次，继续原有有序 CPU 流水线。
            const char *reason = "Vulkan feature resources unavailable; using CPU before extraction";
            runtime.emit("warning", "vulkan_resource_fallback", reason);
            runtime.backends("cpu", "none", reason);
            runtime.computeIdentity("cpu", "compatible", "cpu-v6");
            portableRequested = false;
        }
        // 正式对照只证明新建 CPU 图库获益；追加及其他后端保持原先的哈希位置。
        const bool producerHash = MOSAICRAFT_TRIAL_BUILD_HASH && !request.appendMode &&
                                  !inputIsOutput && !compatibleRequested && !portableRequested;
        const bool compatibleReady = portableReady || (compatibleRequested &&
            compatible.prepare(request.normalizeWidth, request.normalizeHeight, static_cast<int>(batchLimit),
                               pixelBudget, true));
        if (compatibleReady)
        {
            batchLimit = static_cast<size_t>(portableReady ? portableFeatures.capacity() : compatible.capacity());
        }
        std::cout << "Features: " << (portableReady ? backendName(portableDevice.backend) : compatibleReady ? "generic CUDA (CPU compatible)" : "CPU")
                  << " (ordered batches, max " << batchLimit << " images, 64 MiB buffer budget)" << std::endl;
        if (compatibleRequested && !compatibleReady)
        {
            if (request.backend == BackendPolicy::Cuda)
            {
                return runtimeFailure(runtime, 4, "required CUDA feature resources unavailable", "cuda_required");
            }
            runtime.emit("warning", "cuda_resource_fallback",
                         "Generic CUDA resources unavailable; using CPU before extraction.");
            std::cout << "  Generic CUDA resources unavailable; using CPU before extraction." << std::endl;
        }
        runtime.backends(portableReady ? backendName(portableDevice.backend) : compatibleReady ? "compatible_cuda" : "cpu", "none");
        std::vector<ImageNormalizer::LibraryTimings> timings;
        PendingNormalized pending{normalizers.get(), outPaths, okFlags};
        // 归一化最多保留一批独占写槽；wait/run 完成后才汇总或复用。
        const auto mergeTimings = [&]()
        {
            if (profile)
            {
                for (const auto &item : timings)
                {
                    profile->normalization.merge(item);
                }
            }
        };
        if (profile)
        {
            profile->featureSlots = workers.size();
            if (normalizers)
            {
                profile->normalizeSlots = normalizers->size();
            }
        }
        const auto startNext = [&](size_t first)
        {
            pending.first = first;
            const auto count = std::min(batchLimit, files.size() - first);
            pending.images.resize(count);
            pending.records.resize(count);
            if (profile)
            {
                timings.assign(count, {});
            }
            normalizers->start(count, [&, first](size_t i)
            {
                if (normalizeOne(first + i, &pending.images[i], profile ? &timings[i] : nullptr) && producerHash)
                {
                    pending.records[i].fileHash = hashMatForBuild(pending.images[i]);
                }
            });
        };
        if (overlap)
        {
            startNext(0);
        }
        for (size_t first = 0; first < files.size(); first += batchLimit)
        {
            const size_t count = std::min(batchLimit, files.size() - first);
            std::vector<cv::Mat> images(count);
            std::vector<ImageRecord> records(count);
            std::vector<TextureFeatures> textures(count);
            std::vector<uint8_t> ready(count, 0);
            std::vector<std::string> errors(count);
            // 生产者只写自己的槽，查重和提交仍由单写者按扫描顺序执行。
            const auto hashReady = [&](size_t i)
            {
                if (producerHash && !images[i].empty())
                {
                    records[i].fileHash = hashMatForBuild(images[i]);
                }
            };
            if (overlap)
            {
                const auto waiting = std::chrono::steady_clock::now();
                normalizers->wait();
                // 仅统计未被本批其他工作掩盖的等待，避免墙钟重复计入。
                batchNormalization += std::chrono::steady_clock::now() - waiting;
                mergeTimings();
                images.swap(pending.images);
                records.swap(pending.records);
                pending.images.clear();
                pending.records.clear();
                if (first + count < files.size())
                {
                    startNext(first + count);
                }
            }
            else if (normalizers)
            {
                // 只保留当前有界批次的落盘像素，让顺序去重和提交继续使用原路径。
                const auto started = std::chrono::steady_clock::now();
                if (profile)
                {
                    timings.assign(count, {});
                }
                normalizers->run(count, [&](size_t i)
                {
                    normalizeOne(first + i, &images[i], profile ? &timings[i] : nullptr);
                    hashReady(i);
                });
                batchNormalization += std::chrono::steady_clock::now() - started;
                mergeTimings();
            }
            else
            {
                workers.run(count, [&](size_t i)
                {
                    if (!okFlags[first + i])
                    {
                        return;
                    }
                    try
                    {
                        images[i] = ImageNormalizer::readLibrarySource(outPaths[first + i]);
                        if (images[i].empty())
                        {
                            throw std::runtime_error("cannot decode saved image");
                        }
                        if (inputIsOutput && images[i].size() != requestedSize)
                        {
                            throw std::runtime_error("image dimensions changed after in-place validation");
                        }
                        hashReady(i);
                    }
                    catch (const std::exception &error)
                    {
                        errors[i] = error.what();
                        okFlags[first + i] = 0;
                        images[i].release();
                    }
                });
            }
            std::set<std::string> pendingHashes;
            for (size_t i = 0; i < count; ++i)
            {
                if (images[i].empty())
                {
                    ++skipped;
                    continue;
                }
                const auto &outPath = outPaths[first + i];
                auto &record = records[i];
                if (!producerHash)
                {
                    record.fileHash = pixelHash(images[i]);
                }
                const auto &hash = record.fileHash;
                if (isDuplicate(hash, pendingHashes))
                {
                    ++visuals.duplicates;
                    if (!inputIsOutput)
                    {
                        std::error_code removeError;
                        fs::remove(u8path(outPath), removeError);
                        if (removeError)
                        {
                            {
                                std::ostringstream detail;
                                detail << "cannot remove duplicate output file: " << outPath;
                                runtime.emit("warning", "source_skipped", detail.str());
                            }
                        }
                    }
                    images[i].release();
                    ++skipped;
                    continue;
                }
                record.filePath = outPath;
                record.srcWidth = images[i].cols;
                record.srcHeight = images[i].rows;
                record.aspectRatio = static_cast<double>(record.srcWidth) / record.srcHeight;
                std::error_code sizeError;
                record.fileSize = static_cast<int64_t>(fs::file_size(u8path(outPath), sizeError));
                if (sizeError)
                {
                    images[i].release();
                    record.filePath.clear();
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
            // 自定义尺寸保留 CPU 的去重和提交顺序；只把已准备好的像素统计交给 GPU。
            std::vector<cv::Mat> compatibleImages;
            std::vector<ImageRecord *> statistics;
            std::vector<int> featureRows(count, -1);
            std::vector<cuda::CompatibleFeature> features;
            bool useCompatible = compatibleReady;
            for (size_t i = 0; i < count && useCompatible; ++i)
            {
                if (ready[i])
                {
                    // 同目录建库允许混合原始尺寸，沿用原 CPU 行为，不能冒用归一化元数据。
                    useCompatible = images[i].cols == request.normalizeWidth &&
                                    images[i].rows == request.normalizeHeight;
                    featureRows[i] = static_cast<int>(compatibleImages.size());
                    compatibleImages.push_back(images[i]);
                    statistics.push_back(&records[i]);
                }
            }
            if (useCompatible && !compatibleImages.empty())
            {
                try
                {
                    if (portableReady)
                    {
                        portableFeatures.extract(compatibleImages, features, workers, statistics);
                    }
                    else
                    {
                        compatible.extract(compatibleImages, features, workers, statistics, &compatibleTimings);
                    }
                    usedCompatibleFeatures = true;
                }
                catch (const std::exception &error)
                {
                    // CUDA 运行期失败中止本批；此时尚未开始数据库事务。
                    return runtimeFailure(runtime, 4, error.what(), portableReady ? "gpu_execution_failed" : "cuda_execution_failed", "");
                }
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
                                if (useCompatible)
                                {
                                    const auto &feature = features.at(static_cast<size_t>(featureRows[i]));
                                    auto &record = records[i];
                                    record.avgL = feature.lab[0];
                                    record.avgA = feature.lab[1];
                                    record.avgB = feature.lab[2];
                                    record.gridLab.assign(feature.grid.begin(), feature.grid.end());
                                    std::copy_n(feature.tiny.begin(), textures[i].tiny.size(), textures[i].tiny.begin());
                                    std::copy_n(feature.lbp.begin(), textures[i].lbp.size(), textures[i].lbp.begin());
                                    record.featureVersion = FeatureExtractor::featureVersion();
                                }
                                else
                                {
                                    FeatureExtractor featureExtractor;
                                    featureExtractor.compute(images[i], records[i], &textures[i]);
                                }
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
                    {
                        std::ostringstream detail;
                        detail << "feature extraction skipped " << outPaths[first + i] << ": " << errors[i];
                        runtime.emit("warning", "source_skipped", detail.str());
                    }
                }
                if (!records[i].filePath.empty() && !ready[i])
                {
                    ++skipped;
                }
            }
            if (batchCount > 0)
            {
                // SQL 只在调用线程执行；失败回滚整批，提交后才增加已入库数量。
                BenchmarkTimer timer(profile ? &profile->transaction : nullptr);
                if (!db.beginImmediateTransaction())
                {
                    return runtimeFailure(runtime, 2, "cannot begin CPU build transaction", "library_write_failed",
                                          "dbPath", dbPath);
                }
                std::vector<ImageRecord *> batch;
                for (size_t i = 0; i < count; ++i)
                {
                    if (ready[i])
                    {
                        batch.push_back(&records[i]);
                    }
                }
                bool written = publishBatch(batch);
                for (size_t i = 0; written && i < count; ++i)
                {
                    if (ready[i] && !writeImage(records[i], textures[i]))
                    {
                        written = false;
                        break;
                    }
                }
                if (!written || !commitBatch())
                {
                    db.rollbackTransaction();
                    return runtimeFailure(runtime, 2, "cannot commit CPU build batch; transaction rolled back",
                                          "library_write_failed", "dbPath", dbPath);
                }
                inserted += batchCount;
            }
            runtime.advance("features", first + count, files.size(), "images", first + count == files.size());
            std::cout << "\r  features " << first + count << "/" << files.size() << " (ok:" << inserted
                      << " skip:" << skipped << ")   " << std::flush;
        }
        std::cout << std::endl;
        runtime.backends(portableReady ? backendName(portableDevice.backend) : compatibleReady ? "compatible_cuda" : "cpu", "none");
        if (portableReady)
        {
            const auto timings = portableFeatures.timings();
            runtime.metric("gpuAllocationMs", timings.allocationMs);
            runtime.metric("gpuUploadMs", timings.uploadMs);
            runtime.metric("gpuComputeWaitMs", timings.computeMs);
            runtime.metric("gpuDownloadMs", timings.downloadMs);
            runtime.metric("gpuResidentBytes", double(timings.residentBytes));
            runtime.metric("gpuPeakBufferBytes", double(timings.peakResidentBytes));
            runtime.metric("gpuSharedMemoryBytes", portableDevice.integrated ? double(timings.residentBytes) : 0);
            runtime.metric("gpuUploadBytes", double(timings.uploadBytes));
            runtime.metric("gpuDownloadBytes", double(timings.downloadBytes));
        }
        if (compatibleReady && !portableReady)
        {
            std::cout << "  Generic CUDA: " << compatibleTimings.images << " images, "
                      << compatibleTimings.batches << " batches; host/device buffers "
                      << compatible.hostBytes() << "/" << compatible.deviceBytes() << " bytes; preprocess "
                      << compatibleTimings.prepareMs << " ms, CPU completion " << compatibleTimings.cpuMs
                      << " ms, device upload/compute/download " << compatibleTimings.gpu.eventUploadMs << "/"
                      << compatibleTimings.gpu.eventComputeMs << "/" << compatibleTimings.gpu.eventDownloadMs
                      << " ms" << std::endl;
        }
    }

    if (request.normalizeOnly)
    {
        visuals.normalized = normDone.load();
        runtime.buildVisuals(std::move(visuals));
        std::cout << "Normalize only: " << normDone.load()
                  << " images written to " << outputDir << std::endl;
        runtime.stage("finalizing");
        runtime.artifact("normalized", "directory", outputDir);
        return ServiceResult::success("normalize completed");
    }
    std::string ingestError;
    if (ingest && !ingest->finish(ingestError))
    {
        return runtimeFailure(runtime, 2, ingestError, "library_write_failed", "dbPath", dbPath);
    }
    if (inserted == 0 && db.totalCount() == 0)
    {
        return runtimeFailure(runtime, 1, "no readable images could be indexed", "input_unreadable", "inputDir",
                              request.inputDir);
    }

    runtime.stage("reordering_library");
    const auto tColorStart = std::chrono::steady_clock::now();
    ColorOrderResult colorResult;
    std::string colorError;
    if (!reorderLibraryByColor(db, outputDir, colorResult, colorError))
    {
        return runtimeFailure(runtime, 1, "cannot reorder library by color: " + colorError, "library_unavailable",
                              "dbPath", dbPath);
    }
    const double colorSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tColorStart).count();
    std::cout << "Color order: " << colorResult.computed << " colors computed, "
              << colorResult.renamed << " images renamed (" << std::fixed << std::setprecision(3)
              << colorSeconds << "s)" << std::endl;
    std::cout << "  Color validation (paths+keys): " << colorResult.validationMs << " ms\n"
              << "  Color sorting: " << colorResult.sortingMs << " ms\n"
              << "  Color filesystem/SQL writing: " << colorResult.writingMs << " ms\n";

    std::cout << std::endl;
    std::cout << "Done: " << inserted << " images indexed, "
              << db.totalCount() << " total in database." << std::endl;

    using Ms = std::chrono::milliseconds;
    auto normMs = std::chrono::duration_cast<Ms>(tNormEnd - tNormStart + batchNormalization).count();
    // 重排和特征提取分别计时，不能把大量文件重命名算作 GPU 特征耗时。
    auto gpuMs = gpuOk && gpuDone.load() == 0 ? 0 :
        std::chrono::duration_cast<Ms>(tColorStart - tGpuStart - batchNormalization).count();
    if (!gpuOk && !inputIsOutput)
    {
        runtime.metric("normalizedPixelsReused", normDone.load());
    }

    std::cout << "  Phase timing: normalize " << std::fixed << std::setprecision(1) << normMs / 1000.0
              << "s | " << (gpuOk ? "template CUDA" : usedCompatibleFeatures ? "generic CUDA" : "CPU")
              << " features " << gpuMs / 1000.0
              << "s | color ordering " << colorSeconds << "s";
    std::cout << std::endl;

    std::cout << "Feature resolution: " << request.normalizeWidth << "x" << request.normalizeHeight << std::endl;
    const auto wallMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - commandStart).count();
    std::cout << "  Build total (wall, including scan and color order): " << wallMs << " ms\n";
    if (gpuOk)
    {
        std::cout << "  GPU normalization/features overlap; child timings are not additive.\n";
    }

    runtime.stage("finalizing");
    visuals.normalized = normDone.load();
    visuals.inserted = inserted;
    runtime.metric("totalMs", wallMs);
    runtime.metric("normalizeMs", normMs);
    runtime.metric("libraryFeaturesMs", gpuMs);
    runtime.metric("reorderMs", colorSeconds * 1000);
    try
    {
        visuals.after = summarizeColors(db.averageColors());
        visuals.hasColors = true;
    }
    catch (const std::exception &error)
    {
        runtime.emit("warning", "analysis_write_failed", error.what());
    }
    runtime.buildVisuals(std::move(visuals));
    runtime.artifact("library", "database", resolveDbPathForService(dbPath), "application/vnd.sqlite3");
    runtime.artifact("library-directory", "directory", outputDir);
    if (profile)
    {
        profile->report(runtime, request.threads);
    }
    return ServiceResult::success("build completed");
}

ServiceResult BuildService::run(const BuildRequest &request) const
{
    RuntimeContext runtime;
    return run(request, runtime);
}

ServiceResult BuildService::run(const BuildRequest &request, RuntimeContext &runtime) const
{
    auto result = runBuild(request, runtime);
    // SQLite 最终关闭后再刷新图库文件指纹。
    for (const auto &item : runtime.snapshot().artifacts)
    {
        if (item.kind == "database")
        {
            runtime.artifact(item.id, item.kind, item.path, item.mime);
        }
    }
    return runtimeResult(std::move(result), runtime);
}
} // namespace mosaicraft
