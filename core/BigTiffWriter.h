#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include <tiffio.h>

namespace mosaicraft
{

// ============================================================
// BigTiffWriter — libtiff 直写，支持超 65500px 尺寸
// ============================================================
class BigTiffWriter
{
public:
    BigTiffWriter(const std::string& path, int width, int height)
        : m_w(width), m_h(height)
    {
        int64_t rawSize = static_cast<int64_t>(width) * height * 3;
        const char* mode = (rawSize > 3500LL * 1024 * 1024) ? "w8" : "w";

        m_tif = TIFFOpen(path.c_str(), mode);
        if (!m_tif)
            throw std::runtime_error("BigTiffWriter: cannot open " + path);

        TIFFSetField(m_tif, TIFFTAG_IMAGEWIDTH,      width);
        TIFFSetField(m_tif, TIFFTAG_IMAGELENGTH,     height);
        TIFFSetField(m_tif, TIFFTAG_BITSPERSAMPLE,   8);
        TIFFSetField(m_tif, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(m_tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
        TIFFSetField(m_tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
        TIFFSetField(m_tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
        // LZW 需整图单 strip；文件过大时降级为无压缩避免单 strip 内存过高
        if (rawSize < 400LL * 1024 * 1024) {
            TIFFSetField(m_tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
        }
        TIFFSetField(m_tif, TIFFTAG_ROWSPERSTRIP,    height);
    }

    ~BigTiffWriter() { if (m_tif) TIFFClose(m_tif); }

    // 逐行写入（OpenCV Mat BGR 交错，step 为行字节数 = cols * 3 或补齐值）
    bool writeMat(const uint8_t* data, int step)
    {
        for (int y = 0; y < m_h; ++y)
        {
            if (TIFFWriteScanline(m_tif,
                    const_cast<uint8_t*>(data + static_cast<size_t>(y) * step),
                    y, 0) < 0)
            {
                std::cerr << "  BigTiffWriter: row " << y << " failed" << std::endl;
                return false;
            }
        }
        return true;
    }

    void close() { if (m_tif) { TIFFClose(m_tif); m_tif = nullptr; } }

private:
    TIFF* m_tif = nullptr;
    int m_w = 0, m_h = 0;
};

} // namespace mosaicraft
