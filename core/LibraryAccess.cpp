#include "LibraryAccess.h"
#include "UnicodeIO.h"
#include <chrono>
#include <thread>
#include <cerrno>

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

LibraryAccess::LibraryAccess(const std::string& dbPath, bool exclusive, int waitMilliseconds)
{
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(u8path(dbPath), error);
    if (error)
    {
        return;
    }
    const auto lockPath = u8path(pathToUtf8(path) + ".library.lock");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMilliseconds);
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
    do
    {
        m_acquired = LockFileEx(handle, flags, 0, 1, 0, &overlapped) != 0;
        if (m_acquired || GetLastError() != ERROR_LOCK_VIOLATION || std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (true);
#else
    m_fd = open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    while (m_fd >= 0)
    {
        m_acquired = flock(m_fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0;
        if (m_acquired || (errno != EWOULDBLOCK && errno != EAGAIN) || std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
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
