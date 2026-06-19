#pragma once

#include "Database.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mosaicraft
{

// ============================================================
// FeaturePack — 二进制特征缓存
//
// 将 50K 个小文件（.tiny + .hist）合并为 2 个二进制文件，
// 消除 open()/close() 文件句柄风暴，将 Prep 阶段从 ~1400ms 降至 ~200ms。
//
// 文件格式（小端序）：
//   tiny.bin: [uint32_t count][count × 256B uint8_t]
//   lbp.bin:  [uint32_t count][count × 1024B float]
//
// 特征按递增 image_id 顺序存储。
// 加载时按 allRecords() 顺序重排（因为 allRecords 按 use_count 排序）。
// ============================================================
class FeaturePack
{
public:
    // ——— 写入（build 阶段调用） ———

    // 打开缓存文件准备写入，返回 true 表示成功
    static bool beginWrite(const std::string& featDir, int totalCount)
    {
        std::string tinyPath = featDir + "/tiny.bin";
        std::string lbpPath  = featDir + "/lbp.bin";

        s_tinyFile = fopen(tinyPath.c_str(), "wb");
        s_lbpFile  = fopen(lbpPath.c_str(), "wb");
        if (!s_tinyFile || !s_lbpFile)
        {
            if (s_tinyFile) { fclose(s_tinyFile); s_tinyFile = nullptr; }
            if (s_lbpFile)  { fclose(s_lbpFile);  s_lbpFile  = nullptr; }
            return false;
        }

        // 写入文件头：图片总数
        uint32_t count = static_cast<uint32_t>(totalCount);
        fwrite(&count, sizeof(count), 1, s_tinyFile);
        fwrite(&count, sizeof(count), 1, s_lbpFile);
        s_writtenCount = totalCount;
        return true;
    }

    // 追加一张图片的特征（按 image_id 递增顺序调用）
    static void appendImage(const std::vector<uint8_t>& tiny,
                            const std::vector<float>& lbp)
    {
        if (!s_tinyFile || !s_lbpFile) return;
        // tiny: 256 字节 uint8_t
        fwrite(tiny.data(), 1, 256, s_tinyFile);
        // lbp: 256 × float = 1024 字节
        fwrite(lbp.data(), sizeof(float), 256, s_lbpFile);
    }

    // 关闭写入
    static void endWrite()
    {
        if (s_tinyFile) { fclose(s_tinyFile); s_tinyFile = nullptr; }
        if (s_lbpFile)  { fclose(s_lbpFile);  s_lbpFile  = nullptr; }
    }

    // ——— 读取（mosaic 阶段调用） ———

    // 从数据库记录构建缓存（build 结束时调用）
    // 读取所有 tiny/lbp 文件，按 image_id 升序写入二进制缓存
    static bool buildCache(const std::string& featDir,
                           const std::vector<ImageRecord>& records)
    {
        // 按 image_id 排序
        std::vector<const ImageRecord*> sorted;
        sorted.reserve(records.size());
        for (const auto& r : records)
            sorted.push_back(&r);
        std::sort(sorted.begin(), sorted.end(),
            [](const ImageRecord* a, const ImageRecord* b) {
                return a->id < b->id;
            });

        if (!beginWrite(featDir, static_cast<int>(sorted.size())))
            return false;

        for (const auto* rec : sorted)
        {
            // 读取 tiny 文件（256 字节 uint8_t）
            std::vector<uint8_t> tiny(256, 0);
            if (!rec->tinyPath.empty())
            {
                FILE* f = fopen(rec->tinyPath.c_str(), "rb");
                if (f)
                {
                    fread(tiny.data(), 1, 256, f);
                    fclose(f);
                }
            }

            // 读取 lbp 文件（256 个 float = 1024 字节）
            std::vector<float> lbp(256, 0.0f);
            if (!rec->histPath.empty())
            {
                FILE* f = fopen(rec->histPath.c_str(), "rb");
                if (f)
                {
                    fread(lbp.data(), sizeof(float), 256, f);
                    fclose(f);
                }
            }

            appendImage(tiny, lbp);
        }

        endWrite();
        return true;
    }

    // 尝试加载缓存：成功返回 true，h_tiny/h_lbp 按 allRecords 索引顺序填充
    // allRecords 需按 image_id 升序传入以建立 ID→offset 映射
    // 如果不按 ID 排序，需要传入 idOrder 来说明 allRecords[i] 的 image_id
    static bool tryLoad(const std::string& featDir,
                        const std::vector<int>& recordIds,  // allRecords[i].id
                        std::vector<uint8_t>& h_tiny,
                        std::vector<float>& h_lbp)
    {
        std::string tinyPath = featDir + "/tiny.bin";
        std::string lbpPath  = featDir + "/lbp.bin";

        FILE* ft = fopen(tinyPath.c_str(), "rb");
        if (!ft) return false;
        FILE* fl = fopen(lbpPath.c_str(), "rb");
        if (!fl) { fclose(ft); return false; }

        // 读取并校验计数
        uint32_t tinyCount = 0, lbpCount = 0;
        if (fread(&tinyCount, sizeof(tinyCount), 1, ft) != 1 ||
            fread(&lbpCount, sizeof(lbpCount), 1, fl)   != 1 ||
            tinyCount != lbpCount)
        {
            fclose(ft); fclose(fl);
            return false;
        }

        int N = static_cast<int>(tinyCount);
        int dbCount = static_cast<int>(recordIds.size());
        if (N != dbCount)
        {
            // 缓存与数据库不同步
            fclose(ft); fclose(fl);
            return false;
        }

        // 一次性读取全部数据（仅 2 次 fread 替代 50K 次 open/read/close）
        // tiny: N × 256 字节
        std::vector<uint8_t> fileTiny(N * 256);
        // lbp: N × 256 × sizeof(float) = N × 1024 字节
        std::vector<float> fileLbp(N * 256);

        size_t tr = fread(fileTiny.data(), 1, N * 256, ft);
        size_t lr = fread(fileLbp.data(), sizeof(float), N * 256, fl);
        fclose(ft); fclose(fl);

        if (tr != static_cast<size_t>(N * 256) ||
            lr != static_cast<size_t>(N * 256))
        {
            return false;
        }

        // 建立 recordIds[i] → binary_offset 的映射
        // binary 文件按 image_id 递增存储（id 1 在 offset 0, id 2 在 offset 1, ...）
        // 注意：image_id 从 1 开始，binary offset 从 0 开始
        // 需要找到最大 id 以分配映射数组
        int maxId = 0;
        for (int id : recordIds)
            if (id > maxId) maxId = id;

        std::vector<int> idToOffset(maxId + 1, -1);
        for (int i = 0; i < N; ++i)
        {
            // binary 文件中的第 i 个特征对应 image_id = i + 1
            int imgId = i + 1;
            if (imgId <= maxId)
                idToOffset[imgId] = i;
        }

        // 按 recordIds 顺序填充输出数组（匹配 allRecords 的 use_count 排序）
        h_tiny.resize(dbCount * 256);
        h_lbp.resize(dbCount * 256);
        for (int i = 0; i < dbCount; ++i)
        {
            int imgId = recordIds[i];
            int offset = (imgId > 0 && imgId <= maxId) ? idToOffset[imgId] : -1;
            if (offset >= 0)
            {
                std::memcpy(&h_tiny[i * 256], &fileTiny[offset * 256], 256);
                std::memcpy(&h_lbp[i * 256],  &fileLbp[offset * 256],  256 * sizeof(float));
            }
            else
            {
                // 缓存中无此 ID（不应发生），清零
                std::memset(&h_tiny[i * 256], 0, 256);
                std::memset(&h_lbp[i * 256],  0, 256 * sizeof(float));
            }
        }

        return true;
    }

private:
    inline static FILE* s_tinyFile = nullptr;
    inline static FILE* s_lbpFile  = nullptr;
    inline static int   s_writtenCount = 0;
};

} // namespace mosaicraft
