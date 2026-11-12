#pragma once
#include "UnicodeIO.h"
#include <filesystem>
#include <fstream>

namespace mosaicraft
{
enum class LibraryFileState
{
    Available,
    Missing,
    Unreadable
};

inline LibraryFileState inspectLibraryFile(const std::string &path)
{
    std::error_code error;
    const auto status = std::filesystem::status(u8path(path), error);
    if (!path.empty() && status.type() == std::filesystem::file_type::not_found)
    {
        return LibraryFileState::Missing;
    }
    if (error || !std::filesystem::is_regular_file(status))
    {
        return LibraryFileState::Unreadable;
    }
    // status 成功不代表当前进程可读；权限错误不能误判成可清理的缺失。
    std::ifstream input(u8path(path), std::ios::binary);
    return input ? LibraryFileState::Available : LibraryFileState::Unreadable;
}
} // namespace mosaicraft
