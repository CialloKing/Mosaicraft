#include "LibraryColorOrder.h"
#include "ColorOrder.h"
#include "Database.h"
#include "LibraryAccess.h"
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

fs::path identity(const fs::path& path)
{
    auto normalized = fs::weakly_canonical(path);
#ifdef _WIN32
    auto native = normalized.wstring();
    std::transform(native.begin(), native.end(), native.begin(), std::towlower);
    return fs::path(native);
#else
    return normalized;
#endif
}

bool sameVolume(const fs::path& file, const fs::path& directory)
{
#ifdef _WIN32
    auto volume = [](const fs::path& path)
    {
        const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        BY_HANDLE_FILE_INFORMATION information{};
        const bool ok = handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &information);
        if (handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
        if (!ok)
        {
            throw std::runtime_error("cannot determine library volume: " + pathToUtf8(path));
        }
        return information.dwVolumeSerialNumber;
    };
    return volume(file) == volume(directory);
#else
    struct stat source{}, destination{};
    return stat(file.c_str(), &source) == 0 && stat(directory.c_str(), &destination) == 0 &&
           source.st_dev == destination.st_dev;
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
    fs::create_hard_link(from, to);
    fs::remove(from);
#endif
}

void writeJournal(const fs::path& path, const Json& journal)
{
    const auto temporary = u8path(pathToUtf8(path) + ".tmp");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << journal.dump(2);
    output.close();
    if (!output)
    {
        throw std::runtime_error("cannot write color reorder journal");
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
        throw std::runtime_error("cannot persist color reorder journal");
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
        throw std::runtime_error("cannot persist color reorder journal");
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
        throw std::runtime_error("cannot persist color reorder journal directory");
    }
#endif
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
    if (journal.at("version") != 1 || journal.at("database") != database.path())
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
    for (auto& record : database.allRecords())
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
        const auto final = absolutePath(entry.at("final").get<std::string>());
        const auto temporary = absolutePath(entry.at("temporary").get<std::string>());
        if (found == records.end() || !ids.insert(id).second ||
            !originals.insert(identity(original)).second || !destinations.insert(identity(final)).second ||
            final.parent_path() != root || temporary != staging / (std::to_string(id) + ".stage") ||
            original != absolutePath(entry.at("old_db_path").get<std::string>()) ||
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

} // namespace

bool recoverLibraryColorOrder(Database& database, std::string& error)
{
    try
    {
        const auto path = libraryJournalPath(database.path());
        if (!fs::exists(path))
        {
            return true;
        }
        std::ifstream input(path, std::ios::binary);
        Json journal;
        input >> journal;
        input.close();
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
        auto records = database.allRecords();
        if (records.size() != static_cast<size_t>(database.totalCount()))
        {
            throw std::runtime_error("cannot read all library records for color ordering");
        }
        const auto root = absolutePath(outputDir);
        auto colors = database.imageColorOrders();
        std::vector<int> changedColors;
        std::set<fs::path> sources;
        for (const auto& record : records)
        {
            const auto source = absolutePath(record.filePath);
            if (!fs::is_regular_file(source) || !sources.insert(identity(source)).second)
            {
                throw std::runtime_error("missing or aliased library image: " + record.filePath);
            }
            if (!sameVolume(source, root))
            {
                throw std::runtime_error("color ordering cannot move images across volumes: " + record.filePath);
            }
            auto found = colors.find(record.id);
            if (found == colors.end() || found->second.version != kColorOrderVersion ||
                found->second.fileHash != record.fileHash)
            {
                colors[record.id] = {record.fileHash, kColorOrderVersion,
                                    imageColorOrderKey(imreadUnicode(record.filePath))};
                changedColors.push_back(record.id);
                ++result.computed;
            }
        }
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
        const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        staging = root / (".mosaicraft-color-order-" + token);
        Json journal = {{"version", 1}, {"database", database.path()}, {"token", token},
            {"output", pathToUtf8(root)}, {"staging", pathToUtf8(staging)},
            {"phase", "staging"}, {"entries", Json::array()}};
        const int width = std::max(6, static_cast<int>(std::to_string(records.empty() ? 0 : records.size() - 1).size()));
        for (size_t i = 0; i < records.size(); ++i)
        {
            const auto& record = records[i];
            const auto original = absolutePath(record.filePath);
            std::ostringstream name;
            name << std::setfill('0') << std::setw(width) << i << pathToUtf8(original.extension());
            const auto final = root / u8path(name.str());
            if (fs::exists(final) && !sources.count(identity(final)))
            {
                throw std::runtime_error("unindexed file blocks color ordering: " + pathToUtf8(final));
            }
            if (original == final)
            {
                continue;
            }
            journal["entries"].push_back({{"id", record.id}, {"old_db_path", record.filePath},
                {"original", pathToUtf8(original)}, {"final", pathToUtf8(final)},
                {"temporary", pathToUtf8(staging / (std::to_string(record.id) + ".stage"))},
                {"hash", record.fileHash}, {"size", fs::file_size(original)}});
        }
        if (changedColors.empty() && journal["entries"].empty())
        {
            return true;
        }
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
            if (!database.setMeta("color_order_batch", token))
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
