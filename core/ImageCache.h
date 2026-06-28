#pragma once

#include <array>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <opencv2/core.hpp>
#include "UnicodeIO.h"

namespace mosaicraft
{

// ============================================================
// ImageCache — 分片线程安全 LRU 内存缓存（16 桶）
// ============================================================
class ImageCache
{
    static constexpr int kShards = 16;
    static constexpr size_t kDefaultMaxPerShard = 1024;  // 每分片最多缓存张数

    struct Shard {
        std::mutex mtx;
        // key → (Mat, LRU iterator)
        std::unordered_map<uint64_t, std::pair<cv::Mat, std::list<uint64_t>::iterator>> map;
        std::list<uint64_t> lru;  // front = least recent, back = most recent
        size_t maxEntries = kDefaultMaxPerShard;
    };

public:
    explicit ImageCache(size_t maxTotal = kDefaultMaxPerShard * kShards)
    {
        size_t perShard = std::max(size_t(1), maxTotal / kShards);
        for (auto& s : m_shards) s.maxEntries = perShard;
    }

    cv::Mat getOrLoad(int imageId, const std::string& filePath,
                      int outW, int outH)
    {
        uint64_t key = (static_cast<uint64_t>(imageId) << 32)
                     | (static_cast<uint32_t>(outW) << 16)
                     | static_cast<uint32_t>(outH);
        int si = (imageId >= 0) ? (imageId & (kShards - 1))
                                : ((-imageId) & (kShards - 1));  // 安全处理负 id
        auto& s = m_shards[si];

        {
            std::lock_guard<std::mutex> lock(s.mtx);
            auto it = s.map.find(key);
            if (it != s.map.end()) {
                // 命中：移到 LRU 尾部（最近使用）
                s.lru.splice(s.lru.end(), s.lru, it->second.second);
                return it->second.first.clone();
            }
        }

        cv::Mat img = imreadUnicode(filePath, cv::IMREAD_COLOR);
        if (img.empty()) return img;

        cv::Mat toCache;
        if (img.cols == outW && img.rows == outH)
            toCache = img;
        else
            cv::resize(img, toCache, cv::Size(outW, outH), 0, 0, cv::INTER_AREA);

        {
            std::lock_guard<std::mutex> lock(s.mtx);
            // 双重检查
            auto it = s.map.find(key);
            if (it != s.map.end()) {
                s.lru.splice(s.lru.end(), s.lru, it->second.second);
                return it->second.first.clone();
            }

            // LRU 淘汰
            if (s.map.size() >= s.maxEntries) {
                uint64_t oldKey = s.lru.front();
                s.lru.pop_front();
                s.map.erase(oldKey);
            }

            // 插入
            s.lru.push_back(key);
            auto lruIt = std::prev(s.lru.end());
            s.map[key] = {toCache, lruIt};
            return toCache.clone();
        }
    }

private:
    std::array<Shard, kShards> m_shards;
};

} // namespace mosaicraft
