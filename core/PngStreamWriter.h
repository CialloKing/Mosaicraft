#pragma once
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <png.h>

namespace mosaicraft {

// 真正流式 PNG 写出器：逐行接收 RGB 数据并立即写入 libpng
// 不缓存全图，内存仅 ~m_w*3 字节（单行缓冲）
// 调用方负责 BGR→RGB 转换（在组装行时内联完成，消除独立遍历）
class PngStreamWriter
{
public:
    PngStreamWriter(const PngStreamWriter&) = delete;
    PngStreamWriter& operator=(const PngStreamWriter&) = delete;

    PngStreamWriter(const std::string& path, int w, int h, int compressionLevel = 1) : m_w(w), m_h(h)
    {
#ifdef _WIN32
        std::wstring wp = std::filesystem::u8path(path).wstring();
        m_fp = _wfopen(wp.c_str(), L"wb");
#else
        m_fp = fopen(path.c_str(), "wb");
#endif
        if (!m_fp) throw std::runtime_error("Png open");
        m_png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!m_png) { fclose(m_fp); throw std::runtime_error("png_create_write_struct"); }
        m_info = png_create_info_struct(m_png);
        if (!m_info) { png_destroy_write_struct(&m_png,nullptr); fclose(m_fp); throw std::runtime_error("png_create_info_struct"); }
        if (setjmp(png_jmpbuf(m_png))) { png_destroy_write_struct(&m_png,&m_info); fclose(m_fp); throw std::runtime_error("Png init"); }
        png_init_io(m_png, m_fp);
        // 压缩级别：1=最快 9=最小，默认 1
        png_set_compression_level(m_png, compressionLevel);
        // 全部 5 种过滤器：马赛克 tile 边界处 Up/Average/Paeth 能显著提升压缩率
        png_set_filter(m_png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);
        png_set_IHDR(m_png, m_info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(m_png, m_info);
        m_rowBuf.resize(m_w * 3);
    }

    // 写入一行 RGB 数据（每行 m_w*3 字节，R/G/B 顺序）
    // 调用方已在组装行时完成 BGR→RGB 转换，此方法直接写盘
    bool writeRow(const uint8_t* rgb)
    {
        if (!m_png) return false;
        if (setjmp(png_jmpbuf(m_png))) {
            destroyAfterError();
            return false;
        }
        png_write_row(m_png, const_cast<png_bytep>(rgb));
        m_rowsWritten++;
        // 每 1000 行刷新一次，防止 libpng 内部压缩缓冲区无限增长
        if ((m_rowsWritten % 1000) == 0) png_write_flush(m_png);
        return true;
    }

    void close()
    {
        if (m_png)
        {
            if (!setjmp(png_jmpbuf(m_png)))
            {
                png_write_end(m_png, m_info);
            }
            png_destroy_write_struct(&m_png, &m_info);
            m_png = nullptr;
        }
        if (m_fp) { fclose(m_fp); m_fp = nullptr; }
    }

    ~PngStreamWriter() { close(); }

private:
    void destroyAfterError()
    {
        if (m_png)
        {
            png_destroy_write_struct(&m_png, &m_info);
            m_png = nullptr;
            m_info = nullptr;
        }
        if (m_fp) { fclose(m_fp); m_fp = nullptr; }
    }

    FILE* m_fp = nullptr;
    png_structp m_png = nullptr;
    png_infop m_info = nullptr;
    int m_w, m_h;
    int m_rowsWritten = 0;
    std::vector<uint8_t> m_rowBuf;  // 保留以备将来使用，目前 writeRow 直接使用调用方缓冲
};

}
