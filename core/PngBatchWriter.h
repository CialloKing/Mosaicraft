#pragma once
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <png.h>

namespace mosaicraft {

// 批量 PNG 写出：收集所有行后一次 png_write_image
// 内存 = outW × outH × 3 字节（相比 cv::Mat 免 OpenCV 额外缓冲）
class PngBatchWriter
{
public:
    PngBatchWriter(const PngBatchWriter&) = delete;
    PngBatchWriter& operator=(const PngBatchWriter&) = delete;

    PngBatchWriter(const std::string& path, int w, int h, int compressionLevel = 1) : m_w(w), m_h(h)
    {
#ifdef _WIN32
        std::wstring wp = std::filesystem::u8path(path).wstring();
        m_fp = _wfopen(wp.c_str(), L"wb");
#else
        m_fp = fopen(path.c_str(), "wb");
#endif
        if (!m_fp) throw std::runtime_error("PngBatchWriter: open");
        m_png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!m_png) { fclose(m_fp); throw std::runtime_error("png_create_write_struct"); }
        m_info = png_create_info_struct(m_png);
        if (!m_info) { png_destroy_write_struct(&m_png,nullptr); fclose(m_fp); throw std::runtime_error("png_create_info_struct"); }
        if (setjmp(png_jmpbuf(m_png))) { png_destroy_write_struct(&m_png,&m_info); fclose(m_fp); throw std::runtime_error("Png init"); }
        png_init_io(m_png, m_fp);
        png_set_filter(m_png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);
        png_set_compression_level(m_png, compressionLevel);
        png_set_IHDR(m_png, m_info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(m_png, m_info);
        // 预分配全部行缓冲区（BGR→RGB 在 writeAll 中转换）
        m_image.resize(static_cast<size_t>(w) * h * 3);
        m_rows.resize(h);
    }

    // 获取某行的写入指针（调用方直接写 BGR 数据到此）
    uint8_t* rowData(int y) { return &m_image[static_cast<size_t>(y) * m_w * 3]; }

    // 全部写入并关闭
    bool writeAll()
    {
        if (!m_png) return false;
        // BGR→RGB 原地转换
        std::vector<uint8_t> rgb(m_w * 3);
        for (int y = 0; y < m_h; ++y) {
            const uint8_t* bgr = &m_image[static_cast<size_t>(y) * m_w * 3];
            for (int x = 0; x < m_w; ++x) {
                rgb[x*3+0] = bgr[x*3+2]; rgb[x*3+1] = bgr[x*3+1]; rgb[x*3+2] = bgr[x*3+0];
            }
            std::memcpy(const_cast<uint8_t*>(bgr), rgb.data(), m_w * 3);
        }
        // m_image 已转为 RGB，设置行指针
        for (int y = 0; y < m_h; ++y)
            m_rows[y] = &m_image[static_cast<size_t>(y) * m_w * 3];

        // 分批写入，每 kFlushInterval 行刷新一次，防止 libpng 内部压缩缓冲区溢出
        // png_write_image 一次性写入全部行无 flush，超大图(~4GB 原始数据)会导致
        // libpng 内部 ZLIB 缓冲区在写入中途耗尽内存而 longjmp，留下截断的 PNG 文件
        const int kFlushInterval = 1000;
        if (setjmp(png_jmpbuf(m_png))) {
            destroyAfterError();
            return false;
        }
        for (int y = 0; y < m_h; y += kFlushInterval)
        {
            int batchH = kFlushInterval;
            if (batchH > m_h - y) batchH = m_h - y;
            png_write_rows(m_png, &m_rows[y], batchH);
            png_write_flush(m_png);
        }
        png_write_end(m_png, m_info);
        png_destroy_write_struct(&m_png, &m_info);
        m_png = nullptr;
        fclose(m_fp); m_fp = nullptr;
        return true;
    }

    ~PngBatchWriter()
    {
        if (m_png) { png_destroy_write_struct(&m_png, &m_info); }
        if (m_fp) { fclose(m_fp); }
    }

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

    FILE* m_fp=nullptr; png_structp m_png=nullptr; png_infop m_info=nullptr;
    int m_w, m_h;
    std::vector<uint8_t> m_image;
    std::vector<png_bytep> m_rows;
};

}
