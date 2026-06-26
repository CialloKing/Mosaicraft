#pragma once
#include <cstdio>
#include <csetjmp>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <jpeglib.h>

namespace mosaicraft {

// 流式 JPEG 写出器：逐行接收 RGB 数据并立即写入 libjpeg
// 内存仅 ~outW*3 字节（单行缓冲），不缓存全图
// 调用方负责 BGR→RGB 转换（在组装行时内联完成）
class JpgStreamWriter
{
public:
    JpgStreamWriter(const std::string& path, int w, int h, int quality = 100) : m_w(w), m_h(h)
    {
#ifdef _WIN32
        std::wstring wp = std::filesystem::u8path(path).wstring();
        m_fp = _wfopen(wp.c_str(), L"wb");
#else
        m_fp = fopen(path.c_str(), "wb");
#endif
        if (!m_fp) throw std::runtime_error("Jpg open");
        m_cinfo.err = jpeg_std_error(&m_jerr);
        // 覆盖默认exit()行为：致命错误通过setjmp/longjmp处理
        m_cinfo.client_data = &m_jmpBuf;
        m_jerr.error_exit = [](j_common_ptr cinfo) {
            longjmp(*(jmp_buf*)cinfo->client_data, 1);
        };
        if (setjmp(m_jmpBuf)) {
            // libjpeg 致命错误时跳转至此
            jpeg_destroy_compress(&m_cinfo);
            if (m_fp) { fclose(m_fp); m_fp = nullptr; }
            return;
        }
        jpeg_create_compress(&m_cinfo);
        jpeg_stdio_dest(&m_cinfo, m_fp);
        m_cinfo.image_width = w;
        m_cinfo.image_height = h;
        m_cinfo.input_components = 3;
        m_cinfo.in_color_space = JCS_RGB;
        jpeg_set_defaults(&m_cinfo);
        // 使用 4:2:0 色度子采样 + 优化 Huffman 表，与 OpenCV imwrite 行为一致
        m_cinfo.comp_info[0].h_samp_factor = 2;
        m_cinfo.comp_info[0].v_samp_factor = 2;
        // Y:Cb:Cr = 4:2:0 (Cb/Cr 各 1/4 采样)
        jpeg_set_quality(&m_cinfo, quality, TRUE);
        jpeg_start_compress(&m_cinfo, TRUE);
    }

    // 写入一行 RGB 数据（每行 m_w*3 字节，R/G/B 顺序）
    bool writeRow(const uint8_t* rgb)
    {
        if (!m_fp) return false;
        JSAMPROW row = const_cast<JSAMPROW>(rgb);
        jpeg_write_scanlines(&m_cinfo, &row, 1);
        m_rowsWritten++;
        return true;
    }

    void close()
    {
        if (m_fp)
        {
            jpeg_finish_compress(&m_cinfo);
            jpeg_destroy_compress(&m_cinfo);
            fclose(m_fp);
            m_fp = nullptr;
        }
    }

    ~JpgStreamWriter() { close(); }

private:
    FILE* m_fp = nullptr;
    struct jpeg_compress_struct m_cinfo = {};
    struct jpeg_error_mgr m_jerr = {};
    jmp_buf m_jmpBuf = {};
    int m_w, m_h;
    int m_rowsWritten = 0;
};

}
