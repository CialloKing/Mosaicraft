#pragma once
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>
namespace mosaicraft
{
inline bool parseStrictInt(const std::string &text, int &out)
{
    if (text.empty())
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' || value < INT_MIN || value > INT_MAX)
    {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

inline bool parsePositiveInt(const std::string &text, int &out)
{
    int value = 0;
    if (!parseStrictInt(text, value) || value <= 0)
    {
        return false;
    }
    out = value;
    return true;
}

inline bool parseSize(const std::string &text, int &w, int &h)
{
    size_t sep = text.find_first_of("xX");
    if (sep == std::string::npos || sep == 0 || sep + 1 >= text.size())
    {
        return false;
    }
    if (text.find_first_of("xX", sep + 1) != std::string::npos)
    {
        return false;
    }
    int parsedW = 0;
    int parsedH = 0;
    if (!parsePositiveInt(text.substr(0, sep), parsedW) || !parsePositiveInt(text.substr(sep + 1), parsedH))
    {
        return false;
    }
    w = parsedW;
    h = parsedH;
    return true;
}

} // namespace mosaicraft
