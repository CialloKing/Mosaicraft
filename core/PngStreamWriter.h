#pragma once
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <png.h>

namespace mosaicraft {

class PngStreamWriter
{
public:
    PngStreamWriter(const std::string& path, int w, int h) : m_w(w), m_h(h)
    {
#ifdef _WIN32
        std::wstring wp = std::filesystem::u8path(path).wstring();
        m_fp = _wfopen(wp.c_str(), L"wb");
#else
        m_fp = fopen(path.c_str(), "wb");
#endif
        if (!m_fp) throw std::runtime_error("PngStreamWriter: open failed");
        m_png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!m_png) { fclose(m_fp); throw std::runtime_error("png_create_write_struct"); }
        m_info = png_create_info_struct(m_png);
        if (!m_info) { png_destroy_write_struct(&m_png, nullptr); fclose(m_fp);
                       throw std::runtime_error("png_create_info_struct"); }
        if (setjmp(png_jmpbuf(m_png))) { png_destroy_write_struct(&m_png, &m_info); fclose(m_fp);
            throw std::runtime_error("PngStreamWriter: init"); }
        png_init_io(m_png, m_fp);
        png_set_IHDR(m_png, m_info, w, h, 8, PNG_COLOR_TYPE_RGB,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(m_png, m_info);
    }

    bool writeRow(int y, const uint8_t* bgr)
    {
        if (!m_png) return false;
        m_buf.resize(m_w * 3);
        for (int x = 0; x < m_w; ++x) {
            m_buf[x*3+0] = bgr[x*3+2]; m_buf[x*3+1] = bgr[x*3+1]; m_buf[x*3+2] = bgr[x*3+0];
        }
        // 直接写行——libpng 错误会 longjmp 回构造的 setjmp，触发异常
        png_write_row(m_png, m_buf.data());
        return true;
    }

    void close() {
        if (m_png) { png_write_end(m_png, m_info); png_destroy_write_struct(&m_png, &m_info); m_png=nullptr; }
        if (m_fp) { fclose(m_fp); m_fp=nullptr; }
    }

    ~PngStreamWriter() { close(); }

private:
    FILE* m_fp=nullptr; png_structp m_png=nullptr; png_infop m_info=nullptr;
    int m_w, m_h; std::vector<uint8_t> m_buf;
};

}
