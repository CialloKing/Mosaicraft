#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

// libtiff C API — 已在 vcpkg 中（OpenCV 依赖）
#include <tiffio.h>

namespace mosaicraft
{

// ============================================================
// BigTiffWriter — 通过 libtiff 直写超大 TIFF
//
// OpenCV cv::imwrite 所有格式限 65500px。
// libtiff BigTIFF 模式 ("w8") 支持任意尺寸（仅受内存限制）。
// ============================================================
class BigTiffWriter
{
public:
    BigTiffWriter(const std::string& path, int width, int height)
        : m_w(width), m_h(height)
    {
        m_tif = TIFFOpen(path.c_str(), "w8");
        if (!m_tif)
            throw std::runtime_error("BigTiffWriter: cannot open " + path);

        TIFFSetField(m_tif, TIFFTAG_IMAGEWIDTH,      width);
        TIFFSetField(m_tif, TIFFTAG_IMAGELENGTH,     height);
        TIFFSetField(m_tif, TIFFTAG_BITSPERSAMPLE,   8);
        TIFFSetField(m_tif, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(m_tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
        TIFFSetField(m_tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
        // 数据为 BGR（OpenCV 默认），标注为 RGB 以兼容查看器
        TIFFSetField(m_tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
        TIFFSetField(m_tif, TIFFTAG_COMPRESSION,     COMPRESSION_LZW);
        // 每 256 行一个 strip，平衡压缩率与内存
        TIFFSetField(m_tif, TIFFTAG_ROWSPERSTRIP,    256);
    }

    ~BigTiffWriter()
    {
        if (m_tif) TIFFClose(m_tif);
    }

    // 写入整张图像（data 指向 cv::Mat 首地址，step 为每行字节数 = cols * 3）
    // OpenCV Mat 为 BGR 交错格式，逐行连续写入
    bool writeMat(const uint8_t* data, int step)
    {
        // 逐行写入到单个 strip
        for (int y = 0; y < m_h; ++y)
        {
            if (TIFFWriteScanline(m_tif, const_cast<uint8_t*>(data + y * step), y, 0) < 0)
                return false;
        }
        return true;
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
};

} // namespace mosaicraft
