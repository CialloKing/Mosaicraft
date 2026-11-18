#pragma once
#include <optional>

#include "Database.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mosaicraft
{
class RuntimeContext;
// Cache is an acceleration layer; missing source features retain their validity flags.
class FeaturePack
{
  public:
    struct Stats
    {
        bool reused = false;
        double validationMs = 0;
        double writingMs = 0;
    };
    // get/prepare 由单个线程调用；并行读取仅限已冻结批次的 preparedRow。
    class Reader
    {
      public:
        struct Row
        {
            std::array<uint8_t, 256> tiny{};
            std::array<float, 256> lbp{};
            bool tinyValid = false, lbpValid = false;
        };
        // 只缓存已通过行校验的包，不缓存使用次数、来源检查结果或当前 SQL 行序。
        struct Session
        {
            std::string directory, generation;
            uint64_t metadata = 0, tiny = 0, lbp = 0;
            std::vector<Row> rows;
        };
        struct ReadStats
        {
            size_t hits = 0, misses = 0, sourceReads = 0, packedRows = 0, evictions = 0, peakBytes = 0;
            size_t temporaryPeakBytes = 0;
            bool preloaded = false, sessionHit = false;
        };
        Reader(const std::string &directory, const std::vector<ImageRecord> &records,
               size_t budget = 256ULL * 1024 * 1024, RuntimeContext *runtime = nullptr, Session *session = nullptr);
        ~Reader();
        Reader(const Reader &) = delete;
        Reader &operator=(const Reader &) = delete;
        void prepare(const std::vector<int> &candidates);
        const Row &get(int index);
        // prepare 后直到下次非 const 操作为止可并行访问，不加载、不移动 LRU 节点。
        const Row *preparedRow(int index) const;
        const ReadStats &stats() const;

      private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
    static bool load(const std::string &directory, const std::vector<ImageRecord> &records,
                     const std::vector<int> &needed, std::vector<uint8_t> &tiny, std::vector<float> &lbp,
                     std::vector<uint8_t> &tinyValid, std::vector<uint8_t> &lbpValid,
                     Reader::ReadStats *readStats = nullptr, unsigned workers = 0);
    static bool buildCache(const std::string &directory, const std::vector<ImageRecord> &records,
                           Stats *stats = nullptr, unsigned workers = 0);
    static std::optional<uint64_t> sourceVersion(const std::string &directory,
                                                  const std::vector<ImageRecord> &records);
    // Only the exclusive build lock may remove generations that another reader could still use.
    static void removeObsolete(const std::string &directory);
};
} // namespace mosaicraft
