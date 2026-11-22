#include "Database.h"

#include <sqlite3.h>
#include "LibraryAccess.h"
#include "LibraryPaths.h"
#include "UnicodeIO.h"
#include "Version.h"
#include "FeatureIO.h"
#include "FeatureSql.h"

#include <cstring>
#include <algorithm>
#include <iostream>
#include <tuple>
#include <unordered_map>
#include <string>
#include <unordered_set>

namespace mosaicraft
{

// ============================================================
// 构造 / 析构
// ============================================================

Database::Database(const std::string &dbPath, OpenMode mode, std::vector<ImageRecord> *matchingRows,
                   double *readMs, bool includeUseCounts) : m_db(nullptr)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(u8path(dbPath), error);
    if (!error)
    {
        m_path = pathToUtf8(std::filesystem::weakly_canonical(absolute, error));
    }
    if (error)
    {
        m_error = "cannot resolve database: " + dbPath;
        return;
    }
    const bool allowCreate = mode == OpenMode::Create || mode == OpenMode::Build;
    m_new = !std::filesystem::exists(u8path(m_path), error);
    m_exclusive = mode == OpenMode::Build || mode == OpenMode::ExclusiveExisting || mode == OpenMode::Recover;
    if (error || (m_new && !allowCreate))
    {
        m_error = "database missing or cannot be inspected: " + dbPath;
        return;
    }
    m_paths = std::make_unique<LibraryPaths>(m_path);
    const bool exclusiveLock = m_exclusive || m_new;
    m_access = std::make_unique<LibraryAccess>(m_path, exclusiveLock, 5000);
    if (!m_access->acquired())
    {
        m_error = "library is busy or its operation lock cannot be opened: " + dbPath;
        m_access.reset();
        return;
    }
    try
    {
        // 等锁期间另一个进程可能已经建库或开始重排，必须在锁内重新判断。
        m_new = !std::filesystem::exists(u8path(m_path));
        if (m_new && (!allowCreate || !exclusiveLock))
        {
            m_error = "database missing: " + dbPath;
        }
    }
    catch (const std::filesystem::filesystem_error &exception)
    {
        m_error = exception.what();
    }
    if (!m_error.empty())
    {
        m_access.reset();
        return;
    }
    // 先只读校验；旧库失败时不得执行 WAL 设置、DDL 或任何升级。
    if (!m_new && !preflightFeatureSchema())
    {
        m_access.reset();
        return;
    }
    bool valid = openConnection(m_new, !m_new);
    if (valid && !m_new)
    {
        // 在只读连接的同一快照内校验并取走匹配行；损坏库仍在打开可写连接前拒绝。
        valid = (!matchingRows || beginTransaction()) && validateSchema(matchingRows, readMs, includeUseCounts) &&
                (!matchingRows || commitTransaction());
        if (valid && mode != OpenMode::Recover)
        {
            try
            {
                if (std::filesystem::exists(libraryJournalStatus(libraryIngestJournalPath(m_path))))
                {
                    m_error = "unfinished library ingestion; retain the journal and staging directory, then run "
                              "library add <input-directory> -l <library>: " + m_path;
                    valid = false;
                }
                else if (std::filesystem::exists(libraryJournalStatus(libraryJournalPath(m_path))))
                {
                    m_error = "unfinished color reorder; retain the journal and staging directory, then run "
                              "library add <input-directory> -l <library>: " + m_path;
                    valid = false;
                }
            }
            catch (const std::filesystem::filesystem_error &exception)
            {
                m_error = exception.what();
                valid = false;
            }
        }
        if (valid && mode != OpenMode::ReadOnly)
        {
            closeConnection();
            valid = openConnection(false);
        }
    }
    if (valid)
    {
        valid = exec("PRAGMA foreign_keys=ON");
        if (valid && !m_new && mode != OpenMode::ReadOnly)
        {
            // 只在新版结构校验通过后恢复正常 WAL 配置，拒绝旧库时不写入。
            valid = exec("PRAGMA journal_mode=WAL") && exec("PRAGMA synchronous=NORMAL");
        }
    }
    if (!valid)
    {
        closeConnection();
        m_access.reset();
    }
}

Database::~Database()
{
    closeConnection();
}

bool Database::isOpen() const
{
    return m_db != nullptr;
}

// ============================================================
// 建表
// ============================================================

bool Database::createTables(int featureWidth, int featureHeight)
{
    if (!m_db)
    {
        return false;
    }

    if (!m_new)
    {
        return true;
    }
    // 完整结构与标记一次提交；已有库永远不补表或补列。
    if (!exec("PRAGMA page_size=8192") || !exec("BEGIN IMMEDIATE"))
    {
        return false;
    }
    const char* sql = R"SQL(
        CREATE TABLE images (
            -- 主键
            id              INTEGER PRIMARY KEY AUTOINCREMENT,

            -- 元数据
            file_path       TEXT NOT NULL UNIQUE,
            file_hash       TEXT NOT NULL,
            format          TEXT DEFAULT '',
            src_width       INTEGER DEFAULT 0,
            src_height      INTEGER DEFAULT 0,
            aspect_ratio    REAL DEFAULT 0.0,
            file_size       INTEGER DEFAULT 0,

            -- 全局颜色特征
            avg_l           REAL DEFAULT 0.0,
            avg_a           REAL DEFAULT 0.0,
            avg_b           REAL DEFAULT 0.0,

            -- 亮度统计
            mean_brightness REAL DEFAULT 0.0,
            std_brightness  REAL DEFAULT 0.0,
            contrast        REAL DEFAULT 0.0,

            -- 颜色统计
            saturation      REAL DEFAULT 0.0,
            color_variance  REAL DEFAULT 0.0,

            edge_density    REAL DEFAULT 0.0,

            -- 使用统计
            use_count       INTEGER DEFAULT 0,

            -- 版本标记
            feature_version INTEGER NOT NULL,

            -- 时间戳
            created_at      TEXT DEFAULT (datetime('now'))
        );

        CREATE INDEX idx_hash    ON images(file_hash);
        CREATE INDEX idx_avg_l   ON images(avg_l);
        CREATE INDEX idx_use     ON images(use_count);

        CREATE TABLE image_features (
            image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
            grid_lab BLOB NOT NULL CHECK(typeof(grid_lab)='blob' AND length(grid_lab)=768),
            tiny BLOB NOT NULL CHECK(typeof(tiny)='blob' AND length(tiny)=256),
            lbp BLOB NOT NULL CHECK(typeof(lbp)='blob' AND length(lbp)=1024)
        );

        CREATE TABLE meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE image_color_order (
            image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
            file_hash TEXT NOT NULL,
            version INTEGER NOT NULL,
            sort_key INTEGER NOT NULL,
            source_l REAL,
            source_a REAL,
            source_b REAL
        );
    )SQL";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Create tables error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        exec("ROLLBACK");
        return false;
    }

    if (!exec("CREATE TABLE usage_stats(image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,"
              "total_runs INTEGER DEFAULT 0,total_tiles INTEGER DEFAULT 0,last_used TEXT);") ||
        !exec("CREATE TABLE target_runs(target_hash TEXT PRIMARY KEY,first_path TEXT,"
              "run_count INTEGER DEFAULT 0,last_used TEXT);") ||
        !exec("CREATE TABLE image_target_runs (image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE, "
              "target_hash TEXT NOT NULL REFERENCES target_runs(target_hash), "
              "PRIMARY KEY(image_id, target_hash)) WITHOUT ROWID") ||
        !exec("INSERT INTO meta VALUES('library_uuid', lower(hex(randomblob(16))))") ||
        !setMeta("feature_revision", "0") || !setMeta("asset_revision", "0") ||
        !setMeta("path_format", portablePathFormat) ||
        !setMeta("feature_version", std::to_string(kFeatureVersion)) ||
        !setMeta("feature_w", std::to_string(featureWidth)) ||
        !setMeta("feature_h", std::to_string(featureHeight)) ||
        !exec("PRAGMA user_version=" + std::to_string(kLibraryVersion)) || !exec("COMMIT"))
    {
        exec("ROLLBACK");
        return false;
    }
    m_new = false;
    if (!exec("PRAGMA journal_mode=WAL") || !exec("PRAGMA synchronous=NORMAL"))
    {
        return false;
    }
    if (!m_exclusive)
    {
        m_access.reset();
        m_access = std::make_unique<LibraryAccess>(m_path, false, 5000);
        if (!m_access->acquired())
        {
            m_error = "library is busy after initialization";
            return false;
        }
    }
    return true;
}

// ============================================================
// Meta
// ============================================================

bool Database::setMeta(const std::string& key, const std::string& value)
{
    if (!m_db) return false;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db,
        "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)",
        -1, &stmt, nullptr);
    if (!stmt) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::string Database::getMeta(const std::string& key, const std::string& defaultVal)
{
    if (!m_db) return defaultVal;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT value FROM meta WHERE key = ?",
        -1, &stmt, nullptr);
    if (!stmt) return defaultVal;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
    std::string result = defaultVal;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (val) result = val;
    }
    sqlite3_finalize(stmt);
    return result;
}

// ============================================================
// 插入
// ============================================================

bool Database::insertImage(const ImageRecord &rec, const TextureFeatures &texture)
{
    const auto finite = [](double value)
    {
        return std::isfinite(value);
    };
    const std::array scalars{rec.avgL, rec.avgA, rec.avgB, rec.edgeDensity, rec.meanBrightness,
                             rec.stdBrightness, rec.contrast, rec.saturation, rec.colorVariance, rec.aspectRatio};
    if (!inTransaction() || rec.featureVersion != kFeatureVersion || rec.gridLab.size() != 192 ||
        !std::all_of(rec.gridLab.begin(), rec.gridLab.end(), finite) ||
        !std::all_of(texture.lbp.begin(), texture.lbp.end(), finite) ||
        !std::all_of(scalars.begin(), scalars.end(), finite))
    {
        m_error = "invalid feature record or missing write transaction";
        return false;
    }
    std::string file;
    try
    {
        file = m_paths->store(rec.filePath);
    }
    catch (const std::exception &error)
    {
        m_error = error.what();
        return false;
    }
    const char *sql = R"SQL(
        INSERT INTO images(file_path,file_hash,format,src_width,src_height,aspect_ratio,file_size,
             avg_l,avg_a,avg_b,mean_brightness,std_brightness,contrast,saturation,color_variance,
             edge_density,feature_version)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    )SQL";
    auto *stmt = prepareCached(m_insertStatement, sql);
    auto *features = prepareCached(m_featureStatement,
        "INSERT INTO image_features(image_id,grid_lab,tiny,lbp) VALUES(?,?,?,?)");
    if (!stmt || !features)
    {
        return false;
    }
    const bool bound = sqlite3_bind_text(stmt, 1, file.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 2, rec.fileHash.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 3, rec.format.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 4, rec.srcWidth) == SQLITE_OK && sqlite3_bind_int(stmt, 5, rec.srcHeight) == SQLITE_OK &&
        sqlite3_bind_double(stmt, 6, rec.aspectRatio) == SQLITE_OK && sqlite3_bind_int64(stmt, 7, rec.fileSize) == SQLITE_OK &&
        sqlite3_bind_double(stmt, 8, rec.avgL) == SQLITE_OK && sqlite3_bind_double(stmt, 9, rec.avgA) == SQLITE_OK &&
        sqlite3_bind_double(stmt, 10, rec.avgB) == SQLITE_OK &&
        sqlite3_bind_double(stmt, 11, rec.meanBrightness) == SQLITE_OK &&
        sqlite3_bind_double(stmt, 12, rec.stdBrightness) == SQLITE_OK &&
        sqlite3_bind_double(stmt, 13, rec.contrast) == SQLITE_OK && sqlite3_bind_double(stmt, 14, rec.saturation) == SQLITE_OK &&
        sqlite3_bind_double(stmt, 15, rec.colorVariance) == SQLITE_OK && sqlite3_bind_double(stmt, 16, rec.edgeDensity) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 17, rec.featureVersion) == SQLITE_OK;
    const bool inserted = bound && sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_reset(stmt);
    if (!inserted)
    {
        m_error = sqlite3_errmsg(m_db);
        return false;
    }
    std::array<uint8_t, 768> grid{};
    std::array<uint8_t, 1024> lbp{};
    encodeFloats(rec.gridLab.data(), rec.gridLab.size(), grid.data());
    encodeFloats(texture.lbp.data(), texture.lbp.size(), lbp.data());
    const bool ok = sqlite3_bind_int64(features, 1, sqlite3_last_insert_rowid(m_db)) == SQLITE_OK &&
        sqlite3_bind_blob(features, 2, grid.data(), static_cast<int>(grid.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_blob(features, 3, texture.tiny.data(), static_cast<int>(texture.tiny.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_blob(features, 4, lbp.data(), static_cast<int>(lbp.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(features) == SQLITE_DONE;
    if (!ok)
    {
        m_error = sqlite3_errmsg(m_db);
    }
    sqlite3_reset(features);
    return ok;
}

bool Database::inTransaction() const
{
    return m_db && !sqlite3_get_autocommit(m_db);
}

bool Database::advanceRevisions(bool features)
{
    return exec(features
        ? "UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key IN ('feature_revision','asset_revision')"
        : "UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='asset_revision'");
}

std::string Database::featureKey()
{
    return m_path + ':' + getMeta("library_uuid") + ':' + getMeta("feature_revision");
}

std::string Database::assetKey()
{
    return featureKey() + ':' + getMeta("asset_revision");
}

bool Database::readTextures(const std::vector<int> &ids, std::unordered_map<int, TextureFeatures> &out)
{
    // 固定大小的 IN 批次复用同一语句；未使用的参数留为 NULL。
    constexpr size_t chunk = 128;
    if (!m_textureStatement)
    {
        std::string sql = "SELECT image_id,tiny,lbp FROM image_features WHERE image_id IN (?";
        for (size_t i = 1; i < chunk; ++i)
        {
            sql += ",?";
        }
        if (!prepareCached(m_textureStatement, (sql + ") ORDER BY image_id").c_str()))
        {
            m_error = sqlite3_errmsg(m_db);
            return false;
        }
    }
    auto *statement = m_textureStatement;
    for (size_t first = 0; first < ids.size(); first += chunk)
    {
        const auto count = std::min(chunk, ids.size() - first);
        sqlite3_clear_bindings(statement);
        for (size_t i = 0; i < count; ++i)
        {
            if (sqlite3_bind_int(statement, static_cast<int>(i + 1), ids[first + i]) != SQLITE_OK)
            {
                sqlite3_reset(statement);
                return false;
            }
        }
        int rc;
        while ((rc = sqlite3_step(statement)) == SQLITE_ROW)
        {
            const int id = sqlite3_column_int(statement, 0);
            if (!decodeTextures(statement, 1, out[id]))
            {
                out.erase(id);
                sqlite3_reset(statement);
                m_error = "invalid Tiny/LBP for image " + std::to_string(id);
                return false;
            }
        }
        sqlite3_reset(statement);
        if (rc != SQLITE_DONE)
        {
            m_error = sqlite3_errmsg(m_db);
            return false;
        }
        for (size_t i = 0; i < count; ++i)
        {
            if (!out.count(ids[first + i]))
            {
                m_error = "missing feature row for image " + std::to_string(ids[first + i]);
                return false;
            }
        }
    }
    return true;
}

bool Database::checkIntegrity()
{
    sqlite3_stmt *statement = nullptr;
    const bool quick = sqlite3_prepare_v2(m_db, "PRAGMA quick_check(1)", -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_text(statement, 0) &&
        std::string(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0))) == "ok";
    sqlite3_finalize(statement);
    statement = nullptr;
    const bool references = sqlite3_prepare_v2(m_db, "PRAGMA foreign_key_check", -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return quick && references;
}

std::vector<int> Database::invalidFeatureIds()
{
    std::vector<int> invalid;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT i.id,i.feature_version,i.avg_l,i.avg_a,i.avg_b,i.edge_density,"
        "f.grid_lab,f.tiny,f.lbp FROM images i LEFT JOIN image_features f ON f.image_id=i.id ORDER BY i.id",
        -1, &statement, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error(sqlite3_errmsg(m_db));
    }
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(statement, sqlite3_finalize);
    std::array<float, 192> grid{};
    TextureFeatures texture;
    int rc;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW)
    {
        bool valid = sqlite3_column_int(statement, 1) == kFeatureVersion;
        for (int column = 2; column <= 5; ++column)
        {
            const int type = sqlite3_column_type(statement, column);
            valid = valid && (type == SQLITE_INTEGER || type == SQLITE_FLOAT) &&
                std::isfinite(sqlite3_column_double(statement, column));
        }
        if (!valid || !decodeFeatureBlob(statement, 6, grid.data(), grid.size()) || !decodeTextures(statement, 7, texture))
        {
            invalid.push_back(sqlite3_column_int(statement, 0));
        }
    }
    if (rc != SQLITE_DONE)
    {
        throw std::runtime_error(sqlite3_errmsg(m_db));
    }
    return invalid;
}

std::unordered_map<int, ImageColorOrder> Database::imageColorOrders()
{
    std::unordered_map<int, ImageColorOrder> colors;
    sqlite3_stmt* statement = nullptr;
    if (!m_db || sqlite3_prepare_v2(m_db,
        "SELECT image_id,file_hash,version,sort_key,source_l,source_a,source_b FROM image_color_order",
        -1, &statement, nullptr) != SQLITE_OK)
    {
        return colors;
    }
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        ImageColorOrder color
        {
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            sqlite3_column_int(statement, 2), sqlite3_column_int64(statement, 3)
        };
        auto sourceMean = [&](int column)
        {
            return sqlite3_column_type(statement, column) == SQLITE_NULL
                ? std::numeric_limits<double>::quiet_NaN() : sqlite3_column_double(statement, column);
        };
        color.sourceL = sourceMean(4);
        color.sourceA = sourceMean(5);
        color.sourceB = sourceMean(6);
        colors.emplace(sqlite3_column_int(statement, 0), std::move(color));
    }
    sqlite3_finalize(statement);
    return colors;
}

sqlite3_stmt *Database::prepareCached(sqlite3_stmt *&statement, const char *sql)
{
    if (!m_db || (!statement && sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr) != SQLITE_OK))
    {
        return nullptr;
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return statement;
}

bool Database::saveImageColorOrder(int id, const ImageColorOrder& color)
{
    auto *statement =
        prepareCached(m_colorStatement,
                      "INSERT INTO image_color_order(image_id,file_hash,version,sort_key,source_l,source_a,source_b) "
                      "VALUES(?,?,?,?,?,?,?) "
                      "ON CONFLICT(image_id) DO UPDATE SET file_hash=excluded.file_hash,"
                      "version=excluded.version,sort_key=excluded.sort_key,source_l=excluded.source_l,"
                      "source_a=excluded.source_a,source_b=excluded.source_b");
    if (!statement)
    {
        return false;
    }
    sqlite3_bind_int(statement, 1, id);
    sqlite3_bind_text(statement, 2, color.fileHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, color.version);
    sqlite3_bind_int64(statement, 4, color.key);
    sqlite3_bind_double(statement, 5, color.sourceL);
    sqlite3_bind_double(statement, 6, color.sourceA);
    sqlite3_bind_double(statement, 7, color.sourceB);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_reset(statement);
    return ok;
}

bool Database::updateImagePath(int id, const std::string& oldPath, const std::string& newPath)
{
    std::string oldValue = oldPath, newValue = newPath;
    try
    {
        if (m_db)
        {
            const LibraryPaths paths(m_path);
            oldValue = paths.store(oldPath);
            newValue = paths.store(newPath);
        }
    }
    catch (const std::exception &error)
    {
        m_error = error.what();
        return false;
    }
    auto *statement = prepareCached(m_pathStatement, "UPDATE images SET file_path=? WHERE id=? AND file_path=?");
    if (!statement)
    {
        return false;
    }
    sqlite3_bind_text(statement, 1, newValue.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, id);
    sqlite3_bind_text(statement, 3, oldValue.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(m_db) == 1;
    sqlite3_reset(statement);
    return ok;
}

bool Database::removeImage(int id)
{
    return removeImages({id});
}

bool Database::removeImages(const std::vector<int> &ids)
{
    if (!m_db || !beginImmediateTransaction())
    {
        return false;
    }
    sqlite3_stmt *statement = nullptr;
    bool ok = sqlite3_prepare_v2(m_db, "DELETE FROM images WHERE id=?", -1, &statement, nullptr) == SQLITE_OK;
    bool removed = false;
    for (int id : ids)
    {
        if (!ok || sqlite3_bind_int(statement, 1, id) != SQLITE_OK || sqlite3_step(statement) != SQLITE_DONE)
        {
            ok = false;
            break;
        }
        removed = removed || sqlite3_changes(m_db) != 0;
        ok = sqlite3_reset(statement) == SQLITE_OK;
    }
    sqlite3_finalize(statement);
    if (ok && (!removed || advanceRevisions(true)) && exec("COMMIT"))
    {
        return true;
    }
    exec("ROLLBACK");
    return false;
}

// ============================================================
// 查询
// ============================================================

bool Database::existsByHash(const std::string& hash)
{
    if (!m_db)
    {
        return false;
    }

    const char* sql = "SELECT COUNT(*) FROM images WHERE file_hash = ?";
    auto *stmt = prepareCached(m_hashStatement, sql);
    if (!stmt)
    {
        return false;
    }
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);

    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_reset(stmt);
    return exists;
}

bool Database::readMatchingRows(std::vector<ImageRecord> &out, bool includeUseCounts)
{
    sqlite3_stmt *count = nullptr;
    const bool counted = sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM images", -1, &count, nullptr) == SQLITE_OK &&
                         sqlite3_step(count) == SQLITE_ROW;
    const auto size = counted ? sqlite3_column_int64(count, 0) : -1;
    sqlite3_finalize(count);
    if (size < 0 || size >= std::numeric_limits<int>::max())
    {
        m_error = "cannot count library image rows";
        return false;
    }
    bool valid = false;
    // 计数覆盖被 BETWEEN 排除的 NULL、无穷或非法亮度。
    auto rows = readRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max(),
                          static_cast<int>(size) + 1, &valid, includeUseCounts);
    if (!valid || rows.size() != static_cast<size_t>(size))
    {
        return false;
    }
    out = std::move(rows);
    return true;
}

std::vector<ImageRecord> Database::queryByLRange(double minL, double maxL, int limit)
{
    return readRange(minL, maxL, limit, nullptr, true);
}

std::vector<ImageRecord> Database::readRange(double minL, double maxL, int limit, bool *validated,
                                             bool includeUseCounts, const RecordReadProgress &progress, bool withGrid)
{
    std::vector<ImageRecord> results;
    if (!m_db)
    {
        return results;
    }
    std::string sql = "SELECT i.id,i.file_path,i.file_hash,i.avg_l,i.avg_a,i.avg_b,i.edge_density,i.feature_version";
    sql += includeUseCounts ? ",i.use_count" : ",0";
    sql += !validated ? ",i.format,i.src_width,i.src_height,i.aspect_ratio,i.file_size,i.mean_brightness,"
                        "i.std_brightness,i.contrast,i.saturation,i.color_variance" : "";
    sql += withGrid ? ",f.grid_lab FROM images i LEFT JOIN image_features f ON f.image_id=i.id" : " FROM images i";
    sql += withGrid ? " WHERE i.avg_l BETWEEN ? AND ?" : "";
    sql += includeUseCounts ? " ORDER BY i.use_count,i.id LIMIT ?" : " ORDER BY i.id LIMIT ?";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        return {};
    }
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    if (withGrid)
    {
        sqlite3_bind_double(stmt, 1, minL);
        sqlite3_bind_double(stmt, 2, maxL);
    }
    sqlite3_bind_int(stmt, withGrid ? 3 : 1, limit);
    const LibraryPaths paths(m_path);
    if (validated)
    {
        results.reserve(static_cast<size_t>(limit - 1));
    }
    const auto progressBatch = static_cast<size_t>(std::max(1, limit / 100));
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        ImageRecord rec;
        const auto text = [&](int column)
        {
            const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, column));
            return value ? value : "";
        };
        rec.id = sqlite3_column_int(stmt, 0);
        rec.filePath = text(1);
        if (rec.filePath.empty() || !LibraryPaths::relative(rec.filePath))
        {
            m_error = "library_incompatible: invalid stored path for image " + std::to_string(rec.id);
            return {};
        }
        rec.filePath = paths.resolve(rec.filePath);
        rec.fileHash = text(2);
        const auto number = [&](int column)
        {
            const auto type = sqlite3_column_type(stmt, column);
            return type == SQLITE_INTEGER || type == SQLITE_FLOAT ? sqlite3_column_double(stmt, column)
                : std::numeric_limits<double>::quiet_NaN();
        };
        rec.avgL = number(3);
        rec.avgA = number(4);
        rec.avgB = number(5);
        rec.edgeDensity = number(6);
        rec.featureVersion = sqlite3_column_int(stmt, 7);
        rec.useCount = sqlite3_column_int(stmt, 8);
        if (!validated)
        {
            rec.format = text(9);
            rec.srcWidth = sqlite3_column_int(stmt, 10);
            rec.srcHeight = sqlite3_column_int(stmt, 11);
            rec.aspectRatio = sqlite3_column_double(stmt, 12);
            rec.fileSize = sqlite3_column_int64(stmt, 13);
            rec.meanBrightness = sqlite3_column_double(stmt, 14);
            rec.stdBrightness = sqlite3_column_double(stmt, 15);
            rec.contrast = sqlite3_column_double(stmt, 16);
            rec.saturation = sqlite3_column_double(stmt, 17);
            rec.colorVariance = sqlite3_column_double(stmt, 18);
        }
        if (withGrid)
        {
            const int column = sqlite3_column_count(stmt) - 1;
            rec.gridLab.resize(192);
            bool scalars = rec.featureVersion == kFeatureVersion;
            for (int i = 3; i <= 6; ++i)
            {
                const int type = sqlite3_column_type(stmt, i);
                scalars = scalars && (type == SQLITE_FLOAT || type == SQLITE_INTEGER) && std::isfinite(sqlite3_column_double(stmt, i));
            }
            if (!scalars || !decodeFeatureBlob(stmt, column, rec.gridLab.data(), rec.gridLab.size()))
            {
                m_error = "library_incompatible: invalid Grid or scalar features for image " + std::to_string(rec.id);
                return {};
            }
        }
        results.emplace_back(std::move(rec));
        if (progress && results.size() % progressBatch == 0)
        {
            progress(results.size());
        }
    }
    if (progress && rc == SQLITE_DONE && results.size() % progressBatch != 0)
    {
        progress(results.size());
    }
    if (validated)
    {
        *validated = rc == SQLITE_DONE;
    }
    return rc == SQLITE_DONE ? std::move(results) : std::vector<ImageRecord>{};
}

std::vector<int> Database::queryIdsByLRange(double minL, double maxL, int limit,
                                              bool sortByUse)
{
    std::vector<int> results;
    if (!m_db)
    {
        return results;
    }

    // 只取 id 列，跳过 BLOB/长文本，避免不必要的数据传输
    // GPU 路径无需排序（GPU 端会重新评分），去掉 ORDER BY 可节省 ~40% 查询时间
    const char* sql = sortByUse
        ? R"SQL(
            SELECT id FROM images WHERE avg_l BETWEEN ? AND ?
            ORDER BY use_count ASC LIMIT ?
        )SQL"
        : R"SQL(
            SELECT id FROM images WHERE avg_l BETWEEN ? AND ? LIMIT ?
        )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;
    sqlite3_bind_double(stmt, 1, minL);
    sqlite3_bind_double(stmt, 2, maxL);
    sqlite3_bind_int(stmt, 3, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        results.push_back(sqlite3_column_int(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return results;
}

int Database::totalCount()
{
    if (!m_db)
    {
        return 0;
    }

    const char* sql = "SELECT COUNT(*) FROM images";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

// ============================================================
// 迁移
// ============================================================

// ============================================================
// 工具
// ============================================================

bool Database::exec(const std::string& sql)
{
    if (!m_db) return false;
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// ============================================================
// 使用统计
// ============================================================

std::vector<std::array<double, 3>> Database::averageColors()
{
    sqlite3_stmt *statement = nullptr;
    if (!m_db || sqlite3_prepare_v2(m_db, "SELECT avg_l, avg_a, avg_b FROM images", -1, &statement, nullptr) != SQLITE_OK)
    {
        return {};
    }
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> cleanup(statement, sqlite3_finalize);
    std::vector<std::array<double, 3>> result;
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        result.push_back({sqlite3_column_double(statement, 0), sqlite3_column_double(statement, 1),
                          sqlite3_column_double(statement, 2)});
    }
    return result;
}

bool Database::recordRunUsage(const std::unordered_map<int, int> &imageUseCount,
                              const std::string &targetHash, const std::string &targetPath)
{
    if (imageUseCount.empty())
    {
        return true;
    }
    if (!m_db || targetHash.empty() || !beginImmediateTransaction())
    {
        return false;
    }

    const bool wrote = [&]()
    {
        const auto prepare = [&](const char *sql)
        {
            sqlite3_stmt *statement = nullptr;
            sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr);
            return std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(statement, sqlite3_finalize);
        };
        const auto target = prepare("INSERT INTO target_runs(target_hash,first_path,run_count,last_used) "
                                    "VALUES(?1,?2,1,datetime('now')) ON CONFLICT(target_hash) DO UPDATE SET "
                                    "run_count=run_count+1,last_used=datetime('now')");
        const auto pair = prepare("INSERT INTO image_target_runs(image_id,target_hash) VALUES(?1,?2) "
                                  "ON CONFLICT(image_id,target_hash) DO NOTHING");
        const auto usage = prepare("INSERT INTO usage_stats(image_id,total_tiles,last_used) "
                                   "VALUES(?1,?2,datetime('now')) ON CONFLICT(image_id) DO UPDATE SET "
                                   "total_tiles=total_tiles+?2,last_used=datetime('now')");
        const auto bindHash = [&](sqlite3_stmt *stmt, int index)
        {
            return sqlite3_bind_text(stmt, index, targetHash.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
        };
        if (!target || !pair || !usage || !bindHash(target.get(), 1) || !bindHash(pair.get(), 2) ||
            sqlite3_bind_text(target.get(), 2, targetPath.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(target.get()) != SQLITE_DONE)
        {
            return false;
        }
        for (const auto &[id, tiles] : imageUseCount)
        {
            if (sqlite3_bind_int(pair.get(), 1, id) != SQLITE_OK || sqlite3_step(pair.get()) != SQLITE_DONE ||
                sqlite3_reset(pair.get()) != SQLITE_OK || sqlite3_bind_int(usage.get(), 1, id) != SQLITE_OK ||
                sqlite3_bind_int(usage.get(), 2, tiles) != SQLITE_OK || sqlite3_step(usage.get()) != SQLITE_DONE ||
                sqlite3_reset(usage.get()) != SQLITE_OK)
            {
                return false;
            }
        }
        return true;
    }();
    if (wrote && commitTransaction())
    {
        return true;
    }
    rollbackTransaction();
    return false;
}

std::vector<std::tuple<int, int, int>> Database::topUsedImages(int limit, bool sortByTiles)
{
    std::vector<std::tuple<int, int, int>> result;
    if (!m_db || !beginTransaction())
    {
        return result;
    }
    const std::string sql = std::string("SELECT usage_stats.image_id, (SELECT count(*) FROM image_target_runs "
                            "WHERE image_id=usage_stats.image_id) AS tracked_runs,total_tiles "
                            "FROM usage_stats JOIN images ON images.id=usage_stats.image_id ORDER BY " ) +
                            (sortByTiles ? "total_tiles DESC, tracked_runs DESC" : "tracked_runs DESC, total_tiles DESC") +
                            ", usage_stats.image_id ASC" + (limit > 0 ? " LIMIT " + std::to_string(limit) : "");
    const auto append = [](void *data, int, char **values, char **)
    {
        auto &rows = *static_cast<std::vector<std::tuple<int, int, int>> *>(data);
        rows.emplace_back(std::atoi(values[0]), std::atoi(values[1]), values[2] ? std::atoi(values[2]) : 0);
        return 0;
    };
    if (sqlite3_exec(m_db, sql.c_str(), append, &result, nullptr) != SQLITE_OK || !commitTransaction())
    {
        rollbackTransaction();
        result.clear();
    }
    return result;
}

} // namespace mosaicraft
