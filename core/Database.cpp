#include "Database.h"

#include <sqlite3.h>
#include "LibraryAccess.h"
#include "UnicodeIO.h"

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

Database::Database(const std::string& dbPath, OpenMode mode)
    : m_db(nullptr)
{
    std::error_code pathError;
    m_path = pathToUtf8(std::filesystem::weakly_canonical(u8path(dbPath), pathError));
    if (pathError || (mode == OpenMode::Existing && !std::filesystem::is_regular_file(u8path(m_path), pathError)))
    {
        m_error = "database file does not exist: " + dbPath;
        return;
    }
    m_access = std::make_unique<LibraryAccess>(m_path, mode == OpenMode::Build);
    if (!m_access->acquired())
    {
        m_error = "library is busy or its operation lock cannot be opened: " + dbPath;
        std::cerr << m_error << std::endl;
        m_access.reset();
        return;
    }
    if (mode != OpenMode::Build && std::filesystem::exists(libraryJournalPath(m_path)))
    {
        m_error = "unfinished color reorder; run build to recover: " + dbPath;
        std::cerr << m_error << std::endl;
        m_access.reset();
        return;
    }
    // SQLITE_OPEN_CREATE: 文件不存在时自动创建
    // SQLITE_OPEN_READWRITE: 读写模式
    int rc = sqlite3_open_v2(m_path.c_str(), &m_db,
                              SQLITE_OPEN_READWRITE | (mode != OpenMode::Existing ? SQLITE_OPEN_CREATE : 0),
                              nullptr);
    if (rc != SQLITE_OK)
    {
        m_error = sqlite3_errmsg(m_db);
        std::cerr << "Database open error: " << sqlite3_errmsg(m_db) << std::endl;
        sqlite3_close(m_db);
        m_db = nullptr;
        return;
    }

    // 竞争写者时先等待；只有建库允许创建文件，维护命令必须验证已有索引。
    sqlite3_busy_timeout(m_db, 5000);
    bool valid = exec("PRAGMA foreign_keys=ON") && exec("PRAGMA journal_mode=WAL") &&
                 exec("PRAGMA synchronous=NORMAL");
    if (valid && mode == OpenMode::Existing)
    {
        sqlite3_stmt* statement = nullptr;
        valid = sqlite3_prepare_v2(m_db,
            "SELECT id,file_path,file_hash,format,src_width,src_height,aspect_ratio,file_size,avg_l,avg_a,avg_b,"
            "mean_brightness,std_brightness,contrast,saturation,color_variance,grid4x4,tiny_path,edge_density,"
            "hist_path,feature_version,use_count "
            "FROM images LIMIT 0", -1, &statement, nullptr) == SQLITE_OK;
        if (valid)
        {
            valid = sqlite3_step(statement) == SQLITE_DONE;
        }
        sqlite3_finalize(statement);
        statement = nullptr;
        if (valid)
        {
            // 能打开文件不代表其数据页完整；避免损坏索引仍被读成空库。
            valid = sqlite3_prepare_v2(m_db, "PRAGMA quick_check(1)", -1, &statement, nullptr) == SQLITE_OK &&
                    sqlite3_step(statement) == SQLITE_ROW;
            if (valid)
            {
                const auto* result = sqlite3_column_text(statement, 0);
                valid = result && std::string(reinterpret_cast<const char*>(result)) == "ok";
            }
            sqlite3_finalize(statement);
        }
    }
    if (!valid)
    {
        m_error = "invalid database: " + dbPath;
        std::cerr << "ERROR: Invalid database: " << dbPath << " (" << sqlite3_errmsg(m_db) << ")" << std::endl;
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

Database::~Database()
{
    if (m_db)
    {
        sqlite3_close(m_db);
    }
}

bool Database::isOpen() const
{
    return m_db != nullptr;
}

// ============================================================
// 建表
// ============================================================

bool Database::createTables()
{
    if (!m_db)
    {
        return false;
    }

    // 建表语句：包含所有特征分层字段（V1~V4 预留）
    // IF NOT EXISTS 保证幂等
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS images (
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
            grid4x4         BLOB,

            -- TinyImage 16×16 灰度图文件路径（V3）
            tiny_path       TEXT,

            -- 边缘特征（V4）
            edge_density    REAL DEFAULT 0.0,

            -- 纹理特征文件路径（V4）
            hist_path       TEXT,

            -- 使用统计
            use_count       INTEGER DEFAULT 0,

            -- 版本标记
            feature_version INTEGER DEFAULT 0,

            -- 时间戳
            created_at      TEXT DEFAULT (datetime('now'))
        );

        CREATE INDEX IF NOT EXISTS idx_hash    ON images(file_hash);
        CREATE INDEX IF NOT EXISTS idx_avg_l   ON images(avg_l);
        CREATE INDEX IF NOT EXISTS idx_use     ON images(use_count);

        CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS image_color_order (
            image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,
            file_hash TEXT NOT NULL,
            version INTEGER NOT NULL,
            sort_key INTEGER NOT NULL
        );
    )SQL";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Create tables error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    // 自动迁移：为旧表补上缺失的列
    migrate();

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

    const char* sql = R"SQL(
        INSERT INTO images
            (file_path, file_hash, format, src_width, src_height, aspect_ratio, file_size,
             avg_l, avg_a, avg_b,
             mean_brightness, std_brightness, contrast,
             saturation, color_variance,
             grid4x4,
             tiny_path,
             edge_density,
             hist_path,
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
            grid4x4=excluded.grid4x4, tiny_path=excluded.tiny_path, edge_density=excluded.edge_density,
            hist_path=excluded.hist_path, feature_version=excluded.feature_version
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Prepare insert error: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    // 参数索引从 1 开始
    int idx = 1;

    // 元数据
    sqlite3_bind_text(stmt, idx++, rec.filePath.c_str(), -1, SQLITE_TRANSIENT);
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

    // 区域结构：grid4x4 序列化为 BLOB
    if (rec.grid4x4.empty())
    {
        sqlite3_bind_null(stmt, idx++);
    }
    else
    {
        // 48 个 float → 192 字节
        std::size_t blobSize = rec.grid4x4.size() * sizeof(float);
        sqlite3_bind_blob(stmt, idx++, rec.grid4x4.data(),
                          static_cast<int>(blobSize), SQLITE_TRANSIENT);
    }

    // tiny_path
    if (rec.tinyPath.empty()) { sqlite3_bind_null(stmt, idx++); }
    else                       { sqlite3_bind_text(stmt, idx++, rec.tinyPath.c_str(), -1, SQLITE_TRANSIENT); }

    // 边缘
    sqlite3_bind_double(stmt, idx++, rec.edgeDensity);

    // hist_path
    if (rec.histPath.empty()) { sqlite3_bind_null(stmt, idx++); }
    else                       { sqlite3_bind_text(stmt, idx++, rec.histPath.c_str(), -1, SQLITE_TRANSIENT); }

    // feature_version
    sqlite3_bind_int(stmt, idx++, rec.featureVersion);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

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
        "SELECT image_id,file_hash,version,sort_key FROM image_color_order",
        -1, &statement, nullptr) != SQLITE_OK)
    {
        return colors;
    }
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        colors.emplace(sqlite3_column_int(statement, 0), ImageColorOrder{
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            sqlite3_column_int(statement, 2), sqlite3_column_int64(statement, 3)});
    }
    sqlite3_finalize(statement);
    return colors;
}

bool Database::saveImageColorOrder(int id, const ImageColorOrder& color)
{
    sqlite3_stmt* statement = nullptr;
    if (!m_db || sqlite3_prepare_v2(m_db,
        "INSERT INTO image_color_order(image_id,file_hash,version,sort_key) VALUES(?,?,?,?) "
        "ON CONFLICT(image_id) DO UPDATE SET file_hash=excluded.file_hash,"
        "version=excluded.version,sort_key=excluded.sort_key", -1, &statement, nullptr) != SQLITE_OK)
    {
        return false;
    }
    sqlite3_bind_int(statement, 1, id);
    sqlite3_bind_text(statement, 2, color.fileHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, color.version);
    sqlite3_bind_int64(statement, 4, color.key);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return ok;
}

bool Database::updateImagePath(int id, const std::string& oldPath, const std::string& newPath)
{
    sqlite3_stmt* statement = nullptr;
    if (!m_db || sqlite3_prepare_v2(m_db,
        "UPDATE images SET file_path=? WHERE id=? AND file_path=?",
        -1, &statement, nullptr) != SQLITE_OK)
    {
        return false;
    }
    sqlite3_bind_text(statement, 1, newPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, id);
    sqlite3_bind_text(statement, 3, oldPath.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(m_db) == 1;
    sqlite3_finalize(statement);
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
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);

    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
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
               grid4x4,
               tiny_path,
               edge_density,
               hist_path,
               use_count,
               feature_version
        FROM images
        WHERE avg_l BETWEEN ? AND ?
        ORDER BY use_count ASC
        LIMIT ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;
    sqlite3_bind_double(stmt, 1, minL);
    sqlite3_bind_double(stmt, 2, maxL);
    sqlite3_bind_int(stmt, 3, limit);

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
        rec.avgA         = sqlite3_column_double(stmt, col++);
        rec.avgB         = sqlite3_column_double(stmt, col++);

        rec.meanBrightness = sqlite3_column_double(stmt, col++);
        rec.stdBrightness  = sqlite3_column_double(stmt, col++);
        rec.contrast       = sqlite3_column_double(stmt, col++);

        rec.saturation     = sqlite3_column_double(stmt, col++);
        rec.colorVariance  = sqlite3_column_double(stmt, col++);

        // grid4x4 BLOB → std::vector<float>
        if (sqlite3_column_type(stmt, col) != SQLITE_NULL)
        {
            int blobSize = sqlite3_column_bytes(stmt, col);
            const float* blobData = static_cast<const float*>(
                sqlite3_column_blob(stmt, col));
            int numFloats = blobSize / static_cast<int>(sizeof(float));
            rec.grid4x4.assign(blobData, blobData + numFloats);
        }
        col++;

        rec.tinyPath = sqlite3_column_type(stmt, col) != SQLITE_NULL
                       ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
        col++;

        rec.edgeDensity   = sqlite3_column_double(stmt, col++);

        rec.histPath = sqlite3_column_type(stmt, col) != SQLITE_NULL
                       ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
        col++;

        rec.useCount       = sqlite3_column_int(stmt, col++);
        rec.featureVersion = sqlite3_column_int(stmt, col++);

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

bool Database::columnExists(const std::string& table, const std::string& column)
{
    // PRAGMA table_info 返回每列：cid, name, type, notnull, dflt_value, pk
    std::string sql = "PRAGMA table_info(" + table + ")";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && column == name)
        {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

void Database::migrate()
{
    if (!m_db)
    {
        return;
    }

    // 将缺失列及默认值定义为列表；新列追加到表末尾，SQLite 默认用 NULL 填充旧行
    // 但 DEFAULT 对 ALTER TABLE ADD COLUMN 立即生效（旧行自动设为默认值）
    struct ColumnDef
    {
        const char* name;
        const char* typeAndDefault;  // 例如 "REAL DEFAULT 0.0"
    };

    // 只列出自上一个版本以来可能缺失的列（老表升级用）
    const ColumnDef missing[] = {
        {"format",           "TEXT DEFAULT ''"},
        {"src_width",        "INTEGER DEFAULT 0"},
        {"src_height",       "INTEGER DEFAULT 0"},
        {"aspect_ratio",     "REAL DEFAULT 0.0"},
        {"file_size",        "INTEGER DEFAULT 0"},
        {"mean_brightness",  "REAL DEFAULT 0.0"},
        {"std_brightness",   "REAL DEFAULT 0.0"},
        {"contrast",         "REAL DEFAULT 0.0"},
        {"saturation",       "REAL DEFAULT 0.0"},
        {"color_variance",   "REAL DEFAULT 0.0"},
        {"grid4x4",          "BLOB"},
        {"edge_density",     "REAL DEFAULT 0.0"},
        {"feature_version",  "INTEGER DEFAULT 0"},
    };

    for (const auto& col : missing)
    {
        if (!columnExists("images", col.name))
        {
            std::string alter = "ALTER TABLE images ADD COLUMN "
                                + std::string(col.name) + " "
                                + std::string(col.typeAndDefault);
            exec(alter);
        }
    }
}

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
    bool ok = exec("CREATE TABLE IF NOT EXISTS usage_stats ("
         "image_id INTEGER PRIMARY KEY,"
         "total_runs INTEGER DEFAULT 0,"
         "total_tiles INTEGER DEFAULT 0,"
         "last_used TEXT,"
         "FOREIGN KEY (image_id) REFERENCES images(id) ON DELETE CASCADE)");
    // 目标图哈希记录：防止改名后重复计数
    return exec("CREATE TABLE IF NOT EXISTS target_runs ("
         "target_hash TEXT PRIMARY KEY,"
         "first_path TEXT,"
         "run_count INTEGER DEFAULT 0,"
         "last_used TEXT)") && ok;
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
