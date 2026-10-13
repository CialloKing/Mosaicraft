#pragma once

#include "Database.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mosaicraft
{
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
    // 仅由顺序选图线程调用；返回的行引用在下一次 get/prepare 前有效。
    class Reader
    {
      public:
        struct Row
        {
            std::array<uint8_t, 256> tiny{};
            std::array<float, 256> lbp{};
            bool tinyValid = false, lbpValid = false;
        };
        struct ReadStats
        {
            size_t hits = 0, misses = 0, sourceReads = 0, packedRows = 0, evictions = 0, peakBytes = 0;
            bool preloaded = false;
        };
        Reader(const std::string &directory, const std::vector<ImageRecord> &records,
               size_t budget = 256ULL * 1024 * 1024);
        ~Reader();
        Reader(const Reader &) = delete;
        Reader &operator=(const Reader &) = delete;
        void prepare(const std::vector<int> &candidates);
        const Row &get(int index);
        const ReadStats &stats() const;

      private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
    static bool load(const std::string &directory, const std::vector<ImageRecord> &records,
                     const std::vector<int> &needed, std::vector<uint8_t> &tiny, std::vector<float> &lbp,
                     std::vector<uint8_t> &tinyValid, std::vector<uint8_t> &lbpValid);
    static bool buildCache(const std::string &directory, const std::vector<ImageRecord> &records,
                           Stats *stats = nullptr);
    // Only the exclusive build lock may remove generations that another reader could still use.
    static void removeObsolete(const std::string &directory);
};
} // namespace mosaicraft
