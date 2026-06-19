#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <opencv2/core.hpp>

namespace mosaicraft
{

// ============================================================
// ImageCache — 线程安全内存缓存
// ============================================================
class ImageCache
{
public:
    cv::Mat getOrLoad(int imageId, const std::string& filePath,
                      int outW, int outH)
    {
        uint64_t key = (static_cast<uint64_t>(imageId) << 32)
                     | (static_cast<uint32_t>(outW) << 16)
                     | static_cast<uint32_t>(outH);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cache.find(key);
            if (it != m_cache.end()) return it->second;
        }

        cv::Mat img = cv::imread(filePath, cv::IMREAD_COLOR);
        if (img.empty()) return img;

        // 归一化图片本身就是 180×320，与输出 tile 同尺寸时跳过 resize
        if (img.cols == outW && img.rows == outH)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cache.find(key);
            if (it != m_cache.end()) return it->second;
            m_cache[key] = img;
            return img;
        }

        cv::Mat resized;
        cv::resize(img, resized, cv::Size(outW, outH), 0, 0, cv::INTER_AREA);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cache.find(key);
            if (it != m_cache.end()) return it->second;
            m_cache[key] = resized;
            return resized;
        }
    }

private:
    std::unordered_map<uint64_t, cv::Mat> m_cache;
    std::mutex m_mutex;
};

} // namespace mosaicraft

