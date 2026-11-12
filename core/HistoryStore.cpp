#include "HistoryStore.h"
#include "ApiJson.h"
#include "UnicodeIO.h"
#include <memory>
#include <sqlite3.h>

namespace mosaicraft
{
namespace
{
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement prepare(sqlite3 *database, const char *sql)
{
    sqlite3_stmt *statement = nullptr;
    sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    return Statement(statement, sqlite3_finalize);
}

bool exec(sqlite3 *database, const char *sql)
{
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}
} // namespace

HistoryStore::HistoryStore() noexcept
{
    try
    {
        const auto directory = std::filesystem::current_path() / ".mosaicraft";
        std::filesystem::create_directories(directory);
        const auto path = pathToUtf8(directory / "history.sqlite3");
        if (sqlite3_open_v2(path.c_str(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) == SQLITE_OK)
        {
            sqlite3_busy_timeout(m_db, 1000);
            auto version = prepare(m_db, "PRAGMA user_version");
            if (version && sqlite3_step(version.get()) == SQLITE_ROW)
            {
                const int current = sqlite3_column_int(version.get(), 0);
                version.reset();
                if (current == 1 || (current == 0 && exec(m_db,
                    "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS completed (key TEXT PRIMARY KEY, finished INTEGER NOT NULL, payload TEXT NOT NULL);"
                    "PRAGMA user_version=1; COMMIT;")))
                {
                    return;
                }
            }
        }
    }
    catch (...)
    {
    }
    sqlite3_close(m_db);
    m_db = nullptr;
}

HistoryStore::~HistoryStore()
{
    sqlite3_close(m_db);
}

bool HistoryStore::record(const std::string &session, const JobSnapshot &snapshot) noexcept
{
    if (!m_db)
    {
        return false;
    }
    try
    {
        auto record = apiJobJson(snapshot).at("job");
        const auto key = session + ":" + snapshot.id;
        record["key"] = key;
        auto &outputs = record["outputs"] = nlohmann::json::array();
        for (const auto &item : snapshot.result.runtime.artifacts)
        {
            if (item.id == "main" || item.id == "preview" || item.id == "library" || item.id == "tiles" || item.id == "manifest")
            {
                outputs.push_back({{"id", item.id}, {"path", item.path}, {"bytes", item.bytes}});
            }
        }
        record.erase("artifacts");
        record.erase("progress");
        record.erase("warnings");
        auto &runtime = record["runtime"];
        runtime.erase("build");
        if (runtime.contains("resources"))
        {
            runtime["resources"].erase("samples");
        }
        const auto json = record.dump();
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto write = prepare(m_db, "INSERT OR REPLACE INTO completed (key, finished, payload) VALUES (?, ?, ?)");
        if (!write || !exec(m_db, "BEGIN IMMEDIATE"))
        {
            return false;
        }
        const auto finished = std::chrono::duration_cast<std::chrono::milliseconds>(snapshot.finishedAt.time_since_epoch()).count();
        sqlite3_bind_text(write.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(write.get(), 2, finished);
        sqlite3_bind_text(write.get(), 3, json.c_str(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
        const bool written = sqlite3_step(write.get()) == SQLITE_DONE;
        write.reset();
        if (written && exec(m_db, "DELETE FROM completed WHERE rowid IN ("
                                "SELECT rowid FROM completed ORDER BY finished DESC, rowid DESC LIMIT -1 OFFSET 100)") &&
            exec(m_db, "COMMIT"))
        {
            return true;
        }
        exec(m_db, "ROLLBACK");
    }
    catch (...)
    {
    }
    return false;
}

bool HistoryStore::list(nlohmann::json &records) const
{
    if (!m_db)
    {
        return false;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto query = prepare(m_db, "SELECT payload FROM completed ORDER BY finished DESC, rowid DESC LIMIT 100");
    if (!query)
    {
        return false;
    }
    records = nlohmann::json::array();
    int state = sqlite3_step(query.get());
    while (state == SQLITE_ROW)
    {
        const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(query.get(), 0));
        if (text)
        {
            auto item = nlohmann::json::parse(text, nullptr, false);
            if (item.is_object())
            {
                records.push_back(std::move(item));
            }
        }
        state = sqlite3_step(query.get());
    }
    return state == SQLITE_DONE;
}

bool HistoryStore::clear(int &removed)
{
    if (!m_db)
    {
        return false;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!exec(m_db, "DELETE FROM completed"))
    {
        return false;
    }
    removed = sqlite3_changes(m_db);
    return true;
}
} // namespace mosaicraft
