#include "LibraryColorOrder.h"
#include "ColorOrder.h"
#include "Database.h"
#include "LibraryAccess.h"
#include "LibraryPaths.h"
#include "UnicodeIO.h"
#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mosaicraft
{
namespace
{

namespace fs = std::filesystem;
using Json = nlohmann::json;

fs::path absolutePath(const std::string& path)
{
    return fs::weakly_canonical(u8path(path));
}

fs::path normalizedIdentity(const fs::path &normalized)
{
#ifdef _WIN32
    auto native = normalized.wstring();
    std::transform(native.begin(), native.end(), native.begin(), std::towlower);
    return fs::path(native);
#else
    return normalized;
#endif
}

fs::path identity(const fs::path &path)
{
    return normalizedIdentity(fs::weakly_canonical(path));
}

struct FileInformation
{
    uint64_t volume, size, fileId;
    bool regular;
};

FileInformation fileInformation(const fs::path &path)
{
#ifdef _WIN32
    const auto handle =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION information{};
    const bool ok = handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &information);
    if (handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle);
    }
    if (!ok)
    {
        throw std::runtime_error("cannot inspect library path: " + pathToUtf8(path));
    }
    return {information.dwVolumeSerialNumber,
            (static_cast<uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow,
            (static_cast<uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow,
            !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)};
#else
    struct stat information{};
    if (stat(path.c_str(), &information) != 0)
    {
        throw std::runtime_error("cannot inspect library path: " + pathToUtf8(path));
    }
    return {static_cast<uint64_t>(information.st_dev), static_cast<uint64_t>(information.st_size),
            static_cast<uint64_t>(information.st_ino), S_ISREG(information.st_mode)};
#endif
}

void moveFile(const fs::path& from, const fs::path& to)
{
    // Windows 与 POSIX 都禁止覆盖；清单之外的文件只能由用户自行处理。
    if (fs::is_regular_file(from) && fs::exists(to) && fs::equivalent(from, to))
    {
        // POSIX link/unlink 在两步之间中断时，只删除清单内的重复链接。
        fs::remove(from);
        return;
    }
    if (!fs::is_regular_file(from) || fs::exists(to))
    {
        throw std::runtime_error("color reorder path missing or occupied: " + pathToUtf8(from) +
                                 " -> " + pathToUtf8(to));
    }
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        throw std::runtime_error("cannot rename library image: " + pathToUtf8(from) +
                                 " (Windows error " + std::to_string(GetLastError()) + ")");
    }
#else
    // 同卷硬链接再删除旧名字提供不覆盖语义；崩溃时可能留下同一文件的两个链接。
    try
    {
        fs::create_hard_link(from, to);
    }
    catch (const fs::filesystem_error &exception)
    {
        if (exception.code() == std::errc::cross_device_link)
        {
            throw std::runtime_error("color reorder cannot move images across filesystems: " +
                                     pathToUtf8(from) + " -> " + pathToUtf8(to));
        }
        throw;
    }
    fs::remove(from);
#endif
}

Json journalPaths(Json journal, const std::string &database, bool storing)
{
    const LibraryPaths paths(database);
    auto transform = [&](Json &value, bool directory = false)
    {
        const auto text = value.get<std::string>();
        if (storing && directory && identity(u8path(text)) == identity(u8path(database).parent_path()))
        {
            value = ".";
        }
        else if (!storing && directory && text == ".")
        {
            value = pathToUtf8(u8path(database).parent_path());
        }
        else
        {
            value = storing ? paths.store(text) : paths.resolve(text);
        }
    };
    transform(journal.at("database"));
    transform(journal.at("output"), true);
    transform(journal.at("staging"));
    for (auto &entry : journal.at("entries"))
    {
        for (const auto *key : {"old_db_path", "original", "final", "temporary"})
        {
            transform(entry.at(key));
        }
    }
    return journal;
}

void persistJournal(const fs::path &path, const Json &contents)
{
    const auto temporary = u8path(pathToUtf8(path) + ".tmp");
    const auto temporaryStatus = libraryJournalStatus(temporary);
    if (fs::exists(temporaryStatus) && !fs::is_regular_file(temporaryStatus))
    {
        throw std::runtime_error("invalid temporary journal path");
    }
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << contents.dump(2);
    output.close();
    if (!output)
    {
        throw std::runtime_error("cannot write library recovery journal");
    }
#ifdef _WIN32
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    const bool flushed = handle != INVALID_HANDLE_VALUE && FlushFileBuffers(handle);
    if (handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle);
    }
    if (!flushed || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        throw std::runtime_error("cannot persist library recovery journal");
    }
#else
    const int descriptor = open(temporary.c_str(), O_WRONLY);
    const bool flushed = descriptor >= 0 && fsync(descriptor) == 0;
    if (descriptor >= 0)
    {
        close(descriptor);
    }
    if (!flushed)
    {
        throw std::runtime_error("cannot persist library recovery journal");
    }
    fs::rename(temporary, path);
    const int directory = open(path.parent_path().c_str(), O_RDONLY);
    const bool directoryFlushed = directory >= 0 && fsync(directory) == 0;
    if (directory >= 0)
    {
        close(directory);
    }
    if (!directoryFlushed)
    {
        throw std::runtime_error("cannot persist library recovery journal directory");
    }
#endif
}

void writeJournal(const fs::path &path, const Json &journal)
{
    persistJournal(path, journalPaths(journal, journal.at("database").get<std::string>(), true));
}

constexpr const char *ingestFilePrefix = ".mosaicraft-new-";

fs::path ingestStaging(Database &db)
{
    const auto uuid = db.getMeta("library_uuid");
    if (uuid.size() != 32 || uuid.find_first_not_of("0123456789abcdef") != std::string::npos)
    {
        throw std::runtime_error("invalid library identity");
    }
    return u8path(db.path()).parent_path() / u8path(".mosaicraft-ingest-" + uuid);
}

bool isIngestFilename(const fs::path &path)
{
    return path == path.filename() && pathToUtf8(path).rfind(ingestFilePrefix, 0) == 0;
}

Json ingestManifest(Database &db, const std::string &batch)
{
    return {{"uuid", db.getMeta("library_uuid")}, {"batch", batch}, {"entries", Json::array()}};
}

void cleanupJournal(const fs::path& path, const Json& journal)
{
    const auto staging = u8path(journal.at("staging").get<std::string>());
    // 只移除空目录；不能递归删除重命名清单未包含的文件。
    if (fs::exists(staging))
    {
        fs::remove(staging);
    }
    fs::remove(u8path(pathToUtf8(path) + ".tmp"));
    fs::remove(path);
}

void validateJournal(Database& database, const Json& journal, bool committed)
{
    if (journal.at("version") != kJournalVersion || journal.at("library_version") != kLibraryVersion ||
        journal.at("feature_version") != kFeatureVersion || journal.at("uuid") != database.getMeta("library_uuid") ||
        identity(u8path(journal.at("database").get<std::string>())) != identity(u8path(database.path())))
    {
        throw std::runtime_error("color reorder journal belongs to another database or version");
    }
    const auto token = journal.at("token").get<std::string>();
    if (token.empty() || token.find_first_not_of("0123456789-") != std::string::npos)
    {
        throw std::runtime_error("invalid color reorder batch token");
    }
    const auto root = absolutePath(journal.at("output").get<std::string>());
    const auto staging = absolutePath(journal.at("staging").get<std::string>());
    if (staging != root / (".mosaicraft-color-order-" + token))
    {
        throw std::runtime_error("invalid color reorder staging directory");
    }
    std::unordered_map<int, ImageRecord> records;
    for (auto& record : database.metadataRecords())
    {
        records.emplace(record.id, std::move(record));
    }
    std::set<int> ids;
    std::set<fs::path> originals, destinations;
    for (const auto& entry : journal.at("entries"))
    {
        const int id = entry.at("id");
        const auto found = records.find(id);
        const auto original = absolutePath(entry.at("original").get<std::string>());
        const auto expectedOriginal = fs::weakly_canonical(u8path(entry.at("old_db_path").get<std::string>()));
        const auto final = absolutePath(entry.at("final").get<std::string>());
        const auto temporary = absolutePath(entry.at("temporary").get<std::string>());
        if (found == records.end() || !ids.insert(id).second ||
            !originals.insert(identity(original)).second || !destinations.insert(identity(final)).second ||
            final.parent_path() != root || temporary != staging / (std::to_string(id) + ".stage") ||
            original != expectedOriginal ||
            found->second.filePath != entry.at(committed ? "final" : "old_db_path").get<std::string>() ||
            found->second.fileHash != entry.at("hash").get<std::string>())
        {
            throw std::runtime_error("color reorder journal does not match indexed image paths");
        }
    }
}

void requireSize(const fs::path& path, const Json& entry)
{
    if (!fs::is_regular_file(path) || fs::file_size(path) != entry.at("size").get<std::uintmax_t>())
    {
        throw std::runtime_error("color reorder image missing or changed: " + pathToUtf8(path));
    }
}

std::uint64_t ingestWriteTime(const fs::path &path)
{
    // 文件时钟的宽度和单位依平台而异，只在本机比较原始计数的低 64 位。
    return static_cast<std::uint64_t>(fs::last_write_time(path).time_since_epoch().count());
}

} // namespace

bool recoverLibraryIngest(Database &db, std::string &error)
{
    try
    {
        const auto path = libraryIngestJournalPath(db.path());
        const auto status = libraryJournalStatus(path);
        if (!fs::exists(status))
        {
            return true;
        }
        if (!fs::is_regular_file(status))
        {
            throw std::runtime_error("ingest journal is not a regular file");
        }
        std::ifstream file(path, std::ios::binary);
        const auto manifest = Json::parse(file);
        file.close();
        const auto staging = ingestStaging(db);
        const auto stagingStatus = libraryJournalStatus(staging);
        if (manifest.at("uuid") != db.getMeta("library_uuid") ||
            (fs::exists(stagingStatus) && !fs::is_directory(stagingStatus)))
        {
            throw std::runtime_error("ingest journal or staging directory belongs to another library");
        }
        const bool committed = manifest.at("batch") == db.getMeta("ingest_batch");
        const LibraryPaths paths(db.path());
        std::vector<fs::path> uncommitted;
        for (const auto &entry : manifest.at("entries"))
        {
            const auto relative = entry.at("path").get<std::string>();
            const auto name = u8path(relative).filename();
            if (!LibraryPaths::relative(relative) || !isIngestFilename(name))
            {
                throw std::runtime_error("invalid ingest destination");
            }
            const auto final = u8path(paths.resolve(relative));
            if (paths.store(pathToUtf8(fs::weakly_canonical(final.parent_path()) / name)) != relative)
            {
                throw std::runtime_error("ingest destination escapes the library");
            }
            const auto pending = staging / name;
            const auto pendingStatus = libraryJournalStatus(pending);
            const auto finalStatus = libraryJournalStatus(final);
            if (!fs::exists(finalStatus))
            {
                if (committed)
                {
                    throw std::runtime_error("committed library image is missing");
                }
                continue;
            }
            if (!fs::is_regular_file(finalStatus) || (fs::exists(pendingStatus) &&
                (!fs::is_regular_file(pendingStatus) || !fs::equivalent(pending, final))))
            {
                throw std::runtime_error("ingest destination is occupied by another file");
            }
            const auto info = fileInformation(final);
            if (info.volume != entry.at("volume").get<uint64_t>() || info.fileId != entry.at("file_id").get<uint64_t>() ||
                info.size != entry.at("size").get<uint64_t>() ||
                ingestWriteTime(final) != entry.at("time").get<uint64_t>())
            {
                throw std::runtime_error("ingest destination changed");
            }
            if (!committed)
            {
                uncommitted.push_back(final);
            }
        }
        std::vector<fs::path> pendingFiles;
        if (fs::is_directory(stagingStatus))
        {
            for (const auto &item : fs::directory_iterator(staging))
            {
                if (!isIngestFilename(item.path().filename()) || !fs::is_regular_file(item.symlink_status()))
                {
                    throw std::runtime_error("unexpected file in ingest staging directory");
                }
                pendingFiles.push_back(item.path());
            }
        }
        const auto temporary = u8path(pathToUtf8(path) + ".tmp");
        const auto temporaryStatus = libraryJournalStatus(temporary);
        if (fs::exists(temporaryStatus) && !fs::is_regular_file(temporaryStatus))
        {
            throw std::runtime_error("invalid temporary ingest journal");
        }
        // 先核实整份清单，发生外部冲突时不留下部分清理的现场。
        for (const auto &final : uncommitted)
        {
            fs::remove(final);
        }
        for (const auto &pending : pendingFiles)
        {
            fs::remove(pending);
        }
        fs::remove(staging);
        fs::remove(temporary);
        fs::remove(path);
        return true;
    }
    catch (const std::exception &exception)
    {
        error = std::string("library ingestion recovery failed; retain journal and staging: ") + exception.what();
        return false;
    }
}

LibraryIngest::LibraryIngest(Database &db, const fs::path &output)
    : m_db(db), m_output(output), m_staging(ingestStaging(db))
{
    if (fs::exists(libraryJournalStatus(m_staging)))
    {
        throw std::runtime_error("library ingest staging directory already exists");
    }
    persistJournal(libraryIngestJournalPath(db.path()), ingestManifest(db, ""));
    fs::create_directory(m_staging);
}

LibraryIngest::~LibraryIngest()
{
    std::string ignored;
    finish(ignored);
}

void LibraryIngest::publish(const std::vector<ImageRecord *> &records)
{
    // 当前修订号与上个已提交标记不同；回滚后仍可辨别这个批次的文件。
    const auto batch = m_db.getMeta("feature_revision");
    auto manifest = ingestManifest(m_db, batch);
    const LibraryPaths paths(m_db.path());
    std::vector<fs::path> finals;
    finals.reserve(records.size());
    for (const auto *record : records)
    {
        const auto pending = u8path(record->filePath);
        const auto final = m_output / pending.filename();
        if (pending.parent_path() != m_staging || !isIngestFilename(pending.filename()) ||
            !fs::is_regular_file(libraryJournalStatus(pending)) || fs::exists(libraryJournalStatus(final)))
        {
            throw std::runtime_error("invalid staged image or occupied destination");
        }
        const auto info = fileInformation(pending);
        manifest["entries"].push_back({{"path", paths.store(pathToUtf8(final))},
            {"size", info.size}, {"volume", info.volume}, {"file_id", info.fileId},
            {"time", ingestWriteTime(pending)}});
        finals.push_back(final);
    }
    persistJournal(libraryIngestJournalPath(m_db.path()), manifest);
    for (size_t i = 0; i < records.size(); ++i)
    {
        moveFile(u8path(records[i]->filePath), finals[i]);
        records[i]->filePath = pathToUtf8(finals[i]);
    }
    if (!m_db.setMeta("ingest_batch", batch))
    {
        throw std::runtime_error("cannot write ingest commit marker");
    }
}

bool LibraryIngest::finish(std::string &error)
{
    if (m_active && m_db.inTransaction())
    {
        // 事务尚未确定结果时必须保留清单，由下次打开读取已持久化的提交标记。
        error = "cannot recover ingestion while a database transaction is active";
        return false;
    }
    if (m_active && !recoverLibraryIngest(m_db, error))
    {
        return false;
    }
    m_active = false;
    return true;
}

bool recoverLibraryColorOrder(Database& database, std::string& error)
{
    try
    {
        const auto path = libraryJournalPath(database.path());
        const auto status = libraryJournalStatus(path);
        if (!fs::exists(status))
        {
            return true;
        }
        if (!fs::is_regular_file(status))
        {
            throw std::runtime_error("color reorder journal is not a regular file");
        }
        std::ifstream input(path, std::ios::binary);
        Json journal;
        input >> journal;
        input.close();
        journal = journalPaths(std::move(journal), database.path(), false);
        const bool committed = database.getMeta("color_order_batch") == journal.at("token").get<std::string>();
        validateJournal(database, journal, committed);
        if (committed)
        {
            for (const auto& entry : journal.at("entries"))
            {
                requireSize(u8path(entry.at("final").get<std::string>()), entry);
                const auto temporary = u8path(entry.at("temporary").get<std::string>());
                if (fs::exists(temporary))
                {
                    if (!fs::equivalent(temporary, u8path(entry.at("final").get<std::string>())))
                    {
                        throw std::runtime_error("unexpected file in committed color reorder staging");
                    }
                    fs::remove(temporary);
                }
            }
        }
        else
        {
            const auto phase = journal.at("phase").get<std::string>();
            if (phase != "staging" && phase != "publishing" && phase != "restoring")
            {
                throw std::runtime_error("invalid color reorder recovery phase");
            }
            if (phase == "publishing")
            {
                // 先撤回全部已发布名字，再恢复原名，避免循环置换互相覆盖。
                for (const auto& entry : journal.at("entries"))
                {
                    const auto temporary = u8path(entry.at("temporary").get<std::string>());
                    if (!fs::exists(temporary))
                    {
                        const auto final = u8path(entry.at("final").get<std::string>());
                        requireSize(final, entry);
                        moveFile(final, temporary);
                    }
                    else if (fs::exists(u8path(entry.at("final").get<std::string>())))
                    {
                        const auto final = u8path(entry.at("final").get<std::string>());
                        if (!fs::equivalent(final, temporary))
                        {
                            throw std::runtime_error("occupied destination during color reorder recovery");
                        }
                        fs::remove(final);
                    }
                    requireSize(temporary, entry);
                }
            }
            journal["phase"] = "restoring";
            writeJournal(path, journal);
            for (const auto& entry : journal.at("entries"))
            {
                const auto temporary = u8path(entry.at("temporary").get<std::string>());
                const auto original = u8path(entry.at("original").get<std::string>());
                if (fs::exists(temporary))
                {
                    requireSize(temporary, entry);
                    moveFile(temporary, original);
                }
                requireSize(original, entry);
            }
        }
        cleanupJournal(path, journal);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("color reorder recovery failed; retain journal and restore access: ") + exception.what();
        return false;
    }
}

bool reorderLibraryByColor(Database& database, const std::string& outputDir,
                           ColorOrderResult& result, std::string& error)
{
    bool transaction = false;
    fs::path staging;
    try
    {
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        auto elapsed = [](Clock::time_point from)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
        };
        auto records = database.metadataRecords();
        if (records.size() != static_cast<size_t>(database.totalCount()))
        {
            throw std::runtime_error("cannot read all library records for color ordering");
        }
        const auto root = absolutePath(outputDir);
#ifdef _WIN32
        const auto rootVolume = fileInformation(root).volume;
#else
        // OverlayFS 的目录和图片可能报告不同的 st_dev；是否可移动由硬链接操作判定。
        if (!fs::is_directory(root))
        {
            throw std::runtime_error("color reorder output path is not a directory: " + pathToUtf8(root));
        }
#endif
        struct Source
        {
            fs::path path;
            uint64_t size;
        };
        std::unordered_map<int, Source> sourceInformation;
        sourceInformation.reserve(records.size());
        auto colors = database.imageColorOrders();
        std::vector<int> changedColors;
        std::set<fs::path> sources;
        for (const auto& record : records)
        {
            const auto source = absolutePath(record.filePath);
            const auto information = fileInformation(source);
            if (!information.regular || !sources.insert(normalizedIdentity(source)).second)
            {
                throw std::runtime_error("missing or aliased library image: " + record.filePath);
            }
#ifdef _WIN32
            if (information.volume != rootVolume)
            {
                throw std::runtime_error("color ordering cannot move images across volumes: " + record.filePath);
            }
#endif
            sourceInformation.emplace(record.id, Source{source, information.size});
            auto found = colors.find(record.id);
            if (found == colors.end() || found->second.version != kColorOrderVersion ||
                found->second.fileHash != record.fileHash ||
                found->second.sourceL != record.avgL || found->second.sourceA != record.avgA ||
                found->second.sourceB != record.avgB)
            {
                try
                {
                    colors[record.id] =
                    {
                        record.fileHash, kColorOrderVersion,
                        meanLabColorOrderKey(record.avgL, record.avgA, record.avgB),
                        record.avgL, record.avgA, record.avgB
                    };
                }
                catch (const std::invalid_argument& exception)
                {
                    throw std::runtime_error("invalid average color for image id=" +
                        std::to_string(record.id) + ": " + exception.what());
                }
                changedColors.push_back(record.id);
                ++result.computed;
            }
        }
        result.validationMs = elapsed(start);
        const auto sortStart = Clock::now();
        std::sort(records.begin(), records.end(),
            [&](const auto& left, const auto& right)
            {
                const auto leftKey = colors.at(left.id).key, rightKey = colors.at(right.id).key;
                if (leftKey != rightKey)
                {
                    return leftKey < rightKey;
                }
                if (left.fileHash != right.fileHash)
                {
                    return left.fileHash < right.fileHash;
                }
                return left.id < right.id;
            });
        result.sortingMs = elapsed(sortStart);
        const auto destinationCheckStart = Clock::now();
        const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        staging = root / (".mosaicraft-color-order-" + token);
        Json journal = {{"version", kJournalVersion}, {"library_version", kLibraryVersion},
            {"feature_version", kFeatureVersion}, {"uuid", database.getMeta("library_uuid")},
            {"database", database.path()}, {"token", token},
            {"output", pathToUtf8(root)}, {"staging", pathToUtf8(staging)},
            {"phase", "staging"}, {"entries", Json::array()}};
        const int width = std::max(6, static_cast<int>(std::to_string(records.empty() ? 0 : records.size() - 1).size()));
        for (size_t i = 0; i < records.size(); ++i)
        {
            const auto& record = records[i];
            const auto &original = sourceInformation.at(record.id).path;
            std::ostringstream name;
            name << std::setfill('0') << std::setw(width) << i << pathToUtf8(original.extension());
            const auto final = root / u8path(name.str());
            if (original == final)
            {
                continue;
            }
            if (fs::exists(final) && !sources.count(identity(final)))
            {
                throw std::runtime_error("unindexed file blocks color ordering: " + pathToUtf8(final));
            }
            journal["entries"].push_back({{"id", record.id},
                                          {"old_db_path", record.filePath},
                                          {"original", pathToUtf8(original)},
                                          {"final", pathToUtf8(final)},
                                          {"temporary", pathToUtf8(staging / (std::to_string(record.id) + ".stage"))},
                                          {"hash", record.fileHash},
                                          {"size", sourceInformation.at(record.id).size}});
        }
        result.validationMs += elapsed(destinationCheckStart);
        if (changedColors.empty() && journal["entries"].empty())
        {
            return true;
        }
        const auto writeStart = Clock::now();
        if (!database.beginImmediateTransaction())
        {
            throw std::runtime_error("cannot begin color ordering transaction");
        }
        transaction = true;
        for (const int id : changedColors)
        {
            if (!database.saveImageColorOrder(id, colors.at(id)))
            {
                throw std::runtime_error("cannot save image color order");
            }
        }
        const auto path = libraryJournalPath(database.path());
        if (!journal["entries"].empty())
        {
            if (!fs::create_directory(staging))
            {
                throw std::runtime_error("color reorder staging directory is occupied");
            }
            writeJournal(path, journal);
            for (const auto& entry : journal["entries"])
            {
                moveFile(u8path(entry.at("original").get<std::string>()),
                         u8path(entry.at("temporary").get<std::string>()));
                if (!database.updateImagePath(entry.at("id"), entry.at("old_db_path"), entry.at("temporary")))
                {
                    throw std::runtime_error("cannot stage database image path");
                }
            }
            journal["phase"] = "publishing";
            writeJournal(path, journal);
            for (const auto& entry : journal["entries"])
            {
                moveFile(u8path(entry.at("temporary").get<std::string>()),
                         u8path(entry.at("final").get<std::string>()));
                if (!database.updateImagePath(entry.at("id"), entry.at("temporary"), entry.at("final")))
                {
                    throw std::runtime_error("cannot finalize database image path");
                }
                ++result.renamed;
            }
            if (!database.setMeta("color_order_batch", token) || !database.advanceRevisions(false))
            {
                throw std::runtime_error("cannot save color reorder commit marker");
            }
        }
        if (!database.commitTransaction())
        {
            throw std::runtime_error("cannot commit color ordering transaction");
        }
        transaction = false;
        if (!journal["entries"].empty())
        {
            cleanupJournal(path, journal);
        }
        result.writingMs = elapsed(writeStart);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        if (transaction)
        {
            database.rollbackTransaction();
        }
        std::string recoveryError;
        if (!recoverLibraryColorOrder(database, recoveryError))
        {
            error += "; " + recoveryError;
        }
        else if (!staging.empty())
        {
            std::error_code cleanupError;
            if (fs::is_directory(staging, cleanupError) && fs::is_empty(staging, cleanupError))
            {
                fs::remove(staging, cleanupError);
            }
        }
        return false;
    }
}

} // namespace mosaicraft
