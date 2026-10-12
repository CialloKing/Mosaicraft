#pragma once

#include <filesystem>
#include <string>

namespace mosaicraft
{

std::filesystem::path libraryJournalPath(const std::string& dbPath);

// 多个读取者可并行；建库持有独占锁，覆盖归一化、重命名及崩溃恢复。
class LibraryAccess
{
public:
    LibraryAccess(const std::string& dbPath, bool exclusive);
    ~LibraryAccess();
    LibraryAccess(const LibraryAccess&) = delete;
    LibraryAccess& operator=(const LibraryAccess&) = delete;
    bool acquired() const { return m_acquired; }

private:
    bool m_acquired = false;
#ifdef _WIN32
    void* m_handle = nullptr;
#else
    int m_fd = -1;
#endif
};

} // namespace mosaicraft
