#pragma once

#include "UnicodeIO.h"
#include <list>
#include <future>
#include <functional>
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
    explicit ImageCache(size_t budget = kDefaultBudget, bool benchmark = false)
        : m_budget(budget), m_benchmark(benchmark)
    {
    }

    using Image = std::shared_ptr<const cv::Mat>;
    struct Stats
    {
        size_t hits = 0, decodes = 0, peakBytes = 0, sharedLoads = 0;
    };

    void setUseCounts(std::unordered_map<int, size_t> counts)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remaining = std::move(counts);
        m_managed = true;
    }

    void finishUse(int id, int width, int height)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto remaining = m_remaining.find(id);
        if (remaining != m_remaining.end() && remaining->second > 0 && --remaining->second == 0)
        {
            auto entry = m_entries.find(Key{id, width, height});
            if (entry != m_entries.end())
            {
                erase(entry);
            }
        }
    }

    Stats stats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

    Image getShared(int imageId, const std::string &path, int width, int height)
    {
        return getSharedWithLoader(imageId, width, height, [&]()
        {
            cv::Mat source = imreadUnicode(path, cv::IMREAD_COLOR);
            if (source.empty() || (source.cols == width && source.rows == height))
            {
                return source;
            }
            cv::Mat resized;
            cv::resize(source, resized, cv::Size(width, height), 0, 0, cv::INTER_AREA);
            return resized;
        });
    }

    template <class Loader> Image getSharedWithLoader(int imageId, int width, int height, Loader &&loader)
    {
        const Key key{imageId, width, height};
        std::shared_future<Image> pending;
        std::promise<Image> promise;
        bool owner = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_entries.find(key);
            if (found != m_entries.end())
            {
                m_lru.splice(m_lru.end(), m_lru, found->second.position);
                if (m_benchmark)
                {
                    ++m_stats.hits;
                }
                return found->second.image;
            }
            auto loading = m_loading.find(key);
            if (loading != m_loading.end())
            {
                pending = loading->second;
                if (m_benchmark)
                {
                    ++m_stats.sharedLoads;
                }
            }
            else
            {
                pending = promise.get_future().share();
                m_loading.emplace(key, pending);
                owner = true;
                if (m_benchmark)
                {
                    ++m_stats.decodes;
                }
            }
        }
        if (!owner)
        {
            // 等待和解码都在全局锁外，其他图片可以继续加载。
            return pending.get();
        }
        try
        {
            cv::Mat pixels = loader();
            Image image = pixels.empty() ? Image{} : std::make_shared<const cv::Mat>(std::move(pixels));
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const size_t bytes = image ? image->total() * image->elemSize() : 0;
                const auto uses = m_remaining.find(imageId);
                const bool reusable = !m_managed || (uses != m_remaining.end() && uses->second > 1);
                if (image && bytes <= m_budget && reusable)
                {
                    while (m_bytes > m_budget - bytes && !m_lru.empty())
                    {
                        erase(m_entries.find(m_lru.front()));
                    }
                    m_lru.push_back(key);
                    m_entries.emplace(key, Entry{image, std::prev(m_lru.end()), bytes});
                    m_bytes += bytes;
                    if (m_benchmark)
                    {
                        m_stats.peakBytes = std::max(m_stats.peakBytes, m_bytes);
                    }
                }
                promise.set_value(image);
                m_loading.erase(key);
            }
            return image;
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // 每条完成路径都兑现 promise，失败不能让其他工作线程永久等待。
            promise.set_exception(std::current_exception());
            m_loading.erase(key);
            throw;
        }
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
    bool m_benchmark = false, m_managed = false;
    Stats m_stats;
    std::unordered_map<int, size_t> m_remaining;
    std::unordered_map<Key, std::shared_future<Image>, Hash> m_loading;

    void erase(std::unordered_map<Key, Entry, Hash>::iterator entry)
    {
        m_bytes -= entry->second.bytes;
        m_lru.erase(entry->second.position);
        m_entries.erase(entry);
    }
};

} // namespace mosaicraft
