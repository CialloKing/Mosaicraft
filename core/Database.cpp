#include "Database.h"

#include <sqlite3.h>
#include "LibraryAccess.h"
#include "LibraryPaths.h"
#include "UnicodeIO.h"
#include "Version.h"
#include "FeatureIO.h"

#include <cstring>
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

Database::Database(const std::string &dbPath, OpenMode mode) : m_db(nullptr)
{
    std::error_code error;
    m_path = pathToUtf8(std::filesystem::weakly_canonical(u8path(dbPath), error));
    if (error)
    {
        m_error = "cannot resolve database: " + dbPath;
        return;
    }
    m_new = !std::filesystem::exists(u8path(m_path));
    m_exclusive = mode == OpenMode::Build;
    if ((m_new && mode == OpenMode::Existing) ||
        (m_new && std::filesystem::exists(libraryJournalPath(m_path))))
    {
        m_error = "database missing or has an orphaned reorder journal: " + dbPath;
        return;
    }
    m_access = std::make_unique<LibraryAccess>(m_path, m_exclusive || m_new, 5000);
    if (!m_access->acquired())
    {
        m_error = "library is busy or its operation lock cannot be opened: " + dbPath;
        m_access.reset();
        return;
    }
    // 先只读校验；旧库失败时不得执行 WAL 设置、DDL 或任何升级。
    bool valid = openConnection(m_new, !m_new);
    if (valid && !m_new)
    {
        valid = validateSchema();
        if (valid && !m_exclusive && std::filesystem::exists(libraryJournalPath(m_path)))
        {
            m_error = "unfinished color reorder; run build to recover: " + dbPath;
            valid = false;
        }
        if (valid)
        {
            closeConnection();
            valid = openConnection(false);
        }
    }
    if (valid)
    {
        valid = exec("PRAGMA foreign_keys=ON");
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
    if (!exec("BEGIN IMMEDIATE"))
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

            -- 全局颜色特征（V1）
            avg_l           REAL DEFAULT 0.0,
            avg_a           REAL DEFAULT 0.0,
            avg_b           REAL DEFAULT 0.0,

            -- 亮度统计（V3）
            mean_brightness REAL DEFAULT 0.0,
            std_brightness  REAL DEFAULT 0.0,
            contrast        REAL DEFAULT 0.0,

            -- 颜色统计（V3）
            saturation      REAL DEFAULT 0.0,
            color_variance  REAL DEFAULT 0.0,

            -- 区域结构特征（V2）：8×8 LAB Grid = 192 float BLOB
            grid_lab         BLOB NOT NULL CHECK(length(grid_lab)=768),

            -- TinyImage 16×16 灰度图文件路径（V3）
            tiny_path       TEXT,

            -- 边缘特征（V4）
            edge_density    REAL DEFAULT 0.0,

            -- 纹理特征文件路径（V4）
            lbp_path       TEXT,

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

bool Database::insertImage(const ImageRecord& rec)
{
    if (!m_db)
    {
        return false;
    }

    std::string file = rec.filePath, tiny = rec.tinyPath, hist = rec.lbpPath;
    try
    {
        if (m_db)
        {
            const LibraryPaths paths(m_path);
            file = paths.store(file);
            tiny = paths.store(tiny);
            hist = paths.store(hist);
        }
    }
    catch (const std::exception &error)
    {
        m_error = error.what();
        return false;
    }

    const char* sql = R"SQL(
        INSERT INTO images
            (file_path, file_hash, format, src_width, src_height, aspect_ratio, file_size,
             avg_l, avg_a, avg_b,
             mean_brightness, std_brightness, contrast,
             saturation, color_variance,
             grid_lab,
             tiny_path,
             edge_density,
             lbp_path,
             feature_version)
        VALUES
            (?, ?, ?, ?, ?, ?, ?,
             ?, ?, ?,
             ?, ?, ?,
             ?, ?,
             ?,
             ?,
             ?,
             ?,
             ?)
        ON CONFLICT(file_path) DO UPDATE SET
            file_hash=excluded.file_hash, avg_l=excluded.avg_l, avg_a=excluded.avg_a, avg_b=excluded.avg_b,
            mean_brightness=excluded.mean_brightness, std_brightness=excluded.std_brightness,
            contrast=excluded.contrast, saturation=excluded.saturation, color_variance=excluded.color_variance,
            grid_lab=excluded.grid_lab, tiny_path=excluded.tiny_path, edge_density=excluded.edge_density,
            lbp_path=excluded.lbp_path, feature_version=excluded.feature_version
    )SQL";

    auto *stmt = prepareCached(m_insertStatement, sql);
    if (!stmt)
    {
        std::cerr << "Prepare insert error: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    // 参数索引从 1 开始
    int idx = 1;

    // 元数据
    sqlite3_bind_text(stmt, idx++, file.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, rec.fileHash.c_str(), -1, SQLITE_TRANSIENT);
    // format
    if (rec.format.empty()) { sqlite3_bind_null(stmt, idx++); }
    else                     { sqlite3_bind_text(stmt, idx++, rec.format.c_str(), -1, SQLITE_TRANSIENT); }
    sqlite3_bind_int(stmt, idx++, rec.srcWidth);
    sqlite3_bind_int(stmt, idx++, rec.srcHeight);
    sqlite3_bind_double(stmt, idx++, rec.aspectRatio);
    sqlite3_bind_int64(stmt, idx++, rec.fileSize);

    // 全局颜色
    sqlite3_bind_double(stmt, idx++, rec.avgL);
    sqlite3_bind_double(stmt, idx++, rec.avgA);
    sqlite3_bind_double(stmt, idx++, rec.avgB);

    // 亮度统计
    sqlite3_bind_double(stmt, idx++, rec.meanBrightness);
    sqlite3_bind_double(stmt, idx++, rec.stdBrightness);
    sqlite3_bind_double(stmt, idx++, rec.contrast);

    // 颜色统计
    sqlite3_bind_double(stmt, idx++, rec.saturation);
    sqlite3_bind_double(stmt, idx++, rec.colorVariance);

    // 区域结构：grid_lab 序列化为 BLOB
    if (rec.gridLab.empty())
    {
        sqlite3_bind_null(stmt, idx++);
    }
    else
    {
        // 48 个 float → 192 字节
        std::size_t blobSize = rec.gridLab.size() * sizeof(float);
        sqlite3_bind_blob(stmt, idx++, rec.gridLab.data(),
                          static_cast<int>(blobSize), SQLITE_TRANSIENT);
    }

    // tiny_path
    if (rec.tinyPath.empty()) { sqlite3_bind_null(stmt, idx++); }
    else                       { sqlite3_bind_text(stmt, idx++, tiny.c_str(), -1, SQLITE_TRANSIENT); }

    // 边缘
    sqlite3_bind_double(stmt, idx++, rec.edgeDensity);

    // lbp_path
    if (rec.lbpPath.empty()) { sqlite3_bind_null(stmt, idx++); }
    else                       { sqlite3_bind_text(stmt, idx++, hist.c_str(), -1, SQLITE_TRANSIENT); }

    // feature_version
    sqlite3_bind_int(stmt, idx++, rec.featureVersion);

    const int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    if (rc != SQLITE_DONE)
    {
        std::cerr << "Insert error: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    return true;
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
    if (!m_db || !initUsageStats() || !exec("BEGIN IMMEDIATE"))
    {
        return false;
    }
    // 旧 usage_stats 表未必有级联外键；在同一个事务里显式删除统计。
    bool ok = exec("DELETE FROM usage_stats WHERE image_id=" + std::to_string(id));
    const char* sql = "DELETE FROM images WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(stmt, 1, id);
        ok = sqlite3_step(stmt) == SQLITE_DONE && ok;
    }
    else
    {
        ok = false;
    }
    sqlite3_finalize(stmt);
    if (ok && exec("COMMIT"))
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

std::vector<ImageRecord> Database::queryByLRange(double minL, double maxL, int limit)
{
    std::vector<ImageRecord> results;
    if (!m_db)
    {
        return results;
    }

    const char* sql = R"SQL(
        SELECT id, file_path, file_hash, format, src_width, src_height,
               aspect_ratio, file_size,
               avg_l, avg_a, avg_b,
               mean_brightness, std_brightness, contrast,
               saturation, color_variance,
               grid_lab,
               tiny_path,
               edge_density,
               lbp_path,
               use_count,
               feature_version
        FROM images
        WHERE avg_l BETWEEN ? AND ?
        ORDER BY use_count ASC
        LIMIT ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        return results;
    }
    sqlite3_bind_double(stmt, 1, minL);
    sqlite3_bind_double(stmt, 2, maxL);
    sqlite3_bind_int(stmt, 3, limit);

    // 每次查询只准备一次库根目录，记录读取只拼接资源路径。
    const LibraryPaths paths(m_path);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        ImageRecord rec;
        int col = 0;

        rec.id           = sqlite3_column_int(stmt, col++);
        rec.filePath     = sqlite3_column_type(stmt, col) != SQLITE_NULL
                           ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
        col++;
        rec.fileHash     = sqlite3_column_type(stmt, col) != SQLITE_NULL
                           ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
        col++;
        rec.format       = sqlite3_column_type(stmt, col) != SQLITE_NULL
                           ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
        col++;
        rec.srcWidth     = sqlite3_column_int(stmt, col++);
        rec.srcHeight    = sqlite3_column_int(stmt, col++);
        rec.aspectRatio  = sqlite3_column_double(stmt, col++);
        rec.fileSize     = sqlite3_column_int64(stmt, col++);

        rec.avgL         = sqlite3_column_double(stmt, col++);
        // NULL 不能冒充合法的零色度，排序应在移动文件前报告损坏的均值。
        auto average = [&]()
        {
            const double value = sqlite3_column_type(stmt, col) == SQLITE_NULL
                ? std::numeric_limits<double>::quiet_NaN() : sqlite3_column_double(stmt, col);
            ++col;
            return value;
        };
        rec.avgA = average();
        rec.avgB = average();

        rec.meanBrightness = sqlite3_column_double(stmt, col++);
        rec.stdBrightness  = sqlite3_column_double(stmt, col++);
        rec.contrast       = sqlite3_column_double(stmt, col++);

        rec.saturation     = sqlite3_column_double(stmt, col++);
        rec.colorVariance  = sqlite3_column_double(stmt, col++);

        // grid_lab BLOB → std::vector<float>
        if (sqlite3_column_type(stmt, col) != SQLITE_NULL)
        {
            int blobSize = sqlite3_column_bytes(stmt, col);
            const float* blobData = static_cast<const float*>(
                sqlite3_column_blob(stmt, col));
            int numFloats = blobSize / static_cast<int>(sizeof(float));
            rec.gridLab.resize(numFloats);
            decodeFloats(reinterpret_cast<const uint8_t *>(blobData), rec.gridLab.data(), numFloats);
        }
        col++;

        rec.tinyPath = sqlite3_column_type(stmt, col) != SQLITE_NULL
                       ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
        col++;

        rec.edgeDensity   = sqlite3_column_double(stmt, col++);

        rec.lbpPath = sqlite3_column_type(stmt, col) != SQLITE_NULL
                       ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
        col++;

        rec.useCount       = sqlite3_column_int(stmt, col++);
        rec.featureVersion = sqlite3_column_int(stmt, col++);

        if (m_db)
        {
            rec.filePath = paths.resolve(rec.filePath);
            rec.tinyPath = paths.resolve(rec.tinyPath);
            rec.lbpPath = paths.resolve(rec.lbpPath);
        }
        results.push_back(rec);
    }

    sqlite3_finalize(stmt);
    return results;
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

bool Database::initUsageStats()
{
    // 使用统计表属于完整图库结构，打开时已经验证，读取不能顺便建表。
    return m_db != nullptr;
}

bool Database::recordRunUsage(const std::unordered_map<int, int>& imageUseCount,
                              const std::string& targetHash,
                              const std::string& targetPath)
{
    if (!m_db || !initUsageStats() || !exec("BEGIN IMMEDIATE"))
    {
        return false;
    }

    // 检查目标图内容哈希是否已存在（改名去重）
    bool isNewTarget = true;
    if (!targetHash.empty() && m_db) {
        const char* checkSql = "SELECT 1 FROM target_runs WHERE target_hash = ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, targetHash.c_str(), -1, SQLITE_TRANSIENT);
            isNewTarget = (sqlite3_step(stmt) != SQLITE_ROW);
            sqlite3_finalize(stmt);
        }
    }
    // 更新图片使用统计
    bool ok = true;
    if (!targetHash.empty() && m_db) {
        if (isNewTarget) {
            const char* insSql = "INSERT INTO target_runs (target_hash, first_path, run_count, last_used) "
                                 "VALUES (?, ?, 1, datetime('now'))";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(m_db, insSql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, targetHash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, targetPath.c_str(), -1, SQLITE_TRANSIENT);
                ok = (sqlite3_step(stmt) == SQLITE_DONE) && ok;
                sqlite3_finalize(stmt);
            } else {
                ok = false;
            }
        } else {
            const char* updSql = "UPDATE target_runs SET run_count = run_count + 1, "
                                 "last_used = datetime('now') WHERE target_hash = ?";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(m_db, updSql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, targetHash.c_str(), -1, SQLITE_TRANSIENT);
                ok = (sqlite3_step(stmt) == SQLITE_DONE) && ok;
                sqlite3_finalize(stmt);
            } else {
                ok = false;
            }
        }
    }
    for (const auto& [imgId, tileCount] : imageUseCount)
    {
        std::string runsExpr = isNewTarget ? "total_runs + 1" : "total_runs";
        std::string sql = "INSERT INTO usage_stats (image_id, total_runs, total_tiles, last_used) "
                          "VALUES (" + std::to_string(imgId) + ", "
                          + (isNewTarget ? "1" : "0") + ", "
                          + std::to_string(tileCount)
                          + ", datetime('now')) "
                          "ON CONFLICT(image_id) DO UPDATE SET "
                          "total_runs = " + runsExpr + ", "
                          "total_tiles = total_tiles + " + std::to_string(tileCount)
                          + ", last_used = datetime('now')";
        ok = exec(sql) && ok;
    }
    if (!ok)
    {
        exec("ROLLBACK");
        return false;
    }
    if (!exec("COMMIT"))
    {
        exec("ROLLBACK");
        return false;
    }
    return true;
}

std::vector<std::tuple<int, int, int>> Database::topUsedImages(int limit)
{
    std::vector<std::tuple<int, int, int>> result;
    if (!m_db) return result;
    initUsageStats();
    // 使用 exec 回调模式
    std::string sql = "SELECT image_id, total_runs, total_tiles FROM usage_stats "
                      "JOIN images ON images.id=usage_stats.image_id "
                      "ORDER BY total_runs DESC LIMIT " + std::to_string(limit);
    char* errMsg = nullptr;
    auto callback = [](void* data, int argc, char** argv, char**) -> int {
        auto* vec = static_cast<std::vector<std::tuple<int,int,int>>*>(data);
        if (argc >= 3)
            vec->emplace_back(std::atoi(argv[0]), std::atoi(argv[1]), std::atoi(argv[2]));
        return 0;
    };
    sqlite3_exec(m_db, sql.c_str(), callback, &result, &errMsg);
    if (errMsg) { sqlite3_free(errMsg); }
    return result;
}

} // namespace mosaicraft
