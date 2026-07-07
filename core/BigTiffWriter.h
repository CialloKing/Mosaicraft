#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <opencv2/imgproc.hpp>

#include <tiffio.h>

namespace mosaicraft
{

// ============================================================
// BigTiffWriter — libtiff 直写，支持超 65500px 尺寸
// ============================================================
class BigTiffWriter
{
public:
    BigTiffWriter(const BigTiffWriter&) = delete;
    BigTiffWriter& operator=(const BigTiffWriter&) = delete;

    BigTiffWriter(const std::string& path, int width, int height)
        : m_w(width), m_h(height)
    {
        int64_t rawSize = static_cast<int64_t>(width) * height * 3;
        const char* mode = (rawSize > 3500LL * 1024 * 1024) ? "w8" : "w";

#ifdef _WIN32
        std::wstring wpath = std::filesystem::u8path(path).wstring();
        m_tif = TIFFOpenW(wpath.c_str(), mode);  // TIFFOpenW: 文件名宽字符，mode 仍为 ANSI
#else
        m_tif = TIFFOpen(path.c_str(), mode);
#endif
        if (!m_tif)
            throw std::runtime_error("BigTiffWriter: cannot open " + path);

        TIFFSetField(m_tif, TIFFTAG_IMAGEWIDTH,      width);
        TIFFSetField(m_tif, TIFFTAG_IMAGELENGTH,     height);
        TIFFSetField(m_tif, TIFFTAG_BITSPERSAMPLE,   8);
        TIFFSetField(m_tif, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(m_tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
        TIFFSetField(m_tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
        TIFFSetField(m_tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
        // DPI 标签（避免查看器按像素长宽比误判）
        TIFFSetField(m_tif, TIFFTAG_XRESOLUTION,    72.0f);
        TIFFSetField(m_tif, TIFFTAG_YRESOLUTION,    72.0f);
        TIFFSetField(m_tif, TIFFTAG_RESOLUTIONUNIT,  RESUNIT_INCH);
        // LZW 需整图单 strip；文件过大时降级为无压缩避免单 strip 内存过高
        if (rawSize < 400LL * 1024 * 1024) {
            TIFFSetField(m_tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
        }
        TIFFSetField(m_tif, TIFFTAG_ROWSPERSTRIP,    height);
    }

    // 流式模式：ROWSPERSTRIP=1，每行独立可写（超大图免全图 Mat）
    BigTiffWriter(const std::string& path, int width, int height, bool streaming)
        : m_w(width), m_h(height)
    {
        int64_t rawSize = static_cast<int64_t>(width) * height * 3;
        const char* mode = (rawSize > 3500LL * 1024 * 1024) ? "w8" : "w";
#ifdef _WIN32
        std::wstring wpath = std::filesystem::u8path(path).wstring();
        m_tif = TIFFOpenW(wpath.c_str(), mode);
#else
        m_tif = TIFFOpen(path.c_str(), mode);
#endif
        if (!m_tif) throw std::runtime_error("BigTiffWriter: cannot open " + path);
        TIFFSetField(m_tif, TIFFTAG_IMAGEWIDTH,      width);
        TIFFSetField(m_tif, TIFFTAG_IMAGELENGTH,     height);
        TIFFSetField(m_tif, TIFFTAG_BITSPERSAMPLE,   8);
        TIFFSetField(m_tif, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(m_tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
        TIFFSetField(m_tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
        TIFFSetField(m_tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
        TIFFSetField(m_tif, TIFFTAG_ROWSPERSTRIP,    1);  // 每行独立 strip：流式可写
    }

    ~BigTiffWriter() { if (m_tif) TIFFClose(m_tif); }

    // 逐行写入（输入为 OpenCV BGR，逐行 cvtColor→RGB 后写 TIFF）
    bool writeMat(const uint8_t* data, int step)
    {
        if (!m_tif || !data) return false;
        for (int y = 0; y < m_h; ++y)
        {
            cv::Mat rowBGR(1, m_w, CV_8UC3,
                           const_cast<uint8_t*>(data + static_cast<size_t>(y) * step));
            cv::Mat rowRGB;
            cv::cvtColor(rowBGR, rowRGB, cv::COLOR_BGR2RGB);
            if (TIFFWriteScanline(m_tif, rowRGB.data, y, 0) < 0)
            {
                std::cerr << "  BigTiffWriter: row " << y << " failed" << std::endl;
                return false;
            }
        }
        return true;
    }

    // 逐行写入 BGR→RGB（流式用，无需完整 cv::Mat）
    bool writeRow(int y, const uint8_t* bgrRow)
    {
        if (!m_tif || !bgrRow || y < 0 || y >= m_h) return false;
        cv::Mat rowRGB;
        cv::Mat rowBGR(1, m_w, CV_8UC3, const_cast<uint8_t*>(bgrRow));
        cv::cvtColor(rowBGR, rowRGB, cv::COLOR_BGR2RGB);
        int rc = TIFFWriteScanline(m_tif, rowRGB.data, y, 0);
        if (rc < 0)
            std::cerr << "BigTiffWriter::writeRow failed at y=" << y << " (w=" << m_w << ")" << std::endl;
        return rc >= 0;
    }

    void close() { if (m_tif) { TIFFClose(m_tif); m_tif = nullptr; } }

private:
    TIFF* m_tif = nullptr;
    int m_w = 0, m_h = 0;
};

} // namespace mosaicraft
