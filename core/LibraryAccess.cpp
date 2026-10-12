#include "LibraryAccess.h"
#include "UnicodeIO.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mosaicraft
{

std::filesystem::path libraryJournalPath(const std::string& dbPath)
{
    return u8path(pathToUtf8(std::filesystem::weakly_canonical(u8path(dbPath))) + ".color-order.json");
}

LibraryAccess::LibraryAccess(const std::string& dbPath, bool exclusive)
{
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(u8path(dbPath), error);
    if (error)
    {
        return;
    }
    const auto lockPath = u8path(pathToUtf8(path) + ".library.lock");
#ifdef _WIN32
    const auto handle = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return;
    }
    m_handle = handle;
    OVERLAPPED overlapped{};
    const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    m_acquired = LockFileEx(handle, flags, 0, 1, 0, &overlapped) != 0;
#else
    m_fd = open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    m_acquired = m_fd >= 0 && flock(m_fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0;
#endif
}

LibraryAccess::~LibraryAccess()
{
#ifdef _WIN32
    if (m_handle)
    {
        CloseHandle(m_handle);
    }
#else
    if (m_fd >= 0)
    {
        close(m_fd);
    }
#endif
}

} // namespace mosaicraft
