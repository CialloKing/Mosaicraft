#pragma once

#include "UnicodeIO.h"
#include <list>
#include <memory>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <unordered_map>

namespace mosaicraft
{

class ImageCache
{
    struct Key
    {
        int id, width, height;
        bool operator==(const Key &other) const
        {
            return id == other.id && width == other.width && height == other.height;
        }
    };
    struct Hash
    {
        size_t operator()(const Key &key) const
        {
            size_t hash = std::hash<int>{}(key.id);
            hash ^= std::hash<int>{}(key.width) + (hash << 6) + (hash >> 2);
            return hash ^ (std::hash<int>{}(key.height) + (hash << 6) + (hash >> 2));
        }
    };
    struct Entry
    {
        std::shared_ptr<const cv::Mat> image;
        std::list<Key>::iterator position;
        size_t bytes;
    };

  public:
    static constexpr size_t kDefaultBudget = 512ULL * 1024 * 1024;
    explicit ImageCache(size_t budget = kDefaultBudget) : m_budget(budget)
    {
    }

    std::shared_ptr<const cv::Mat> getShared(int imageId, const std::string &path, int width, int height)
    {
        const Key key{imageId, width, height};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_entries.find(key);
            if (found != m_entries.end())
            {
                m_lru.splice(m_lru.end(), m_lru, found->second.position);
                return found->second.image;
            }
        }
        // 读盘、解码、缩放均在锁外；共享只读引用在淘汰后仍有效。
        cv::Mat source = imreadUnicode(path, cv::IMREAD_COLOR);
        if (source.empty())
        {
            return {};
        }
        cv::Mat resized;
        if (source.cols == width && source.rows == height)
        {
            resized = std::move(source);
        }
        else
        {
            cv::resize(source, resized, cv::Size(width, height), 0, 0, cv::INTER_AREA);
        }
        const size_t bytes = resized.total() * resized.elemSize();
        auto image = std::make_shared<const cv::Mat>(std::move(resized));
        if (bytes > m_budget)
        {
            return image;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_entries.find(key);
        if (found != m_entries.end())
        {
            m_lru.splice(m_lru.end(), m_lru, found->second.position);
            return found->second.image;
        }
        while (m_bytes > m_budget - bytes && !m_lru.empty())
        {
            auto oldest = m_entries.find(m_lru.front());
            m_bytes -= oldest->second.bytes;
            m_entries.erase(oldest);
            m_lru.pop_front();
        }
        m_lru.push_back(key);
        m_entries.emplace(key, Entry{image, std::prev(m_lru.end()), bytes});
        m_bytes += bytes;
        return image;
    }

    // 兼容需要修改像素的调用方；复制发生在锁外。
    cv::Mat getOrLoad(int imageId, const std::string &path, int width, int height)
    {
        auto image = getShared(imageId, path, width, height);
        return image ? image->clone() : cv::Mat{};
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_lru.clear();
        m_bytes = 0;
    }

    size_t cachedBytes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_bytes;
    }

  private:
    mutable std::mutex m_mutex;
    std::unordered_map<Key, Entry, Hash> m_entries;
    std::list<Key> m_lru;
    size_t m_budget;
    size_t m_bytes = 0;
};

} // namespace mosaicraft
