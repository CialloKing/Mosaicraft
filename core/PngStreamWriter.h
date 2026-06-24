#pragma once

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <png.h>

namespace mosaicraft {

// 流式 PNG 写出：逐行写入，无需全图 cv::Mat
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
        if (!m_fp) throw std::runtime_error("PngStreamWriter: cannot open " + path);

        m_png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!m_png) { fclose(m_fp); throw std::runtime_error("png_create_write_struct failed"); }

        m_info = png_create_info_struct(m_png);
        if (!m_info) { png_destroy_write_struct(&m_png, nullptr); fclose(m_fp);
                       throw std::runtime_error("png_create_info_struct failed"); }

        // 标准 libpng 错误处理：setjmp 在构造时，覆盖写阶段的错误
        if (setjmp(png_jmpbuf(m_png))) {
            png_destroy_write_struct(&m_png, &m_info); fclose(m_fp);
            m_png = nullptr; m_fp = nullptr;
            throw std::runtime_error("PngStreamWriter: init error");
        }

        png_init_io(m_png, m_fp);
        png_set_IHDR(m_png, m_info, width, height, 8, PNG_COLOR_TYPE_RGB,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(m_png, m_info);
    }

    ~PngStreamWriter() { close(); }

    bool writeRow(int y, const uint8_t* bgrRow)
    {
        if (!m_png) return false;
        // 每次 writeRow 前设置 setjmp：libpng 写错误会跳回此处
        if (setjmp(png_jmpbuf(m_png))) {
            m_hasError = true;
            return false;
        }
        // BGR→RGB 转换 + 写入
        m_rowBuf.resize(m_w * 3);
        for (int x = 0; x < m_w; ++x) {
            m_rowBuf[x * 3 + 0] = bgrRow[x * 3 + 2];
            m_rowBuf[x * 3 + 1] = bgrRow[x * 3 + 1];
            m_rowBuf[x * 3 + 2] = bgrRow[x * 3 + 0];
        }
        png_write_row(m_png, m_rowBuf.data());
        return true;
    }

    void close()
    {
        if (m_png && !m_hasError) {
            // 关闭前最后设置 setjmp
            if (!setjmp(png_jmpbuf(m_png)))
                png_write_end(m_png, m_info);
        }
        if (m_png) {
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
    bool m_hasError = false;
    std::vector<uint8_t> m_rowBuf;
};

} // namespace mosaicraft
