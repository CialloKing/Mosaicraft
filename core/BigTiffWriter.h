#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// libtiff C API — 已在 vcpkg 中（OpenCV 依赖）
#include <tiffio.h>

namespace mosaicraft
{

// ============================================================
// BigTiffWriter — 通过 libtiff 直写超大 TIFF
//
// OpenCV 的 cv::imwrite 对所有格式都有 65500px 硬限制。
// libtiff 原生支持 BigTIFF（8 字节偏移），最大文件 18EB，
// 图像尺寸仅受内存限制。
//
// 用法：
//   BigTiffWriter w("output.tiff", width, height);
//   w.writeScanline(row_data, row_index);
//   w.close();
// ============================================================
class BigTiffWriter
{
public:
    // 打开文件准备写入：BGR 8-bit 交错格式，LZW 压缩
    BigTiffWriter(const std::string& path, int width, int height)
        : m_w(width), m_h(height)
    {
        // 使用 "w8" 模式启用 BigTIFF
        m_tif = TIFFOpen(path.c_str(), "w8");
        if (!m_tif)
            throw std::runtime_error("BigTiffWriter: cannot open " + path);

        TIFFSetField(m_tif, TIFFTAG_IMAGEWIDTH,      width);
        TIFFSetField(m_tif, TIFFTAG_IMAGELENGTH,     height);
        TIFFSetField(m_tif, TIFFTAG_BITSPERSAMPLE,   8);
        TIFFSetField(m_tif, TIFFTAG_SAMPLESPERPIXEL, 3);  // BGR = 3 通道
        TIFFSetField(m_tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
        TIFFSetField(m_tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
        TIFFSetField(m_tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
        TIFFSetField(m_tif, TIFFTAG_COMPRESSION,     COMPRESSION_LZW);
        TIFFSetField(m_tif, TIFFTAG_PREDICTOR,       PREDICTOR_HORIZONTAL);  // 提高 LZW 压缩率
        TIFFSetField(m_tif, TIFFTAG_ROWSPERSTRIP,    1);  // 逐行写入

        m_stripSize = TIFFStripSize(m_tif);
        m_buf.resize(m_stripSize);
    }

    ~BigTiffWriter()
    {
        if (m_tif) TIFFClose(m_tif);
    }

    // 写入一行 BGR 像素（数据长度必须 >= width * 3）
    void writeScanline(const uint8_t* bgrRow, int row)
    {
        // libtiff 期望 RGB，但 BGR 可通过 PHOTOMETRIC_RGB + 无 sub-IFD 写入
        // 直接写 BGR 数据即可（libtiff 不做颜色转换）
        if (TIFFWriteScanline(m_tif, const_cast<uint8_t*>(bgrRow), row, 0) < 0)
            throw std::runtime_error("BigTiffWriter: write error at row " + std::to_string(row));
    }

    // 从 cv::Mat 逐行写入（避免逐像素复制，直接传指针）
    void writeMat(const void* data, int step)
    {
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        for (int y = 0; y < m_h; ++y)
            writeScanline(ptr + y * step, y);
    }

    void close()
    {
        if (m_tif)
        {
            TIFFClose(m_tif);
            m_tif = nullptr;
        }
    }

private:
    TIFF* m_tif = nullptr;
    int m_w = 0;
    int m_h = 0;
    tsize_t m_stripSize = 0;
    std::vector<uint8_t> m_buf;
};

} // namespace mosaicraft
