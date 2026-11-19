#include "LibraryPng.h"
#include <png.h>
#include <zlib.h>
#include <array>
#include <cstring>

namespace mosaicraft
{
namespace
{
struct Codec
{
    z_stream in{}, out{};
    ~Codec()
    {
        if (in.state)
        {
            inflateEnd(&in);
        }
        if (out.state)
        {
            deflateEnd(&out);
        }
    }
};
}

void compactLibraryPng(std::vector<uint8_t>& png, std::size_t decodedPixels)
{
    if (png.size() < 33 || png_sig_cmp(png.data(), 0, 8) || png_get_uint_32(png.data() + 8) != 13 ||
        std::memcmp(png.data() + 12, "IHDR", 4) != 0)
    {
        return;
    }
    const uint64_t height = png_get_uint_32(png.data() + 20);
    if (height * png_get_uint_32(png.data() + 16) != decodedPixels)
    {
        return;
    }
    // 只换连续的 IDAT；调色板、透明度、色彩配置、文本以及 APNG 的其他帧均保留原字节。
    size_t first = 0, end = 0;
    for (size_t pos = 8; ; )
    {
        if (png.size() - pos < 12)
        {
            return;
        }
        const size_t size = png_get_uint_32(png.data() + pos);
        const auto type = png.data() + pos + 4;
        if (size > png.size() - pos - 12 || size > PNG_UINT_31_MAX || std::memcmp(type, "dSIG", 4) == 0)
        {
            return; // 不重写签名或结构不完整的源文件。
        }
        if (std::memcmp(type, "IDAT", 4) == 0)
        {
            if ((end && end != pos) || crc32(0, type, static_cast<uInt>(size + 4)) != png_get_uint_32(type + size + 4))
            {
                return;
            }
            first = first ? first : pos;
            end = pos + size + 12;
        }
        if (std::memcmp(type, "IEND", 4) == 0)
        {
            break;
        }
        pos += size + 12;
    }
    if (!first || end - first <= 13 || end - first - 13 > PNG_UINT_31_MAX)
    {
        return;
    }
    // 只保留能严格缩小文件的候选；按块转码滤波后的扫描线，不另外分配整张高位深像素图。
    std::vector<uint8_t> compressed(end - first - 13);
    std::array<uint8_t, 32768> block;
    Codec codec;
    if (inflateInit(&codec.in) != Z_OK || deflateInit(&codec.out, Z_BEST_COMPRESSION) != Z_OK)
    {
        return;
    }
    codec.out.next_out = compressed.data();
    codec.out.avail_out = static_cast<uInt>(compressed.size());
    // 每像素最多 RGBA16 的 8 字节；Adam7 每行至多七次滤波标记和不足一字节的填充。
    const uint64_t limit = 8ULL * decodedPixels + 14 * height;
    size_t pos = first;
    int status;
    do
    {
        while (!codec.in.avail_in && pos < end)
        {
            codec.in.avail_in = png_get_uint_32(png.data() + pos);
            codec.in.next_in = png.data() + pos + 8;
            pos += 12 + static_cast<size_t>(codec.in.avail_in);
        }
        codec.in.next_out = block.data();
        codec.in.avail_out = static_cast<uInt>(block.size());
        status = inflate(&codec.in, Z_NO_FLUSH);
        if ((status != Z_OK && status != Z_STREAM_END) || codec.in.total_out > limit)
        {
            return;
        }
        codec.out.next_in = block.data();
        codec.out.avail_in = static_cast<uInt>(block.size()) - codec.in.avail_out;
        if (!codec.out.avail_in && status != Z_STREAM_END)
        {
            continue; // 小 IDAT 可能只包含部分 zlib 头，尚未解出扫描线。
        }
        const int result = deflate(&codec.out, status == Z_STREAM_END ? Z_FINISH : Z_NO_FLUSH);
        if (result != (status == Z_STREAM_END ? Z_STREAM_END : Z_OK) || codec.out.avail_in)
        {
            return;
        }
    } while (status != Z_STREAM_END);

    const auto size = static_cast<png_uint_32>(codec.out.total_out);
    auto chunk = png.data() + first;
    png_save_uint_32(chunk, size);
    std::memcpy(chunk + 8, compressed.data(), size);
    png_save_uint_32(chunk + 8 + size, crc32(0, chunk + 4, size + 4));
    png.erase(png.begin() + first + size + 12, png.begin() + end);
}
} // namespace mosaicraft
