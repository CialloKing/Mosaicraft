#include "Database.h"
#include "LibraryPaths.h"
#include "RuntimeContext.h"
#include "UnicodeIO.h"
#include <sqlite3.h>

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
bool Database::ensurePortablePaths()
{
    const auto format = getMeta("path_format");
    if (format.empty())
    {
        return true;
    }
    if (format != portablePathFormat)
    {
        m_error = "unsupported library path format: " + format;
        return false;
    }
    m_portable = true;
    return validateStoredPaths();
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
