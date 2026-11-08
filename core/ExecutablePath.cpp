#include "ExecutablePath.h"

#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace mosaicraft
{
std::filesystem::path executableDirectory()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            throw std::runtime_error("Cannot locate the running executable");
        }
        if (length < buffer.size())
        {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        throw std::runtime_error("Cannot locate the running executable");
    }
    // dyld may return a symlink or a path containing '..'.
    return std::filesystem::canonical(buffer.data()).parent_path();
#else
    return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
}
}
