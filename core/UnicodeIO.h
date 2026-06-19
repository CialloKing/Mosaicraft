#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX    // 避免 windows.h 的 min/max 宏污染 OpenCV
#include <windows.h>
#endif

namespace mosaicraft
{

// ============================================================
// pathToUtf8: std::filesystem::path → UTF-8 std::string
// ============================================================
inline std::string pathToUtf8(const std::filesystem::path& p)
{
#ifdef _WIN32
    std::wstring wstr = p.wstring();
    if (wstr.empty())
    {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0)
    {
        return p.string();  // fallback
    }
    std::string result(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
                        &result[0], len, nullptr, nullptr);
    // drop trailing null
    while (!result.empty() && result.back() == '\0')
    {
        result.pop_back();
    }
    return result;
#else
    return p.string();
#endif
}

// ============================================================
// u8path: UTF-8 std::string → std::filesystem::path
// fs::path(string) on Windows uses ACP (e.g. GBK), corrupting
// non-ASCII. This converts via wide chars first.
// ============================================================
inline std::filesystem::path u8path(const std::string& utf8)
{
#ifdef _WIN32
    if (utf8.empty())
    {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0)
    {
        return std::filesystem::path(utf8);  // fallback
    }
    std::wstring wstr(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], len);
    while (!wstr.empty() && wstr.back() == L'\0')
    {
        wstr.pop_back();
    }
    return std::filesystem::path(wstr);
#else
    return std::filesystem::path(utf8);
#endif
}

// ============================================================
// Unicode-safe imread / imwrite
// Paths are UTF-8 encoded std::string.
// ============================================================
inline cv::Mat imreadUnicode(const std::string& utf8Path,
                              int flags = cv::IMREAD_COLOR)
{
    std::ifstream ifs(u8path(utf8Path), std::ios::binary);
    if (!ifs.is_open())
    {
        return cv::Mat();
    }

    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());
    ifs.close();

    return cv::imdecode(data, flags);
}

inline bool imwriteUnicode(const std::string& utf8Path,
                            const cv::Mat& img,
                            const std::vector<int>& params = {})
{
    std::string ext = u8path(utf8Path).extension().string();
    std::vector<uint8_t> buf;
    if (!cv::imencode(ext, img, buf, params))
    {
        return false;
    }

    std::ofstream ofs(u8path(utf8Path), std::ios::binary);
    if (!ofs.is_open())
    {
        return false;
    }

    ofs.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    ofs.close();
    return ofs.good();
}

} // namespace mosaicraft
