#pragma once
#include <filesystem>
#include <string>

namespace mosaicraft
{
inline constexpr const char *portablePathFormat = "db-relative-v1";

// 图库内的持久路径以数据库目录为锚点；调用者的输入输出参数仍使用其工作目录。
class LibraryPaths
{
    std::filesystem::path m_root;
  public:
    explicit LibraryPaths(const std::string &database);
    std::string store(const std::string &path) const;
    std::string legacy(const std::string &path) const;
    std::string resolve(const std::string &path) const;
    static bool relative(const std::string &path);
};
} // namespace mosaicraft
