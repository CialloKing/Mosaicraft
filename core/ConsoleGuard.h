#pragma once
// RAII guard: disables Windows console Quick Edit mode (accidental click → freeze)
// and restores original mode on destruction.
// No-op on non-Windows platforms.
//
// Usage:
//   int main() {
//       ConsoleQuickEditGuard guard;  // disable during long ops
//       // ... long running task ...
//   }  // guard destructor restores original mode

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mosaicraft {

class ConsoleQuickEditGuard
{
public:
    ConsoleQuickEditGuard()
    {
        m_hConsole = GetStdHandle(STD_INPUT_HANDLE);
        if (m_hConsole != INVALID_HANDLE_VALUE && m_hConsole != nullptr)
        {
            m_restore = GetConsoleMode(m_hConsole, &m_oldMode) != 0;
            if (m_restore)
            {
                // ENABLE_QUICK_EDIT_MODE 需要 ENABLE_EXTENDED_FLAGS 才能被识别
                DWORD newMode = (m_oldMode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE;
                SetConsoleMode(m_hConsole, newMode);
            }
        }
        else
        {
            m_restore = false;
        }
    }

    ~ConsoleQuickEditGuard()
    {
        if (m_restore && m_hConsole != nullptr && m_hConsole != INVALID_HANDLE_VALUE)
        {
            SetConsoleMode(m_hConsole, m_oldMode);
        }
    }

    ConsoleQuickEditGuard(const ConsoleQuickEditGuard&) = delete;
    ConsoleQuickEditGuard& operator=(const ConsoleQuickEditGuard&) = delete;

private:
    HANDLE m_hConsole = nullptr;
    DWORD m_oldMode = 0;
    bool  m_restore = false;
};

} // namespace mosaicraft

#else  // !_WIN32

namespace mosaicraft {

class ConsoleQuickEditGuard
{
public:
    // No-op on non-Windows
    ConsoleQuickEditGuard() = default;
    ~ConsoleQuickEditGuard() = default;
    ConsoleQuickEditGuard(const ConsoleQuickEditGuard&) = delete;
    ConsoleQuickEditGuard& operator=(const ConsoleQuickEditGuard&) = delete;
};

} // namespace mosaicraft

#endif // _WIN32
