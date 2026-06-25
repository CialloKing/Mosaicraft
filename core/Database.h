#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

// 前向声明，避免暴露 sqlite3.h 到头文件
struct sqlite3;

namespace mosaicraft
{

// ImageRecord — 图片在数据库中的一条完整记录。
// 按特征分层设计：元数据 → 全局颜色 → 区域结构 → 纹理 → 语义。
struct ImageRecord
{
    // ——— 元数据 ———
    int id = 0;
    std::string filePath;           // 归一化图路径
    std::string fileHash;           // 文件内容 hash（去重用）
    std::string format;             // 原格式：jpg / png / webp / bmp
    int srcWidth = 0;               // 原始宽度
    int srcHeight = 0;              // 原始高度
    double aspectRatio = 0.0;       // 宽高比（srcWidth / srcHeight）
    int64_t fileSize = 0;           // 归一化后文件大小（字节）

    // ——— 全局颜色特征（V1） ———
    double avgL = 0.0;              // 平均 LAB-L（亮度）
    double avgA = 0.0;              // 平均 LAB-A
    double avgB = 0.0;              // 平均 LAB-B

    // ——— 亮度统计（V3） ———
    double meanBrightness = 0.0;    // 感知亮度均值
    double stdBrightness = 0.0;     // 感知亮度标准差
    double contrast = 0.0;          // RMS 对比度

    // ——— 颜色统计（V3） ———
    double saturation = 0.0;        // 平均饱和度
    double colorVariance = 0.0;     // 颜色方差

    // ——— 区域结构特征（V2） ———
    // 8×8 LAB Grid：64 格 × 3 通道 = 192 个 float（768 字节 BLOB）
    // 为空表示尚未计算
    std::vector<float> grid4x4;

    // ——— TinyImage（V3） ———
    // 16×16 灰度图，存为独立文件
    std::string tinyPath;

    // ——— 边缘特征（V4） ———
    double edgeDensity = 0.0;       // 边缘像素占比（0~1）

    // ——— 纹理特征（V4 预留） ———
    std::string histPath;           // 直方图 / LBP / GLCM 特征文件路径

    // ——— 使用统计 ———
    int useCount = 0;               // 被匹配选用的次数

    // ——— 版本标记 ———
    int featureVersion = 0;         // 特征计算版本号，递增便于增量更新
};

// Database — SQLite 索引的 RAII 封装。
// 只存索引指针和标量特征；大块数据（TinyImage / Histogram）存文件系统。
class Database
{
public:
    // 打开或创建数据库文件
    explicit Database(const std::string& dbPath);
    ~Database();

    // 禁止拷贝（sqlite3* 独占所有权）
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 是否成功打开
    bool isOpen() const;

    // 首次使用时建表（幂等）；若旧表缺少列则自动迁移
    bool createTables();

    // ——— 写操作 ———

    // 插入一条记录；若 file_path 已存在则忽略（UNIQUE 约束）
    bool insertImage(const ImageRecord& rec);

    // 删除单条记录（db-purge 用）
    bool removeImage(int id);

    // ——— 读操作 ———

    // 去重：检查 hash 是否已存在
    bool existsByHash(const std::string& hash);

    // 粗筛：按 LAB-L 范围查询候选（用于马赛克匹配第一阶段）
    std::vector<ImageRecord> queryByLRange(double minL, double maxL, int limit = 200);

    // 轻量版：仅返回 id 列表（GPU 路径用，避免取 BLOB/长文本列）
    // sortByUse=true 按使用次数升序；GPU 路径无需排序可传 false 大幅加速
    std::vector<int> queryIdsByLRange(double minL, double maxL, int limit = 200,
                                       bool sortByUse = true);

    // 查询全部记录
    std::vector<ImageRecord> allRecords() { return queryByLRange(0.0, 255.0, 999999); }

    // 获取图库总数
    int totalCount();

    // ——— 元数据 ———
    // 设置全局配置项（如 feature_w=180）；value 为任意字符串
    bool setMeta(const std::string& key, const std::string& value);
    // 读取配置项；若不存在则返回 defaultVal
    std::string getMeta(const std::string& key, const std::string& defaultVal = "");

    // ——— 使用统计 ———
    // 记录一次马赛克生成中各图片的使用情况（同一图片多次 tile 仅计一次 run）
    void recordRunUsage(const std::unordered_map<int, int>& imageUseCount,
                        const std::string& targetHash = "",
                        const std::string& targetPath = "");
    // 查询使用统计：返回 (image_id, total_runs, total_tiles) 按 total_runs 降序
    std::vector<std::tuple<int, int, int>> topUsedImages(int limit = 50);
    void initUsageStats();

private:
    sqlite3* m_db;

    // 执行无结果集 SQL（CREATE / INSERT / DELETE）
    bool exec(const std::string& sql);

public:
    // 事务控制
    bool beginTransaction() { return exec("BEGIN TRANSACTION"); }
    bool commitTransaction() { return exec("COMMIT"); }
private:

    // 检查某列是否存在
    bool columnExists(const std::string& table, const std::string& column);

    // 自动添加旧表缺失的列
    void migrate();
};

} // namespace mosaicraft
