#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <opencv2/core.hpp>

namespace mosaicraft
{

// ============================================================
// ImageCache — 分片线程安全内存缓存（16 桶）
// ============================================================
class ImageCache
{
    static constexpr int kShards = 16;

    struct Shard {
        std::mutex mtx;
        std::unordered_map<uint64_t, cv::Mat> map;
    };

public:
    cv::Mat getOrLoad(int imageId, const std::string& filePath,
                      int outW, int outH)
    {
        uint64_t key = (static_cast<uint64_t>(imageId) << 32)
                     | (static_cast<uint32_t>(outW) << 16)
                     | static_cast<uint32_t>(outH);
        int s = imageId & (kShards - 1);  // imageId % 16

        {
            std::lock_guard<std::mutex> lock(m_shards[s].mtx);
            auto it = m_shards[s].map.find(key);
            if (it != m_shards[s].map.end()) return it->second;
        }

        cv::Mat img = cv::imread(filePath, cv::IMREAD_COLOR);
        if (img.empty()) return img;

        // 归一化图片本身 180×320，同尺寸跳过 resize
        if (img.cols == outW && img.rows == outH)
        {
            std::lock_guard<std::mutex> lock(m_shards[s].mtx);
            auto it = m_shards[s].map.find(key);
            if (it != m_shards[s].map.end()) return it->second;
            m_shards[s].map[key] = img;
            return img;
        }

        cv::Mat resized;
        cv::resize(img, resized, cv::Size(outW, outH), 0, 0, cv::INTER_AREA);

        {
            std::lock_guard<std::mutex> lock(m_shards[s].mtx);
            auto it = m_shards[s].map.find(key);
            if (it != m_shards[s].map.end()) return it->second;
            m_shards[s].map[key] = resized;
            return resized;
        }
    }

private:
    std::array<Shard, kShards> m_shards;
};

} // namespace mosaicraft
