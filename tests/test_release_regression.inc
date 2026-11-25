#include "../core/NormalizeService.h"
#include "../core/ProjectService.h"
#include "../core/SmartCrop.h"
#include <future>
#include <memory>
#include <png.h>
#include <zlib.h>
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

TEST_CASE("concurrent readers reject an old WAL library without changing it")
{
    TempWorkspace workspace;
    const auto db = utf8(workspace.root / "old.db");
    const auto wal = db + "-wal";
    sqlite3 *raw = nullptr;
    const int rc = sqlite3_open(db.c_str(), &raw);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer(raw, sqlite3_close);
    REQUIRE(rc == SQLITE_OK);
    REQUIRE(sqlite3_exec(writer.get(), "PRAGMA journal_mode=WAL; CREATE TABLE images(use_count);"
        "INSERT INTO images VALUES (9)", nullptr, nullptr, nullptr) == SQLITE_OK);
    const auto databaseBytes = readBytesUnicode(db);
    const auto walBytes = readBytesUnicode(wal);
    REQUIRE_FALSE(walBytes.empty());
    std::vector<std::future<std::string>> readers;
    for (int i = 0; i < 4; ++i)
    {
        readers.push_back(std::async(std::launch::async, [&db]
        {
            return DatabaseService().stats({db}).status.runtime.diagnostic.code;
        }));
    }
    for (auto &reader : readers)
    {
        CHECK(reader.get() == "library_incompatible");
    }
    CHECK(readBytesUnicode(db) == databaseBytes);
    CHECK(readBytesUnicode(wal) == walBytes);
    CHECK_FALSE(fs::exists(workspace.root / ".mosaicraft-backups"));
}

TEST_CASE("same-size PNG normalization preserves indexed pixels and ancillary chunks")
{
    TempWorkspace workspace;
    using Bytes = std::vector<uint8_t>;
    auto append32 = [](Bytes &out, uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            out.push_back(static_cast<uint8_t>(value >> shift));
        }
    };
    auto chunk = [&](Bytes &out, const char (&type)[5], const Bytes &data)
    {
        append32(out, static_cast<uint32_t>(data.size()));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        append32(out, crc32(0, out.data() + start, static_cast<uInt>(4 + data.size())));
    };
    Bytes head{137, 80, 78, 71, 13, 10, 26, 10}, tail;
    chunk(head, "IHDR", {0, 0, 0, 32, 0, 0, 0, 32, 8, 3, 0, 0, 0});
    chunk(head, "PLTE", {0, 1, 2, 3, 4, 5});
    chunk(head, "tRNS", {0, 255});
    chunk(tail, "tEXt", {'A', 'u', 't', 'h', 'o', 'r', 0, 'M'});
    chunk(tail, "IEND", {});
    Bytes scanlines(33 * 32, 0);
    for (size_t row = 0; row < 32; ++row)
    {
        for (size_t x = 1; x < 33; x += 2)
        {
            scanlines[row * 33 + x] = 1;
        }
    }
    uLongf length = compressBound(static_cast<uLongf>(scanlines.size()));
    Bytes data(length);
    REQUIRE(compress2(data.data(), &length, scanlines.data(), static_cast<uLong>(scanlines.size()), 0) == Z_OK);
    auto original = head;
    chunk(original, "IDAT", {data.front()});
    chunk(original, "IDAT", Bytes(data.begin() + 1, data.begin() + length));
    original.insert(original.end(), tail.begin(), tail.end());
    const auto source = utf8(workspace.inputDir() / "input.PNG");
    REQUIRE(writeBytesUnicode(source, original));
    auto request = makeBuildRequest(workspace);
    request.outputDir = utf8(workspace.outputDir());
    request.normalizeWidth = request.normalizeHeight = 32;
    RuntimeContext runtime(false);
    REQUIRE(normalizeImages(request, runtime).ok);
    const auto packed = readBytesUnicode(utf8(fs::directory_iterator(workspace.outputDir())->path()));
    REQUIRE(packed.size() >= head.size() + tail.size() + 12);
    CHECK(packed.size() < original.size());
    CHECK(std::equal(head.begin(), head.end(), packed.begin()));
    CHECK(std::equal(tail.rbegin(), tail.rend(), packed.rbegin()));
    const auto *idat = packed.data() + head.size();
    const auto compressed = packed.size() - head.size() - tail.size() - 12;
    REQUIRE(png_get_uint_32(idat) == compressed);
    Bytes restored(scanlines.size());
    length = static_cast<uLongf>(restored.size());
    REQUIRE(uncompress(restored.data(), &length, idat + 8, static_cast<uLong>(compressed)) == Z_OK);
    CHECK(length == scanlines.size());
    CHECK(restored == scanlines);
    CHECK(readBytesUnicode(source) == original);

    ImageNormalizer normalizer(32, 32);
    ImageNormalizer::StandaloneTimings timings;
    const auto pipelineOutput = utf8(workspace.outputDir() / "pipeline.PNG");
    bool saved = false;
    detail::runSmartCropPipeline(normalizer, 1, [&](size_t) { return detail::NormalizePaths{source, pipelineOutput}; },
                                 [&](bool ok) { saved = ok; }, timings);
    REQUIRE(saved);
    CHECK(readBytesUnicode(pipelineOutput) == packed);
}

TEST_CASE("smart crop pipeline preserves order, skips bad input and drains only earlier outputs on inference failure")
{
    TempWorkspace workspace;
    const auto output = workspace.root / "pipelined";
    fs::create_directory(output);
    requireWriteImage(workspace.inputDir() / "0.png", cv::Mat(24, 36, CV_8UC3, cv::Scalar(1, 12, 23)));
    REQUIRE(writeBytesUnicode(utf8(workspace.inputDir() / "1.png"), {0, 1, 2}));
    requireWriteImage(workspace.inputDir() / "2.png", cv::Mat(96, 53, CV_8UC3, cv::Scalar(2, 10, 20)));
    requireWriteImage(workspace.inputDir() / "3.png", cv::Mat(36, 27, CV_8UC3, cv::Scalar(3, 10, 20)));
    const auto paths = [&](size_t index)
    {
        const auto name = std::to_string(index) + ".png";
        return detail::NormalizePaths{utf8(workspace.inputDir() / name), utf8(output / name)};
    };
    bool injectFailure = false;
    ImageNormalizer normalizer(16, 16, [&](const cv::Mat &image)
    {
        if (injectFailure && image.at<cv::Vec3b>(0, 0)[0] == 2)
        {
            throw SmartCropError("smart_crop_cuda_inference", "injected strict CUDA fault", SmartCropCudaFault::Retryable);
        }
        return SmartCropAnalysis{};
    });
    ImageNormalizer::StandaloneTimings timings;
    std::vector<int> completed;
    const auto complete = [&](bool ok) { completed.push_back(ok); };
    // Force the larger image to use the over-budget path without a large fixture.
    detail::runSmartCropPipeline(normalizer, 4, paths, complete, timings, 8192);
    CHECK(completed == std::vector<int>{1, 0, 1, 1});
    for (size_t i : {0, 2, 3})
    {
        const auto [input, pipelined] = paths(i);
        const auto serial = utf8(workspace.outputDir() / (std::to_string(i) + ".png"));
        REQUIRE(normalizer.processStandaloneImage(input, serial));
        CHECK(readBytesUnicode(pipelined) == readBytesUnicode(serial));
    }
    if (smartCropAvailable())
    {
        for (const auto &entry : fs::directory_iterator(output))
        {
            fs::remove(entry.path());
        }
        injectFailure = true;
        completed.clear();
        CHECK_THROWS_AS(detail::runSmartCropPipeline(normalizer, 4, paths, complete, timings), SmartCropError);
        CHECK(completed == std::vector<int>{1, 0});
        CHECK(fs::exists(output / "0.png"));
        CHECK_FALSE(fs::exists(output / "2.png"));
        CHECK_FALSE(fs::exists(output / "3.png"));
    }
}

TEST_CASE("dual crop publishes only the ordered prefix when an earlier inference fails")
{
    if (!smartCropAvailable())
    {
        return;
    }
    TempWorkspace workspace;
    const auto output = workspace.outputDir();
    for (int i = 0; i < 8; ++i)
    {
        const cv::Size size = i == 0 ? cv::Size(16, 16) : cv::Size(24, 20);
        requireWriteImage(workspace.inputDir() / (std::to_string(i) + ".png"), cv::Mat(size, CV_8UC3, cv::Scalar(i, 1, 2)));
    }
    const auto paths = [&](std::size_t index)
    {
        const auto name = std::to_string(index) + ".png";
        return detail::NormalizePaths{utf8(workspace.inputDir() / name), utf8(output / name)};
    };
    const auto [firstSource, firstOutput] = paths(0);
    const auto expected = utf8(workspace.root / "serial-first.png");
    // Even a same-size PNG can be compacted by the original standalone path.
    REQUIRE(ImageNormalizer(16, 16).processStandaloneImage(firstSource, expected));
    std::promise<void> successor;
    auto ready = successor.get_future();
    bool completedOutOfOrder = false;
    const auto infer = [&](const cv::Mat &image)
    {
        const auto index = image.at<cv::Vec3b>(0, 0)[0];
        if (index == 1)
        {
            completedOutOfOrder = ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
            throw SmartCropError("smart_crop_vulkan_inference", "injected failure before completed successor");
        }
        if (index == 2)
        {
            successor.set_value();
        }
        return SmartCropAnalysis{};
    };
    ImageNormalizer cuda(16, 16, infer), vulkan(16, 16, infer);
    ImageNormalizer::StandaloneTimings timings;
    std::vector<int> completed;
    CHECK_THROWS_AS(detail::runDualSmartCropPipeline(cuda, vulkan, 8, paths, [&](bool ok)
    {
        completed.push_back(ok);
    }, timings), SmartCropError);
    CHECK(completedOutOfOrder);
    CHECK(completed == std::vector<int>{1});
    CHECK(readBytesUnicode(firstOutput) == readBytesUnicode(expected));
    CHECK_FALSE(fs::exists(output / "1.png"));
    CHECK_FALSE(fs::exists(output / "2.png"));
}

TEST_CASE("library add drops only oversized PNG iTXt from the saved copy")
{
    TempWorkspace workspace;
    auto request = makeBuildRequest(workspace);
    request.backend = BackendPolicy::Cpu;
    const cv::Size size(request.normalizeWidth, request.normalizeHeight);
    requireWriteImage(workspace.inputDir() / "seed.png", cv::Mat::zeros(size, CV_8UC3));
    REQUIRE(BuildService().run(request).ok);

    const auto insertText = [](std::vector<uint8_t>& png, size_t textSize)
    {
        constexpr size_t dataSize = 6;
        const auto length = static_cast<png_uint_32>(dataSize + textSize);
        // 合法的未压缩 iTXt：一个字符的关键字，以及空语言和翻译字段。
        std::vector<uint8_t> chunk(length + 12, 'x');
        png_save_uint_32(chunk.data(), length);
        std::copy_n("iTXtK", 5, chunk.data() + 4);
        std::fill_n(chunk.data() + 9, 5, 0);
        png_save_uint_32(chunk.data() + 8 + length, crc32(0, chunk.data() + 4, length + 4));
        png.insert(png.begin() + 33, chunk.begin(), chunk.end());
    };
    std::vector<uint8_t> expected;
    REQUIRE(cv::imencode(".png", cv::Mat(size, CV_8UC3, cv::Scalar(20, 90, 170)), expected));
    insertText(expected, 12);
    auto original = expected;
    insertText(original, 8'000'000);
    const auto input = utf8(workspace.emptyInputDir() / "oversized.PNG");
    REQUIRE(writeBytesUnicode(input, original));
    request.appendMode = true;
    request.inputDir = utf8(workspace.emptyInputDir());
    const auto result = BuildService().run(request);
    REQUIRE_MESSAGE(result.ok, result.message);

    Database database(request.dbPath);
    const auto records = database.metadataRecords(false);
    REQUIRE(records.size() == 2);
    const auto repaired = std::find_if(records.begin(), records.end(), [](const auto& record)
    {
        return u8path(record.filePath).extension() == ".PNG";
    });
    REQUIRE(repaired != records.end());
    CHECK(readBytesUnicode(repaired->filePath) == expected);
    CHECK(readBytesUnicode(input) == original);
}

TEST_CASE("portable HTML stays renderable after relocation and rejects corrupt manifests")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());
    auto build = makeBuildRequest(workspace);
    REQUIRE(BuildService().run(build).ok);
    auto mosaic = makeMosaicRequest(workspace);
    const auto original = workspace.outputDir() / "artwork";
    mosaic.outputPath = utf8(original);
    mosaic.config.outputMode = OutputMode::Html;
    mosaic.config.formatExplicit = false;
    RuntimeContext create(false);
    const auto created = MosaicService().run(mosaic, create);
    INFO(created.runtime.diagnostic.message);
    REQUIRE(created.ok);
    const auto relocated = workspace.root / u8path(u8"移动 后的作品");
    fs::rename(original, relocated);
    fs::rename(workspace.libraryDir(), workspace.root / "offline");
    ProjectService project;
    ProjectRequest request;
    request.projectPath = utf8(relocated);
    request.outputPath = utf8(workspace.root / "portable.zip");
    request.deep = true;
    auto run = [&](auto method)
    {
        RuntimeContext runtime(false);
        return (project.*method)(request, runtime).ok;
    };
    REQUIRE(run(&ProjectService::check));
    REQUIRE(run(&ProjectService::pack));
    const auto archive = readBytesUnicode(request.outputPath);
    CHECK_FALSE(run(&ProjectService::pack));
    CHECK(readBytesUnicode(request.outputPath) == archive);
    mz_zip_archive zip{};
    REQUIRE(mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0));
    const std::unique_ptr<mz_zip_archive, decltype(&mz_zip_reader_end)> reader(&zip, mz_zip_reader_end);
    const auto extracted = workspace.root / "unpacked";
    const std::string top = utf8(relocated.filename()) + "/";
    for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); ++index)
    {
        mz_zip_archive_file_stat file{};
        REQUIRE(mz_zip_reader_file_stat(&zip, index, &file));
        const std::string name = file.m_filename;
        REQUIRE(name.rfind(top, 0) == 0);
        REQUIRE(name.find("..") == std::string::npos);
        REQUIRE_FALSE(file.m_is_directory);
        std::vector<uint8_t> contents(static_cast<size_t>(file.m_uncomp_size));
        REQUIRE(mz_zip_reader_extract_to_mem(&zip, index, contents.data(), contents.size(), 0));
        const auto path = extracted / u8path(name);
        fs::create_directories(path.parent_path());
        REQUIRE(writeBytesUnicode(utf8(path), contents));
    }
    request.outputPath = utf8(workspace.mosaicPath());
    REQUIRE(run(&ProjectService::render));
    auto rendered = readBytesUnicode(request.outputPath);
    REQUIRE_FALSE(rendered.empty());
    request.projectPath = utf8(extracted / relocated.filename());
    request.outputPath = utf8(workspace.root / "unpacked.png");
    REQUIRE(run(&ProjectService::check));
    REQUIRE(run(&ProjectService::render));
    CHECK(readBytesUnicode(request.outputPath) == rendered);
    request.projectPath = utf8(relocated);
    request.outputPath = utf8(workspace.mosaicPath());
    const auto manifestPath = utf8(relocated / "project.json");
    const auto bytes = readBytesUnicode(manifestPath);
    const auto manifest = nlohmann::json::parse(bytes);
    for (int mutation = 0; mutation < 4; ++mutation)
    {
        auto changed = manifest;
        switch (mutation)
        {
        case 0: changed["type"] = "unrelated-project"; break;
        case 1: changed["resources"][0]["path"] = "../outside.png"; break;
        case 2: changed["resources"][0]["sha256"] = std::string(64, '0'); break;
        case 3: changed["grid"]["columns"] = 2.5; break;
        }
        const auto text = changed.dump();
        REQUIRE(writeBytesUnicode(manifestPath, {text.begin(), text.end()}));
        CHECK_FALSE(run(&ProjectService::check));
    }
    REQUIRE(writeBytesUnicode(manifestPath, bytes));
    request.config.outputMode = OutputMode::Html;
    request.overwrite = true;
    CHECK_FALSE(run(&ProjectService::render));
    CHECK(readBytesUnicode(request.outputPath) == rendered);
}
