#include "Database.h"
#include "LibraryPaths.h"
#include "LibraryAccess.h"
#include "RuntimeContext.h"
#include "UnicodeIO.h"
#include <sqlite3.h>
#include <array>
#include <chrono>
#include <iostream>
#include <set>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mosaicraft
{
bool Database::openConnection(bool create)
{
    if (sqlite3_open_v2(m_path.c_str(), &m_db, SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0), nullptr) != SQLITE_OK)
    {
        m_error = m_db ? sqlite3_errmsg(m_db) : "cannot allocate SQLite connection";
        closeConnection();
        return false;
    }
    sqlite3_busy_timeout(m_db, 5000);
    return true;
}
void Database::closeConnection()
{
    for (auto **statement : {&m_insertStatement, &m_hashStatement, &m_pathStatement, &m_colorStatement})
    {
        sqlite3_finalize(*statement);
        *statement = nullptr;
    }
    if (m_db)
    {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}
bool Database::validateStoredPaths()
{
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT file_path,tiny_path,hist_path FROM images", -1, &statement, nullptr) != SQLITE_OK)
    {
        m_error = sqlite3_errmsg(m_db);
        return false;
    }
    int rc;
    bool valid = true;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW)
    {
        for (int column = 0; column < 3; ++column)
        {
            const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
            valid = valid && LibraryPaths::relative(value ? value : "");
        }
    }
    sqlite3_finalize(statement);
    if (!valid || rc != SQLITE_DONE)
    {
        m_error = "invalid stored portable library path";
        return false;
    }
    return true;
}
namespace
{
struct PathRow
{
    sqlite3_int64 id;
    std::array<std::string, 3> paths;
    std::array<bool, 3> nulls;
};
struct PathLess
{
    bool operator()(const std::string &a, const std::string &b) const
    {
#ifdef _WIN32
        return CompareStringOrdinal(u8path(a).c_str(), -1, u8path(b).c_str(), -1, TRUE) == CSTR_LESS_THAN;
#else
        return a < b;
#endif
    }
};
struct Statement
{
    sqlite3_stmt *value = nullptr;
    ~Statement()
    {
        sqlite3_finalize(value);
    }
};
void requireSql(bool ok, sqlite3 *db)
{
    if (!ok)
    {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}
void backupDatabase(sqlite3 *source, const std::filesystem::path &path)
{
    sqlite3 *target = nullptr;
    const auto name = pathToUtf8(path);
    if (sqlite3_open_v2(name.c_str(), &target, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    {
        const std::string reason = target ? sqlite3_errmsg(target) : "out of memory";
        sqlite3_close(target);
        throw std::runtime_error("cannot create pre-upgrade backup: " + reason);
    }
    auto *backup = sqlite3_backup_init(target, "main", source, "main");
    int rc = SQLITE_ERROR;
    if (backup)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            rc = sqlite3_backup_step(backup, 128);
            if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        } while ((rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
                 std::chrono::steady_clock::now() < deadline);
        const int finished = sqlite3_backup_finish(backup);
        if (finished != SQLITE_OK)
        {
            rc = finished;
        }
    }
    bool valid = false;
    {
        Statement check;
        valid = rc == SQLITE_DONE && sqlite3_prepare_v2(target, "PRAGMA quick_check", -1, &check.value, nullptr) == SQLITE_OK &&
            sqlite3_step(check.value) == SQLITE_ROW &&
            std::string(reinterpret_cast<const char *>(sqlite3_column_text(check.value, 0))) == "ok";
    }
    sqlite3_close(target);
    if (!valid)
    {
        throw std::runtime_error("pre-upgrade SQLite backup could not be verified");
    }
}
}

bool Database::ensurePortablePaths()
{
    auto format = getMeta("path_format");
    if (format == portablePathFormat)
    {
        m_portable = true;
        return validateStoredPaths();
    }
    if (!format.empty())
    {
        m_error = "unsupported library path format: " + format;
        return false;
    }
    // 尚未建表或正在恢复旧文件名时不能改写路径语义。
    if (!columnExists("images", "file_path") || std::filesystem::exists(libraryJournalPath(m_path)))
    {
        return true;
    }
    const bool releaseExclusive = !m_exclusive;
    if (releaseExclusive)
    {
        // 多个读取者不得持共享锁直接升级；关闭连接后竞争独占锁并重读标记。
        closeConnection();
        m_access.reset();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            m_access = std::make_unique<LibraryAccess>(m_path, true);
            if (m_access->acquired())
            {
                if (!openConnection(false))
                {
                    return false;
                }
                break;
            }
            m_access.reset();
            // 另一个进程可能已经迁移完毕且正在长时间读取；无需再等待其独占锁。
            m_access = std::make_unique<LibraryAccess>(m_path, false);
            if (m_access->acquired())
            {
                if (!openConnection(false))
                {
                    return false;
                }
                if (getMeta("path_format") == portablePathFormat)
                {
                    m_portable = true;
                    return validateStoredPaths();
                }
                closeConnection();
            }
            m_access.reset();
            if (std::chrono::steady_clock::now() >= deadline)
            {
                m_error = "library is busy during path upgrade";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    bool success = true;
    format = getMeta("path_format");
    if (format == portablePathFormat)
    {
        m_portable = true;
        success = validateStoredPaths();
    }
    else if (!format.empty())
    {
        m_error = "unsupported library path format: " + format;
        success = false;
    }
    else
    {
        m_pathNotice.clear();
        std::vector<PathRow> rows;
        try
        {
            const LibraryPaths paths(m_path);
            std::set<std::string, PathLess> names;
            Statement query;
            requireSql(sqlite3_prepare_v2(m_db, "SELECT id,file_path,tiny_path,hist_path FROM images ORDER BY id", -1,
                                          &query.value, nullptr) == SQLITE_OK, m_db);
            int rc;
            while ((rc = sqlite3_step(query.value)) == SQLITE_ROW)
            {
                PathRow row{};
                row.id = sqlite3_column_int64(query.value, 0);
                for (int column = 0; column < 3; ++column)
                {
                    row.nulls[column] = sqlite3_column_type(query.value, column + 1) == SQLITE_NULL;
                    const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(query.value, column + 1));
                    row.paths[column] = paths.legacy(value ? value : "");
                }
                if (!names.insert(row.paths[0]).second)
                {
                    throw std::invalid_argument("converted image paths conflict");
                }
                rows.push_back(std::move(row));
            }
            requireSql(rc == SQLITE_DONE, m_db);
        }
        catch (const std::invalid_argument &error)
        {
            m_pathNotice = std::string("Library paths retained in legacy format: ") + error.what();
            std::cerr << m_pathNotice << std::endl;
        }
        catch (const std::exception &error)
        {
            m_error = std::string("cannot inspect legacy library paths: ") + error.what();
            success = false;
        }
        if (success && m_pathNotice.empty())
        {
            bool transaction = false;
            try
            {
                const auto root = u8path(m_path).parent_path() / ".mosaicraft-backups";
                std::filesystem::create_directories(root);
                const auto token = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
                const auto backup = root / (u8path(m_path).filename().u8string() + ".before-paths-" + token + ".db");
                backupDatabase(m_db, backup);
                m_pathBackup = pathToUtf8(backup);
                requireSql(exec("PRAGMA synchronous=FULL") && exec("BEGIN IMMEDIATE"), m_db);
                transaction = true;
                // 先释放旧 UNIQUE 名字；所有临时值和最终值都只存在于同一事务内。
                Statement staging, update;
                requireSql(sqlite3_prepare_v2(m_db, "UPDATE images SET file_path=? WHERE id=?", -1,
                                              &staging.value, nullptr) == SQLITE_OK, m_db);
                requireSql(sqlite3_prepare_v2(m_db, "UPDATE images SET file_path=?,tiny_path=?,hist_path=? WHERE id=?", -1,
                                              &update.value, nullptr) == SQLITE_OK, m_db);
                for (const auto &row : rows)
                {
                    const auto temporary = ".__path_upgrade__/" + token + "/" + std::to_string(row.id);
                    sqlite3_bind_text(staging.value, 1, temporary.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(staging.value, 2, row.id);
                    requireSql(sqlite3_step(staging.value) == SQLITE_DONE, m_db);
                    sqlite3_reset(staging.value);
                }
                for (const auto &row : rows)
                {
                    for (int column = 0; column < 3; ++column)
                    {
                        if (row.nulls[column])
                        {
                            sqlite3_bind_null(update.value, column + 1);
                        }
                        else
                        {
                            sqlite3_bind_text(update.value, column + 1, row.paths[column].c_str(), -1, SQLITE_TRANSIENT);
                        }
                    }
                    sqlite3_bind_int64(update.value, 4, row.id);
                    requireSql(sqlite3_step(update.value) == SQLITE_DONE, m_db);
                    sqlite3_reset(update.value);
                }
                requireSql(exec("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)") &&
                           setMeta("path_format", portablePathFormat) && exec("COMMIT"), m_db);
                transaction = false;
                m_portable = true;
                std::cout << "Library paths upgraded; backup: " << m_pathBackup << std::endl;
            }
            catch (const std::exception &error)
            {
                if (transaction)
                {
                    exec("ROLLBACK");
                }
                m_error = std::string("automatic library path upgrade failed: ") + error.what();
                success = false;
            }
        }
    }
    if (releaseExclusive && success)
    {
        closeConnection();
        m_access.reset();
        m_access = std::make_unique<LibraryAccess>(m_path, false, 5000);
        if (!m_access->acquired() || !openConnection(false))
        {
            m_error = "library is busy after path upgrade";
            return false;
        }
    }
    return success;
}
void Database::reportPaths(RuntimeContext &runtime) const
{
    if (!m_pathNotice.empty())
    {
        runtime.emit("warning", "library_path_upgrade_deferred", m_pathNotice, "dbPath", m_path);
    }
    if (!m_pathBackup.empty())
    {
        runtime.emit("info", "library_paths_upgraded", "Library paths upgraded; backup: " + m_pathBackup,
                     "dbPath", m_path);
    }
}
} // namespace mosaicraft
