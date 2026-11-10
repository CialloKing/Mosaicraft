#include "Database.h"
#include "LibraryPaths.h"
#include "Version.h"
#include "FeatureIO.h"
#include <sqlite3.h>
#include <charconv>
#include <map>
#include <string>

namespace mosaicraft
{
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
    if (sqlite3_prepare_v2(m_db, "SELECT file_path,tiny_path,lbp_path,grid_lab,avg_l,avg_a,avg_b FROM images", -1, &statement, nullptr) != SQLITE_OK)
    {
        return false;
    }
    int rc;
    bool valid = true;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW)
    {
        for (int column = 0; column < 3; ++column)
        {
            const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
            valid = valid && (column != 0 || (value && *value)) && LibraryPaths::relative(value ? value : "");
        }
        const auto *grid = static_cast<const uint8_t *>(sqlite3_column_blob(statement, 3));
        if (!grid || sqlite3_column_bytes(statement, 3) != 768)
        {
            valid = false;
            continue;
        }
        std::array<float, 192> values{};
        decodeFloats(grid, values.data(), values.size());
        for (float value : values)
        {
            valid = valid && std::isfinite(value);
        }
        for (int column = 4; column < 7; ++column)
        {
            valid = valid && sqlite3_column_type(statement, column) != SQLITE_NULL &&
                std::isfinite(sqlite3_column_double(statement, column));
        }
    }
    sqlite3_finalize(statement);
    return valid && rc == SQLITE_DONE;
}

bool Database::validateSchema()
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
                   "mean_brightness,std_brightness,contrast,saturation,color_variance,grid_lab,tiny_path,"
                   "edge_density,lbp_path,feature_version,use_count,created_at"},
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
    if (!scalar("SELECT count(*) FROM pragma_table_info('images') WHERE "
                "(name='id' AND type='INTEGER' AND pk=1) OR (name='file_path' AND \"notnull\"=1)", 2) ||
        !scalar("SELECT count(*) FROM pragma_index_list('images') i WHERE i.\"unique\"=1 AND "
                "(SELECT count(*) FROM pragma_index_info(i.name))=1 AND "
                "(SELECT name FROM pragma_index_info(i.name))='file_path'", 1) ||
        !scalar("SELECT count(*) FROM pragma_foreign_key_list('usage_stats') WHERE "
                "\"table\"='images' AND \"from\"='image_id' AND on_delete='CASCADE'", 1))
    {
        return incompatible("missing library constraints");
    }
    if (getMeta("path_format") != portablePathFormat ||
        getMeta("feature_version") != std::to_string(kFeatureVersion))
    {
        return incompatible("unsupported path or feature version");
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
    if (!scalar("SELECT count(*) FROM images WHERE feature_version IS NOT " + std::to_string(kFeatureVersion) +
                " OR grid_lab IS NULL OR typeof(grid_lab)!='blob' OR length(grid_lab)!=768", 0) ||
        !validateStoredPaths())
    {
        return incompatible("invalid feature rows or stored paths");
    }
    sqlite3_stmt *check = nullptr;
    const bool valid = sqlite3_prepare_v2(m_db, "PRAGMA quick_check(1)", -1, &check, nullptr) == SQLITE_OK &&
        sqlite3_step(check) == SQLITE_ROW && sqlite3_column_text(check, 0) &&
        std::string(reinterpret_cast<const char *>(sqlite3_column_text(check, 0))) == "ok";
    sqlite3_finalize(check);
    return valid || incompatible("damaged SQLite database");
}
}
