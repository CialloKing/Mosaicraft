#include "LibraryPaths.h"
#include "UnicodeIO.h"
#include <cctype>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mosaicraft
{
namespace
{
bool equalComponent(const std::filesystem::path &a, const std::filesystem::path &b)
{
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
bool foreignAbsolute(const std::string &path)
{
    // POSIX 不把 Windows 盘符视为绝对路径，不能把旧盘符误当作库内文件名。
    return (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') ||
        path.rfind("\\\\", 0) == 0 || path.rfind("//", 0) == 0;
}
}
LibraryPaths::LibraryPaths(const std::string &database) : m_root(u8path(database).parent_path())
{
    m_root = std::filesystem::absolute(m_root).lexically_normal();
}
bool LibraryPaths::relative(const std::string &value)
{
    if (value.empty())
    {
        return true;
    }
    const auto path = u8path(value);
    if (path.is_absolute() || path.has_root_name() || foreignAbsolute(value) || value.find('\\') != std::string::npos)
    {
        return false;
    }
    for (const auto &part : path)
    {
        if (part == "..")
        {
            return false;
        }
    }
    return path != ".";
}
std::string LibraryPaths::store(const std::string &value) const
{
    if (value.empty())
    {
        return {};
    }
    auto path = u8path(value);
    if (!path.is_absolute())
    {
        if (foreignAbsolute(value))
        {
            throw std::invalid_argument("library path belongs to another platform: " + value);
        }
        path = m_root / path;
    }
    path = path.lexically_normal();
    auto a = m_root.begin(), b = path.begin();
    for (; a != m_root.end(); ++a, ++b)
    {
        if (b == path.end() || !equalComponent(*a, *b))
        {
            // 输出目录可能经由符号链接访问；仅在前缀不匹配时解析真实位置。
            std::error_code error;
            const auto canonical = std::filesystem::weakly_canonical(path, error);
            if (!error && canonical != path)
            {
                return store(pathToUtf8(canonical));
            }
            throw std::invalid_argument("library resource is outside the database directory: " + value);
        }
    }
    std::filesystem::path result;
    for (; b != path.end(); ++b)
    {
        result /= *b;
    }
    const auto stored = result.generic_u8string();
    if (stored.empty() || !relative(stored))
    {
        throw std::invalid_argument("invalid library resource path: " + value);
    }
    return stored;
}
std::string LibraryPaths::legacy(const std::string &value) const
{
    if (value.empty())
    {
        return {};
    }
    auto path = u8path(value);
    if (!path.is_absolute() && foreignAbsolute(value))
    {
        throw std::invalid_argument("legacy library belongs to another platform");
    }
    // 保留旧相对路径的 CWD 语义；不存在的文件仍可转换，交给健康检查处理。
    std::error_code error;
    path = std::filesystem::weakly_canonical(std::filesystem::absolute(path), error);
    if (error)
    {
        throw std::invalid_argument("cannot resolve legacy library path: " + value);
    }
    return store(pathToUtf8(path));
}
std::string LibraryPaths::resolve(const std::string &value) const
{
    if (value.empty())
    {
        return {};
    }
    if (!relative(value))
    {
        throw std::invalid_argument("invalid portable library path: " + value);
    }
    return pathToUtf8((m_root / u8path(value)).lexically_normal());
}
} // namespace mosaicraft
