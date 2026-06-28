#pragma once

#include "Database.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace mosaicraft
{

// Unicode-safe fopen (Windows: UTF-8 → wchar)
inline FILE* u8fopen(const std::string& path, const char* mode)
{
#ifdef _WIN32
    std::wstring wp = std::filesystem::u8path(path).wstring();
    std::wstring wm(mode, mode + strlen(mode));
    return _wfopen(wp.c_str(), wm.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

// ============================================================
// FeaturePack — 二进制特征缓存 (v2)
//
// 将 50K 个小文件（.tiny + .hist）合并为 2 个二进制文件。
//
// 文件格式（小端序）：
//   tiny.bin: [uint32_t count][(int32_t id + 256B uint8_t) × count]
//   lbp.bin:  [uint32_t count][(int32_t id + 1024B float) × count]
//
// 特征按递增 image_id 顺序存储，每条记录显式包含 image_id
// 以处理 ID 不连续（重复图片 INSERT OR IGNORE 跳过）的情况。
// ============================================================
class FeaturePack
{
public:
    // ——— 写入 ———

    static bool beginWrite(const std::string& featDir, int totalCount)
    {
        std::string tinyPath = featDir + "/tiny.bin";
        std::string lbpPath  = featDir + "/lbp.bin";

        s_tinyFile = u8fopen(tinyPath, "wb");
        s_lbpFile  = u8fopen(lbpPath, "wb");
        if (!s_tinyFile || !s_lbpFile)
        {
            if (s_tinyFile) { fclose(s_tinyFile); s_tinyFile = nullptr; }
            if (s_lbpFile)  { fclose(s_lbpFile);  s_lbpFile  = nullptr; }
            return false;
        }

        uint32_t count = static_cast<uint32_t>(totalCount);
        fwrite(&count, sizeof(count), 1, s_tinyFile);
        fwrite(&count, sizeof(count), 1, s_lbpFile);
        return true;
    }

    // 追加一张图：先写 image_id，再写特征数据
    static void appendImage(int imageId,
                            const std::vector<uint8_t>& tiny,
                            const std::vector<float>& lbp)
    {
        if (!s_tinyFile || !s_lbpFile) return;
        int32_t id = static_cast<int32_t>(imageId);
        // tiny: id(4B) + 256B uint8_t
        fwrite(&id, sizeof(id), 1, s_tinyFile);
        fwrite(tiny.data(), 1, 256, s_tinyFile);
        // lbp:  id(4B) + 256 float = 1024B
        fwrite(&id, sizeof(id), 1, s_lbpFile);
        fwrite(lbp.data(), sizeof(float), 256, s_lbpFile);
    }

    static void endWrite()
    {
        if (s_tinyFile) { fclose(s_tinyFile); s_tinyFile = nullptr; }
        if (s_lbpFile)  { fclose(s_lbpFile);  s_lbpFile  = nullptr; }
    }

    // ——— 构建缓存（build 结束时调用） ———
    static bool buildCache(const std::string& featDir,
                           const std::vector<ImageRecord>& records)
    {
        std::vector<const ImageRecord*> sorted;
        sorted.reserve(records.size());
        for (const auto& r : records)
            sorted.push_back(&r);
        std::sort(sorted.begin(), sorted.end(),
            [](const ImageRecord* a, const ImageRecord* b) {
                return a->id < b->id;
            });

        // 先读取旧缓存（在 beginWrite 截断之前！）
        std::vector<uint8_t> oldTiny;
        std::vector<float>   oldLbp;
        std::vector<int>     oldIds;
        if (!featDir.empty())
        {
            std::string tp = featDir + "/tiny.bin";
            std::string lp = featDir + "/lbp.bin";
            FILE* ft = u8fopen(tp, "rb");
            FILE* fl = u8fopen(lp, "rb");
            if (ft && fl) {
                uint32_t tc = 0, lc = 0;
                if (fread(&tc,4,1,ft)==1 && fread(&lc,4,1,fl)==1 && tc==lc) {
                    int oldN = static_cast<int>(tc);
                    oldTiny.resize(oldN * 256);
                    oldLbp.resize(oldN * 256);
                    oldIds.resize(oldN);
                    for (int i = 0; i < oldN; ++i) {
                        int32_t tid=0, lid=0;
                        if (fread(&tid,4,1,ft)!=1 || fread(&oldTiny[i*256],1,256,ft)!=256 ||
                            fread(&lid,4,1,fl)!=1 || fread(&oldLbp[i*256],4,256,fl)!=256 || tid!=lid)
                            { oldIds.clear(); break; }
                        oldIds[i] = static_cast<int>(tid);
                    }
                }
            }
            if (ft) fclose(ft);
            if (fl) fclose(fl);
        }
        std::unordered_map<int,int> oldPos;
        for (int i = 0; i < static_cast<int>(oldIds.size()); ++i)
            oldPos[oldIds[i]] = i;

        if (!beginWrite(featDir, static_cast<int>(sorted.size())))
            return false;

        // Parallel read: multiple threads read tiny/lbp files concurrently
        int nTotal = static_cast<int>(sorted.size());
        int nThr = static_cast<int>(std::thread::hardware_concurrency());
        if (nThr < 2) nThr = 2; if (nThr > 16) nThr = 16;
        std::vector<std::vector<uint8_t>> allTiny(nTotal);
        std::vector<std::vector<float>>   allLbp(nTotal);
        std::atomic<int> nextIdx{0};
        std::vector<std::thread> readers;
        for (int t = 0; t < nThr; ++t) {
            readers.emplace_back([&]() {
                for (int i = nextIdx++; i < nTotal; i = nextIdx++) {
                    const auto* rec = sorted[i];
                    auto op = oldPos.find(rec->id);
                    if (op != oldPos.end()) continue;
                    std::vector<uint8_t> tiny(256, 0);
                    if (!rec->tinyPath.empty()) {
                        FILE* f = u8fopen(rec->tinyPath.c_str(), "rb");
                        if (f) { fread(tiny.data(), 1, 256, f); fclose(f); }
                    }
                    std::vector<float> lbp(256, 0.0f);
                    if (!rec->histPath.empty()) {
                        FILE* f = u8fopen(rec->histPath.c_str(), "rb");
                        if (f) { fread(lbp.data(), sizeof(float), 256, f); fclose(f); }
                    }
                    allTiny[i] = std::move(tiny);
                    allLbp[i]  = std::move(lbp);
                }
            });
        }
        for (auto& w : readers) w.join();

        for (int i = 0; i < nTotal; ++i) {
            const auto* rec = sorted[i];
            auto op = oldPos.find(rec->id);
            if (op != oldPos.end()) {
                int o = op->second;
                appendImage(rec->id,
                    std::vector<uint8_t>(oldTiny.begin()+o*256, oldTiny.begin()+(o+1)*256),
                    std::vector<float>(oldLbp.begin()+o*256, oldLbp.begin()+(o+1)*256));
            } else {
                appendImage(rec->id, allTiny[i], allLbp[i]);
            }
        }

        endWrite();
        return true;
    }

    // ——— 加载缓存（mosaic 阶段调用） ———
    // recordIds: allRecords[i].id（allRecords 按 use_count 排序）
    // h_tiny/h_lbp: 输出，按 allRecords 索引顺序填充
    static bool tryLoad(const std::string& featDir,
                        const std::vector<int>& recordIds,
                        std::vector<uint8_t>& h_tiny,
                        std::vector<float>& h_lbp)
    {
        std::string tinyPath = featDir + "/tiny.bin";
        std::string lbpPath  = featDir + "/lbp.bin";

        FILE* ft = u8fopen(tinyPath, "rb");
        if (!ft) return false;
        FILE* fl = u8fopen(lbpPath, "rb");
        if (!fl) { fclose(ft); return false; }
        // 大缓冲区加速顺序读取（45K条记录 ~11MB tiny + ~45MB lbp）
        constexpr size_t BIG_BUF = 2 * 1024 * 1024;
        setvbuf(ft, nullptr, _IOFBF, BIG_BUF);
        setvbuf(fl, nullptr, _IOFBF, BIG_BUF);

        // 校验计数
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
            fclose(ft); fclose(fl);
            return false;
        }

        // 逐条读取，建立 id → offset 映射
        // 每条记录: [int32_t id][256B tiny] / [int32_t id][1024B lbp]
        std::vector<uint8_t> fileTiny(N * 256);
        std::vector<float>   fileLbp(N * 256);
        std::vector<int>     fileIds(N);

        for (int i = 0; i < N; ++i)
        {
            int32_t tid = 0, lid = 0;
            if (fread(&tid, sizeof(tid), 1, ft) != 1 ||
                fread(&fileTiny[i * 256], 1, 256, ft) != 256 ||
                fread(&lid, sizeof(lid), 1, fl) != 1 ||
                fread(&fileLbp[i * 256], sizeof(float), 256, fl) != 256)
            {
                fclose(ft); fclose(fl);
                return false;
            }
            if (tid != lid)
            {
                // tiny.bin 和 lbp.bin 中同一条记录的 id 不一致
                fclose(ft); fclose(fl);
                return false;
            }
            fileIds[i] = static_cast<int>(tid);
        }
        fclose(ft); fclose(fl);

        // 建立 image_id → binary_index 映射
        int maxId = 0;
        for (int id : recordIds)
            if (id > maxId) maxId = id;
        for (int id : fileIds)
            if (id > maxId) maxId = id;

        std::vector<int> idToOffset(maxId + 1, -1);
        for (int i = 0; i < N; ++i)
            idToOffset[fileIds[i]] = i;

        // 按 recordIds 顺序填充输出数组
        h_tiny.resize(dbCount * 256);
        h_lbp.resize(dbCount * 256);
        for (int i = 0; i < dbCount; ++i)
        {
            int imgId = recordIds[i];
            int offset = (imgId >= 0 && imgId <= maxId) ? idToOffset[imgId] : -1;
            if (offset >= 0)
            {
                std::memcpy(&h_tiny[i * 256], &fileTiny[offset * 256], 256);
                std::memcpy(&h_lbp[i * 256],  &fileLbp[offset * 256],  256 * sizeof(float));
            }
            else
            {
                std::memset(&h_tiny[i * 256], 0, 256);
                std::memset(&h_lbp[i * 256],  0, 256 * sizeof(float));
            }
        }

        return true;
    }

private:
    inline static FILE* s_tinyFile = nullptr;
    inline static FILE* s_lbpFile  = nullptr;
};

} // namespace mosaicraft
