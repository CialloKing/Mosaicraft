#include "LibraryPaths.h"
#include "UnicodeIO.h"
#include <algorithm>
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
    // macOS 的 /var 与 /private/var 等别名必须在缓存库根时统一，避免误判库内资源。
    m_root = std::filesystem::weakly_canonical(std::filesystem::absolute(m_root));
    if (MOSAICRAFT_TRIAL_LIBRARY_PATHS)
    {
        m_prefix = pathToUtf8(m_root / std::filesystem::path());
    }
}
bool LibraryPaths::relative(const std::string &value)
{
    if (value.empty())
    {
        return true;
    }
    const auto path = u8path(value);
    // Windows 的 /file 有根目录却没有盘符，is_absolute() 不足以排除它。
    if (path.has_root_directory() || path.has_root_name() || foreignAbsolute(value) || value.find('\\') != std::string::npos)
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
std::string LibraryPaths::resolve(const std::string &value) const
{
    if (value.empty())
    {
        return {};
    }
    if (MOSAICRAFT_TRIAL_LIBRARY_PATHS)
    {
        // 只走已能证明是规范相对路径的 ASCII 子集；其他输入保留平台原有校验和规范化。
        bool simple = true, first = true;
        for (unsigned char c : value)
        {
            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (!letter && (first || (c != '_' && c != '-' && c != '.' && c != '/')))
            {
                simple = false;
                break;
            }
            first = c == '/';
        }
        if (simple && !first)
        {
            std::string result = m_prefix + value;
#ifdef _WIN32
            std::replace(result.begin() + m_prefix.size(), result.end(), '/', '\\');
#endif
            return result;
        }
    }
    if (!relative(value))
    {
        throw std::invalid_argument("invalid portable library path: " + value);
    }
    return pathToUtf8((m_root / u8path(value)).lexically_normal());
}
} // namespace mosaicraft
