#pragma once
// 流式 JPEG 写出器：逐行接收 RGB 数据并立即通过 libjpeg 写盘
// 不缓存全图，内存恒定（libjpeg 内部分配少量压缩缓冲）
//
// 用法：
//   JpgStreamWriter jpg(outputPath, outW, outH, quality);
//   for (int y = 0; y < outH; ++y)
//       jpg.writeRow(rowBuf.data());   // rowBuf: outW*3 bytes, RGB order
//   jpg.close();

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <jpeglib.h>

namespace mosaicraft {

class JpgStreamWriter
{
public:
    JpgStreamWriter(const JpgStreamWriter&) = delete;
    JpgStreamWriter& operator=(const JpgStreamWriter&) = delete;

    JpgStreamWriter(const std::string& path, int w, int h, int quality = 95)
        : m_w(w), m_h(h)
    {
#ifdef _WIN32
        std::wstring wp = std::filesystem::u8path(path).wstring();
        m_fp = _wfopen(wp.c_str(), L"wb");
#else
        m_fp = fopen(path.c_str(), "wb");
#endif
        if (!m_fp)
            throw std::runtime_error("Jpg open failed: " + path);

        m_cinfo.err = jpeg_std_error(&m_jerr);
        jpeg_create_compress(&m_cinfo);
        jpeg_stdio_dest(&m_cinfo, m_fp);

        m_cinfo.image_width      = static_cast<JDIMENSION>(w);
        m_cinfo.image_height     = static_cast<JDIMENSION>(h);
        m_cinfo.input_components = 3;           // RGB
        m_cinfo.in_color_space   = JCS_RGB;

        jpeg_set_defaults(&m_cinfo);
        jpeg_set_quality(&m_cinfo, quality, TRUE);
        jpeg_start_compress(&m_cinfo, TRUE);
    }

    // 写入一行 RGB 数据（m_w*3 字节，R/G/B 顺序）
    // 调用方已在组装行时完成 BGR→RGB 转换
    bool writeRow(const uint8_t* rgb)
    {
        if (m_closed) return false;
        JSAMPROW row[1];
        row[0] = const_cast<JSAMPROW>(rgb);
        jpeg_write_scanlines(&m_cinfo, row, 1);
        m_rowsWritten++;
        return true;
    }

    void close()
    {
        if (m_closed) return;
        m_closed = true;
        jpeg_finish_compress(&m_cinfo);
        jpeg_destroy_compress(&m_cinfo);
        if (m_fp) { fclose(m_fp); m_fp = nullptr; }
    }

    ~JpgStreamWriter()
    {
        if (!m_closed) close();
    }

    int width()  const { return m_w; }
    int height() const { return m_h; }
    int rowsWritten() const { return m_rowsWritten; }

private:
    FILE*             m_fp = nullptr;
    int               m_w;
    int               m_h;
    int               m_rowsWritten = 0;
    bool              m_closed = false;
    jpeg_compress_struct m_cinfo{};
    jpeg_error_mgr       m_jerr{};
};

} // namespace mosaicraft
