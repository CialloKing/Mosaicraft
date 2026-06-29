#pragma once
// RAII guard: disables Windows console Quick Edit mode (accidental click → freeze)
// and restores original mode on destruction or atexit (handles Ctrl+C / ExitProcess).
// No-op on non-Windows platforms.
//
// Usage:
//   int main() {
//       ConsoleQuickEditGuard guard;  // disable during long ops
//       // ... long running task ...
//   }  // restored by destructor (normal exit) or atexit (Ctrl+C/ExitProcess)

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdlib>   // std::atexit

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

                // 注册 atexit：即使 Ctrl+C → ExitProcess 不触发析构，也能恢复
                s_handle = m_hConsole;
                s_oldMode = m_oldMode;
                if (!s_atexitRegistered)
                {
                    s_atexitRegistered = true;
                    std::atexit([]() {
                        if (s_handle != nullptr && s_handle != INVALID_HANDLE_VALUE)
                            SetConsoleMode(s_handle, s_oldMode);
                    });
                }
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
            s_handle = nullptr;  // prevent double-restore from atexit
        }
    }

    ConsoleQuickEditGuard(const ConsoleQuickEditGuard&) = delete;
    ConsoleQuickEditGuard& operator=(const ConsoleQuickEditGuard&) = delete;

private:
    HANDLE m_hConsole = nullptr;
    DWORD m_oldMode = 0;
    bool  m_restore = false;

    // 静态备份：析构函数不执行时（Ctrl+C → ExitProcess），atexit 用这些恢复
    inline static HANDLE s_handle = nullptr;
    inline static DWORD  s_oldMode = 0;
    inline static bool   s_atexitRegistered = false;
};

} // namespace mosaicraft

#else  // !_WIN32

namespace mosaicraft {

class ConsoleQuickEditGuard
{
public:
    ConsoleQuickEditGuard() = default;
    ~ConsoleQuickEditGuard() = default;
    ConsoleQuickEditGuard(const ConsoleQuickEditGuard&) = delete;
    ConsoleQuickEditGuard& operator=(const ConsoleQuickEditGuard&) = delete;
};

} // namespace mosaicraft

#endif // _WIN32
