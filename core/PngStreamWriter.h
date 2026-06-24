#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <png.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace mosaicraft {

// 流式 PNG 写出器：逐行写入，无需全图 cv::Mat
// 与 BigTiffWriter 对称，支持超大图（>65500px 需 libpng 16+）
class PngStreamWriter
{
public:
    PngStreamWriter(const std::string& path, int width, int height)
        : m_w(width), m_h(height)
    {
#ifdef _WIN32
        std::wstring wpath = std::filesystem::u8path(path).wstring();
        m_fp = _wfopen(wpath.c_str(), L"wb");
#else
        m_fp = fopen(path.c_str(), "wb");
#endif
        if (!m_fp)
            throw std::runtime_error("PngStreamWriter: cannot open " + path);

        m_png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!m_png) { fclose(m_fp); throw std::runtime_error("png_create_write_struct failed"); }

        m_info = png_create_info_struct(m_png);
        if (!m_info) { png_destroy_write_struct(&m_png, nullptr); fclose(m_fp);
                       throw std::runtime_error("png_create_info_struct failed"); }

        if (setjmp(png_jmpbuf(m_png))) {
            png_destroy_write_struct(&m_png, &m_info); fclose(m_fp);
            throw std::runtime_error("PngStreamWriter: libpng error");
        }

        png_init_io(m_png, m_fp);
        png_set_IHDR(m_png, m_info, width, height, 8, PNG_COLOR_TYPE_RGB,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(m_png, m_info);
    }

    ~PngStreamWriter() { close(); }

    // 逐行写入 BGR→RGB 转换
    bool writeRow(int y, const uint8_t* bgrRow)
    {
        if (!m_png) return false;
        m_rowBuf.resize(m_w * 3);
        // BGR → RGB
        for (int x = 0; x < m_w; ++x) {
            m_rowBuf[x * 3 + 0] = bgrRow[x * 3 + 2];
            m_rowBuf[x * 3 + 1] = bgrRow[x * 3 + 1];
            m_rowBuf[x * 3 + 2] = bgrRow[x * 3 + 0];
        }
        png_bytep rowPtr = m_rowBuf.data();
        png_write_rows(m_png, &rowPtr, 1);
        return true;
    }

    void close()
    {
        if (m_png) {
            png_write_end(m_png, m_info);
            png_destroy_write_struct(&m_png, &m_info);
            m_png = nullptr; m_info = nullptr;
        }
        if (m_fp) { fclose(m_fp); m_fp = nullptr; }
    }

private:
    FILE* m_fp = nullptr;
    png_structp m_png = nullptr;
    png_infop m_info = nullptr;
    int m_w, m_h;
    std::vector<uint8_t> m_rowBuf;
};

} // namespace mosaicraft
