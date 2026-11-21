#pragma once
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <limits>
#include <string_view>
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

// Thousandths of one percent keep half-pixel rounding and cache identities exact.
inline constexpr int64_t kTargetPercentUnit = 1000;

inline bool parseTargetSize(std::string_view text, int &width, int &height, int64_t &percentMilli)
{
    width = height = 0;
    percentMilli = 0;
    const auto digits = [](std::string_view part, auto &value)
    {
        if (part.empty())
        {
            return false;
        }
        const auto end = part.data() + part.size();
        const auto parsed = std::from_chars(part.data(), end, value);
        return part.front() != '-' && parsed.ec == std::errc{} && parsed.ptr == end;
    };
    if (!text.empty() && text.back() == '%')
    {
        text.remove_suffix(1);
        const auto dot = text.find('.');
        int64_t whole = 0, fraction = 0;
        if (!digits(text.substr(0, dot), whole))
        {
            return false;
        }
        if (dot != std::string_view::npos)
        {
            const auto part = text.substr(dot + 1);
            if (part.size() > 3 || !digits(part, fraction))
            {
                return false;
            }
            for (auto places = part.size(); places < 3; ++places)
            {
                fraction *= 10;
            }
        }
        if (whole > ((std::numeric_limits<int64_t>::max)() - fraction) / kTargetPercentUnit)
        {
            return false;
        }
        percentMilli = whole * kTargetPercentUnit + fraction;
        return percentMilli > 0;
    }
    const auto sep = text.find_first_of("xX");
    if (sep == std::string_view::npos)
    {
        return false;
    }
    const auto w = text.substr(0, sep), h = text.substr(sep + 1);
    return (w == "auto" || (digits(w, width) && width > 0)) &&
           (h == "auto" || (digits(h, height) && height > 0)) && (width > 0 || height > 0);
}

inline std::string formatTargetPercent(int64_t percentMilli)
{
    auto text = std::to_string(percentMilli / kTargetPercentUnit);
    if (const auto fraction = percentMilli % kTargetPercentUnit; fraction != 0)
    {
        // The leading 1 supplies zero padding before we trim insignificant zeroes.
        auto decimals = std::to_string(kTargetPercentUnit + fraction).substr(1);
        decimals.erase(decimals.find_last_not_of('0') + 1);
        text += '.' + decimals;
    }
    return text + '%';
}

} // namespace mosaicraft
