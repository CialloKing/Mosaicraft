#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mosaicraft
{

class Database;
struct ImageRecord;

// 持有独占锁期间暂存素材；SQLite 提交标记决定恢复时保留还是撤回已发布文件。
class LibraryIngest
{
public:
    LibraryIngest(Database &db, const std::filesystem::path &output);
    ~LibraryIngest();
    LibraryIngest(const LibraryIngest &) = delete;
    LibraryIngest &operator=(const LibraryIngest &) = delete;
    const std::filesystem::path &staging() const { return m_staging; }
    void publish(const std::vector<ImageRecord *> &records);
    bool finish(std::string &error);

private:
    Database &m_db;
    std::filesystem::path m_output, m_staging;
    bool m_active = true;
};
bool recoverLibraryIngest(Database &database, std::string &error);

struct ColorOrderResult
{
    int computed = 0;
    int renamed = 0;
    double validationMs = 0;
    double sortingMs = 0;
    double writingMs = 0;
};

// 调用者须以 Build 或 Recover 模式持有独占图库锁。
bool recoverLibraryColorOrder(Database& database, std::string& error);
bool reorderLibraryByColor(Database& database, const std::string& outputDir,
                           ColorOrderResult& result, std::string& error);

} // namespace mosaicraft
