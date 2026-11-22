#include "Database.h"
#include "LibraryPaths.h"
#include "LibrarySchemaCheck.h"
#include "UnicodeIO.h"
#include "Version.h"
#include <sqlite3.h>
#include <charconv>
#include <chrono>
#include <map>
#include <string>

namespace mosaicraft
{
bool Database::preflightFeatureSchema()
{
    std::error_code error;
    const auto wal = std::filesystem::symlink_status(u8path(m_path + "-wal"), error);
    if (wal.type() != std::filesystem::file_type::not_found)
    {
        // 已有 WAL 可能含有新建的表，正常只读连接会看到一致的 SQLite 快照。
        return true;
    }
    // 普通只读 SQLite 仍可能创建空 WAL/SHM；无 WAL 时先探测结构，不污染被拒绝的目录。
    const auto uri = immutableLibraryUri(u8path(m_path).generic_u8string());
    const int opened = sqlite3_open_v2(uri.c_str(), &m_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    sqlite3_stmt *probe = nullptr;
    const bool valid = opened == SQLITE_OK &&
        sqlite3_prepare_v2(m_db, featureSchemaProbe, -1, &probe, nullptr) == SQLITE_OK;
    if (!valid)
    {
        m_error = opened == SQLITE_OK ? "library_incompatible: missing feature table or columns; rebuild the library in a new directory"
                                     : m_db ? sqlite3_errmsg(m_db) : "cannot allocate SQLite connection";
    }
    sqlite3_finalize(probe);
    closeConnection();
    return valid;
}

bool Database::openConnection(bool create, bool readOnly)
{
    const int flags = readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(m_path.c_str(), &m_db, flags, nullptr) != SQLITE_OK)
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
    for (auto **statement : {&m_insertStatement, &m_featureStatement, &m_textureStatement, &m_hashStatement, &m_pathStatement, &m_colorStatement})
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

bool Database::validateSchema(std::vector<ImageRecord> *matchingRows, double *readMs, bool includeUseCounts)
{
    auto incompatible = [&](const std::string &reason)
    {
        m_error = "library_incompatible: " + reason + "; rebuild the library in a new directory";
        return false;
    };
    auto scalar = [&](const std::string &sql, int expected)
    {
        sqlite3_stmt *statement = nullptr;
        bool ok = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == expected;
        sqlite3_finalize(statement);
        return ok;
    };
    if (!scalar("PRAGMA user_version", kLibraryVersion))
    {
        return incompatible("unsupported library version");
    }
    // 用实际 SQL 编译检查完整表结构，不按缺失字段推测旧版本。
    const std::map<std::string, std::string> tables
    {
        {"images", "id,file_path,file_hash,format,src_width,src_height,aspect_ratio,file_size,avg_l,avg_a,avg_b,"
                   "mean_brightness,std_brightness,contrast,saturation,color_variance,"
                   "edge_density,feature_version,use_count,created_at"},
        {"image_features", "image_id,grid_lab,tiny,lbp"},
        {"image_target_runs", "image_id,target_hash"},
        {"meta", "key,value"},
        {"image_color_order", "image_id,file_hash,version,sort_key,source_l,source_a,source_b"},
        {"usage_stats", "image_id,total_runs,total_tiles,last_used"},
        {"target_runs", "target_hash,first_path,run_count,last_used"}
    };
    for (const auto &table : tables)
    {
        sqlite3_stmt *statement = nullptr;
        const auto sql = "SELECT " + table.second + " FROM " + table.first + " LIMIT 0";
        const bool valid = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        if (!valid)
        {
            return incompatible("incomplete table: " + table.first);
        }
    }
    // 主键和非空约束决定后续更新语义；只有列名相同的仿造表不能视为完整图库。
    if (!scalar("SELECT count(*) FROM pragma_table_info('images') WHERE "
                "(name='id' AND type='INTEGER' AND pk=1) OR "
                "(name IN ('file_path','file_hash','feature_version') AND \"notnull\"=1)", 4) ||
        !scalar("SELECT count(*) FROM pragma_table_info('meta') WHERE "
                "(name='key' AND type='TEXT' AND pk=1) OR (name='value' AND \"notnull\"=1)", 2) ||
        !scalar("SELECT count(*) FROM pragma_table_info('image_color_order') WHERE "
                "(name='image_id' AND type='INTEGER' AND pk=1) OR "
                "(name IN ('file_hash','version','sort_key') AND \"notnull\"=1)", 4) ||
        !scalar("SELECT count(*) FROM pragma_table_info('usage_stats') WHERE "
                "name='image_id' AND type='INTEGER' AND pk=1", 1) ||
        !scalar("SELECT count(*) FROM pragma_table_info('target_runs') WHERE "
                "name='target_hash' AND type='TEXT' AND pk=1", 1) ||
        !scalar("SELECT count(*) FROM pragma_index_list('images') i WHERE i.\"unique\"=1 AND "
                "(SELECT count(*) FROM pragma_index_info(i.name))=1 AND "
                "(SELECT name FROM pragma_index_info(i.name))='file_path'", 1) ||
        !scalar("SELECT count(*) FROM pragma_foreign_key_list('usage_stats') WHERE "
                "\"table\"='images' AND \"from\"='image_id' AND \"to\"='id' AND on_delete='CASCADE'", 1) ||
        !scalar("SELECT count(*) FROM pragma_foreign_key_list('image_color_order') WHERE "
                "\"table\"='images' AND \"from\"='image_id' AND \"to\"='id' AND on_delete='CASCADE'", 1))
    {
        return incompatible("missing library constraints");
    }
    if (!scalar("SELECT count(*) FROM pragma_foreign_key_list('image_features') WHERE "
                "\"table\"='images' AND \"from\"='image_id' AND \"to\"='id' AND on_delete='CASCADE'", 1))
    {
        return incompatible("missing feature foreign key");
    }
    if (getMeta("path_format") != portablePathFormat ||
        getMeta("feature_version") != std::to_string(kFeatureVersion))
    {
        return incompatible("unsupported path or feature version");
    }
    if (getMeta("library_uuid").size() != 32)
    {
        return incompatible("missing library identity");
    }
    for (const auto *key : {"feature_revision", "asset_revision"})
    {
        const auto text = getMeta(key);
        std::int64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 0)
        {
            return incompatible("invalid library revision");
        }
    }
    for (const auto *key : {"feature_w", "feature_h"})
    {
        const auto text = getMeta(key);
        int value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 1 || value > 8192)
        {
            return incompatible("invalid feature dimensions");
        }
    }
    bool rowsValid = true;
    if (matchingRows)
    {
        using Clock = std::chrono::steady_clock;
        const auto start = readMs ? Clock::now() : Clock::time_point{};
        rowsValid = readMatchingRows(*matchingRows, includeUseCounts);
        if (readMs)
        {
            *readMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }
    }
    if (!rowsValid && m_error.empty())
    {
        return incompatible("invalid feature rows or stored paths");
    }
    return rowsValid;
}
}
