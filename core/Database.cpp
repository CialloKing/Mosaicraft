#include "Database.h"

#include <sqlite3.h>
#include "LibraryAccess.h"
#include "LibraryPaths.h"
#include "UnicodeIO.h"
#include "Version.h"
#include "FeatureIO.h"

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
                   double *readMs) : m_db(nullptr)
{
    std::error_code error;
    m_path = pathToUtf8(std::filesystem::weakly_canonical(u8path(dbPath), error));
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
        else if (mode != OpenMode::Recover &&
                 std::filesystem::exists(libraryJournalStatus(libraryJournalPath(m_path))))
        {
            m_error = m_new ? "database missing but a color reorder journal remains: " + dbPath :
                "unfinished color reorder; retain the journal and staging directory, then run "
                "library add <input-directory> -l <library>: " + dbPath;
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
    bool valid = openConnection(m_new, !m_new);
    if (valid && !m_new)
    {
        // 在只读连接的同一快照内校验并取走匹配行；损坏库仍在打开可写连接前拒绝。
        valid = (!matchingRows || beginTransaction()) && validateSchema(matchingRows, readMs) &&
                (!matchingRows || commitTransaction());
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

            -- 区域结构特征：8×8 LAB Grid = 192 float BLOB
            grid_lab         BLOB NOT NULL CHECK(length(grid_lab)=768),

            -- TinyImage 16×16 灰度图文件路径
            tiny_path       TEXT,

            -- 边缘特征
            edge_density    REAL DEFAULT 0.0,

            -- 纹理特征文件路径
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

bool Database::insertImage(const ImageRecord& rec)
{
    if (!m_db)
    {
        return false;
    }
    if (rec.featureVersion != kFeatureVersion || rec.gridLab.size() != 192 ||
        !std::all_of(rec.gridLab.begin(), rec.gridLab.end(), [](float value)
        {
            return std::isfinite(value);
        }) || !std::isfinite(rec.avgL) || !std::isfinite(rec.avgA) || !std::isfinite(rec.avgB))
    {
        m_error = "library_incompatible: invalid feature record";
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
        // 192 个 float → 768 字节
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
    return removeImages({id});
}

bool Database::removeImages(const std::vector<int> &ids)
{
    if (!m_db || !beginImmediateTransaction())
    {
        return false;
    }
    bool ok = true;
    // 记录及关联统计先在同一事务中提交，提交前不得删除磁盘特征。
    for (int id : ids)
    {
        ok = ok && exec("DELETE FROM usage_stats WHERE image_id=" + std::to_string(id)) &&
             exec("DELETE FROM images WHERE id=" + std::to_string(id));
    }
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

bool Database::readMatchingRows(std::vector<ImageRecord> &out)
{
    sqlite3_stmt *count = nullptr;
    const bool counted = sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM images", -1, &count, nullptr) == SQLITE_OK &&
                         sqlite3_step(count) == SQLITE_ROW;
    const auto size = counted ? sqlite3_column_int64(count, 0) : -1;
    sqlite3_finalize(count);
    if (size < 0 || size >= std::numeric_limits<int>::max())
    {
        return false;
    }
    bool valid = false;
    // 保持既有查询及平分行序；计数覆盖被 BETWEEN 排除的 NULL、无穷或非法亮度。
    auto rows = readRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max(),
                          static_cast<int>(size) + 1, &valid);
    if (!valid || rows.size() != static_cast<size_t>(size))
    {
        return false;
    }
    out = std::move(rows);
    return true;
}

std::vector<ImageRecord> Database::queryByLRange(double minL, double maxL, int limit)
{
    return readRange(minL, maxL, limit, nullptr);
}

std::vector<ImageRecord> Database::readRange(double minL, double maxL, int limit, bool *validated,
                                             const RecordReadProgress &progress)
{
    std::vector<ImageRecord> results;
    if (!m_db)
    {
        return results;
    }

    // 完整读接口仍返回归一化/分析元数据；匹配路径不从 SQLite 复制不用的列。
    static const std::string matchingColumns = "SELECT id,file_path,file_hash,avg_l,avg_a,avg_b,grid_lab,"
        "tiny_path,edge_density,lbp_path,use_count,feature_version";
    static const std::string range = " FROM images WHERE avg_l BETWEEN ? AND ? ORDER BY use_count ASC LIMIT ?";
    static const std::string matchSql = matchingColumns + range;
    static const std::string fullSql = matchingColumns + ",format,src_width,src_height,aspect_ratio,file_size,"
        "mean_brightness,std_brightness,contrast,saturation,color_variance" + range;
    const char *sql = (validated ? matchSql : fullSql).c_str();

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
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    if (validated)
    {
        results.reserve(static_cast<size_t>(limit - 1));
    }
    // 每批约占全量记录的百分之一，限制通知次数并保留快速读取时的中间进度。
    const auto progressBatch = static_cast<std::size_t>(std::max(1, limit / 100));
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        if (validated && (sqlite3_column_type(stmt, 6) != SQLITE_BLOB || sqlite3_column_bytes(stmt, 6) != 768 ||
                          sqlite3_column_type(stmt, 11) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 11) != kFeatureVersion))
        {
            return {};
        }
        ImageRecord rec;
        int col = 0;

        rec.id           = sqlite3_column_int(stmt, col++);
        rec.filePath     = sqlite3_column_type(stmt, col) != SQLITE_NULL
                           ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
        col++;
        rec.fileHash     = sqlite3_column_type(stmt, col) != SQLITE_NULL
                           ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
        col++;
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

        if (!validated)
        {
            rec.format       = sqlite3_column_type(stmt, col) != SQLITE_NULL
                               ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)) : "";
            col++;
            rec.srcWidth     = sqlite3_column_int(stmt, col++);
            rec.srcHeight    = sqlite3_column_int(stmt, col++);
            rec.aspectRatio  = sqlite3_column_double(stmt, col++);
            rec.fileSize     = sqlite3_column_int64(stmt, col++);

            rec.meanBrightness = sqlite3_column_double(stmt, col++);
            rec.stdBrightness  = sqlite3_column_double(stmt, col++);
            rec.contrast       = sqlite3_column_double(stmt, col++);

            rec.saturation     = sqlite3_column_double(stmt, col++);
            rec.colorVariance  = sqlite3_column_double(stmt, col++);
        }
        if (validated && (rec.filePath.empty() || !LibraryPaths::relative(rec.filePath) ||
                          !LibraryPaths::relative(rec.tinyPath) || !LibraryPaths::relative(rec.lbpPath) ||
                          !std::isfinite(rec.avgL) || !std::isfinite(rec.avgA) || !std::isfinite(rec.avgB) ||
                          !std::all_of(rec.gridLab.begin(), rec.gridLab.end(), [](float value)
                          {
                              return std::isfinite(value);
                          })))
        {
            return {};
        }
        if (m_db)
        {
            rec.filePath = paths.resolve(rec.filePath);
            rec.tinyPath = paths.resolve(rec.tinyPath);
            rec.lbpPath = paths.resolve(rec.lbpPath);
        }
        if (MOSAICRAFT_TRIAL_LIBRARY_PATHS)
        {
            // 行已完整解码；移交路径与 Grid 存储，不再复制同一条记录。
            results.emplace_back(std::move(rec));
        }
        else
        {
            results.push_back(rec);
        }
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

    // 旧库没有素材与目标的对应关系。仅增加新关系，保留旧计数和已累计的格数。
    const bool wrote = [&]()
    {
        if (!exec("CREATE TABLE IF NOT EXISTS image_target_runs ("
                  "image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE, "
                  "target_hash TEXT NOT NULL REFERENCES target_runs(target_hash), "
                  "PRIMARY KEY(image_id, target_hash)) WITHOUT ROWID"))
        {
            return false;
        }
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
    bool tracksTargets = false;
    const auto markFound = [](void *data, int, char **, char **)
    {
        *static_cast<bool *>(data) = true;
        return 0;
    };
    // 读旧库不能隐式建表；探测与查询共享快照，避免并发首次写入时读到混合口径。
    const auto found = sqlite3_exec(m_db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='image_target_runs'",
                                    markFound, &tracksTargets, nullptr);
    const auto runs = tracksTargets ? "(SELECT count(*) FROM image_target_runs WHERE image_id=usage_stats.image_id)"
                                    : "0";
    const std::string sql = std::string("SELECT usage_stats.image_id, ") + runs + " AS tracked_runs,total_tiles "
                            "FROM usage_stats JOIN images ON images.id=usage_stats.image_id ORDER BY " +
                            (sortByTiles ? "total_tiles DESC, tracked_runs DESC" : "tracked_runs DESC, total_tiles DESC") +
                            ", usage_stats.image_id ASC" + (limit > 0 ? " LIMIT " + std::to_string(limit) : "");
    const auto append = [](void *data, int, char **values, char **)
    {
        auto &rows = *static_cast<std::vector<std::tuple<int, int, int>> *>(data);
        rows.emplace_back(std::atoi(values[0]), std::atoi(values[1]), values[2] ? std::atoi(values[2]) : 0);
        return 0;
    };
    if (found != SQLITE_OK || sqlite3_exec(m_db, sql.c_str(), append, &result, nullptr) != SQLITE_OK || !commitTransaction())
    {
        rollbackTransaction();
        result.clear();
    }
    return result;
}

} // namespace mosaicraft
