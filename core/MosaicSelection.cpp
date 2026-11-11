#include "MosaicSelection.h"
#include "Database.h"
#include "NeighborhoodState.h"
#include "UnicodeIO.h"

#include <atomic>
#include <list>
#include <mutex>

namespace mosaicraft
{
namespace
{
constexpr size_t scoreBudget = 128ULL * 1024 * 1024;
std::atomic_size_t reservedScoreBytes{0};

class Fingerprint
{
    uint64_t m_hash = 14695981039346656037ULL;

  public:
    void add(const void *data, size_t size)
    {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i)
        {
            m_hash ^= bytes[i];
            m_hash *= 1099511628211ULL;
        }
    }

    template <typename T> void add(const T &value)
    {
        add(&value, sizeof(value));
    }

    void add(const std::string &value)
    {
        add(value.size());
        add(value.data(), value.size());
    }

    uint64_t value() const
    {
        return m_hash;
    }
};

PreviewAsset assetVersion(const std::string &path, const std::string &hash)
{
    PreviewAsset asset{path, hash};
    std::error_code error;
    asset.bytes = std::filesystem::file_size(u8path(path), error);
    if (!error)
    {
        asset.modified = std::filesystem::last_write_time(u8path(path), error);
    }
    asset.available = !error;
    return asset;
}
} // namespace

PreviewScores::PreviewScores() = default;

PreviewScores::~PreviewScores()
{
    reservedScoreBytes.fetch_sub(reservedBytes, std::memory_order_relaxed);
}

std::shared_ptr<PreviewScores> PreviewScores::create(size_t tiles, int candidates,
                                                     const std::vector<ImageRecord> &records)
{
    const size_t cells = tiles * candidates;
    const size_t bytes = sizeof(PreviewScores) + cells * (sizeof(int) + sizeof(double)) +
                         records.size() * (sizeof(ImageRecord) + 192 * sizeof(float));
    if (bytes > scoreBudget)
    {
        return {};
    }
    auto scores = std::make_shared<PreviewScores>();
    size_t used = reservedScoreBytes.load(std::memory_order_relaxed);
    do
    {
        if (bytes > scoreBudget - used)
        {
            return {};
        }
    } while (!reservedScoreBytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));

    // 完成的预览和工作区可能同时持有数据，最后一个所有者释放预算。
    scores->reservedBytes = bytes;
    scores->candidates = candidates;
    scores->indices.resize(cells);
    scores->values.resize(cells);
    scores->records.resize(records.size());
    for (size_t i = 0; i < records.size(); ++i)
    {
        scores->records[i].id = records[i].id;
        scores->records[i].gridLab = records[i].gridLab;
    }
    return scores;
}

class PreviewRecommendationCache
{
    using Row = std::array<int, MosaicSelection::recommendationLimit>;
    std::shared_ptr<const PreviewScores> m_scores;
    int m_window;
    double m_penalty;
    std::mutex m_mutex;
    std::list<std::pair<size_t, Row>> m_recent;

  public:
    PreviewRecommendationCache(std::shared_ptr<const PreviewScores> scores, int window, double penalty)
        : m_scores(std::move(scores)), m_window(window), m_penalty(penalty)
    {
    }

    Row get(const MosaicSelection &selection, size_t position)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto hit = std::find_if(m_recent.begin(), m_recent.end(), [position](const auto &entry)
        {
            return entry.first == position;
        });
        if (hit != m_recent.end())
        {
            m_recent.splice(m_recent.begin(), m_recent, hit);
            return hit->second;
        }

        const auto &scores = *m_scores;
        detail::NeighborhoodState neighborhood(scores.records, selection.columns, m_window, m_penalty);
        const int tile = static_cast<int>(position);
        neighborhood.replay(selection.indices, tile);
        const size_t start = position * scores.candidates;
        std::vector<std::pair<double, int>> ranked;
        ranked.reserve(scores.candidates);
        for (int candidate = 0; candidate < scores.candidates; ++candidate)
        {
            const int index = scores.indices[start + candidate];
            if (index >= 0)
            {
                double value = scores.values[start + candidate];
                neighborhood.applyPenalty(value, index, tile);
                ranked.emplace_back(value, index);
            }
        }
        const size_t count = std::min(ranked.size(), size_t(MosaicSelection::recommendationLimit));
        std::partial_sort(ranked.begin(), ranked.begin() + count, ranked.end());
        Row row{};
        for (size_t i = 0; i < count; ++i)
        {
            row[i] = scores.records[ranked[i].second].id;
        }
        m_recent.emplace_front(position, row);
        if (m_recent.size() > 128)
        {
            m_recent.pop_back();
        }
        return row;
    }
};

void MosaicSelection::deferRecommendations(std::shared_ptr<const PreviewScores> scores, int window, double penalty)
{
    m_recommendations = std::make_shared<PreviewRecommendationCache>(std::move(scores), window, penalty);
}

std::array<int, MosaicSelection::recommendationLimit> MosaicSelection::recommendationsAt(size_t position) const
{
    if (position >= indices.size())
    {
        return {};
    }
    if (m_recommendations)
    {
        return m_recommendations->get(*this, position);
    }
    return position < recommendations.size() ? recommendations[position] : std::array<int, recommendationLimit>{};
}

uint64_t previewFileVersion(const std::vector<unsigned char> &bytes)
{
    Fingerprint version;
    version.add(bytes.data(), bytes.size());
    return version.value();
}

uint64_t previewLibraryVersion(const std::vector<ImageRecord> &records, int width, int height)
{
    Fingerprint version;
    version.add(width);
    version.add(height);
    version.add(records.size());
    // 绑定实际参与匹配的记录顺序和历史惩罚，避免重建后复用旧的下标。
    for (const auto &record : records)
    {
        version.add(record.id);
        version.add(record.filePath);
        version.add(record.fileHash);
        version.add(record.featureVersion);
        version.add(record.useCount);
        version.add(record.avgL);
        version.add(record.avgA);
        version.add(record.avgB);
        version.add(record.edgeDensity);
        version.add(record.gridLab.size());
        version.add(record.gridLab.data(), record.gridLab.size() * sizeof(float));
        version.add(record.tinyPath);
        version.add(record.lbpPath);
    }
    return version.value();
}

PreviewAsset previewAsset(const ImageRecord &record)
{
    return assetVersion(record.filePath, record.fileHash);
}

PreviewAsset previewAsset(const PreviewCatalogEntry &entry)
{
    return assetVersion(entry.path, entry.hash);
}

std::string PreviewAsset::version() const
{
    // libc++ can use 128-bit file clock ticks, so fingerprint the full value without narrowing it.
    Fingerprint timestamp;
    timestamp.add(modified.time_since_epoch().count());
    return hash + ":" + std::to_string(bytes) + ":" + std::to_string(timestamp.value());
}

const PreviewCatalogEntry *PreviewCatalog::find(int id) const
{
    const auto it = std::lower_bound(entries.begin(), entries.end(), id, [](const auto &entry, int value)
    {
        return entry.id < value;
    });
    return it != entries.end() && it->id == id ? &*it : nullptr;
}

std::shared_ptr<const PreviewCatalog> makePreviewCatalog(const std::vector<ImageRecord> &records)
{
    auto catalog = std::make_shared<PreviewCatalog>();
    catalog->entries.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i)
    {
        const auto &record = records[i];
        catalog->entries.push_back({record.id, static_cast<int>(i), pathToUtf8(u8path(record.filePath).filename()),
                                    record.filePath, record.fileHash});
    }
    std::sort(catalog->entries.begin(), catalog->entries.end(), [](const auto &left, const auto &right)
    {
        return left.id < right.id;
    });
    return catalog;
}

bool PreviewAsset::current() const
{
    const auto latest = assetVersion(path, hash);
    return available == latest.available && (!available || (bytes == latest.bytes && modified == latest.modified));
}

bool previewAssetsCurrent(const MosaicSelection &selection)
{
    return std::all_of(selection.assets.begin(), selection.assets.end(), [](const auto &item)
    {
        return item.second.current();
    });
}
} // namespace mosaicraft
