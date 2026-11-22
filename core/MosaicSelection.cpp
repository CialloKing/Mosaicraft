#include "MosaicSelection.h"
#include "Database.h"
#include "PreviewCandidates.h"
#include "UnicodeIO.h"

#include <algorithm>
#include <list>
#include <mutex>

namespace mosaicraft
{
namespace
{
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

PreviewScores::~PreviewScores() = default;

std::shared_ptr<PreviewScores> PreviewScores::create(size_t tiles, int candidates,
                                                     const std::vector<ImageRecord> &records,
                                                     const PreviewCachePolicy &policy, PreviewCacheMiss &miss)
{
    constexpr size_t cellBytes = sizeof(int) + sizeof(double);
    constexpr size_t rowBytes = sizeof(ImageRecord) + 192 * sizeof(float);
    const size_t limit = policy.limits[static_cast<size_t>(PreviewCacheKind::Scores)];
    if (policy.tier == PreviewCachePolicy::Tier::Disabled)
    {
        miss = PreviewCacheMiss::Memory;
        return {};
    }
    if (limit < sizeof(PreviewScores) || records.size() > (limit - sizeof(PreviewScores)) / rowBytes ||
        candidates <= 0 || tiles > (limit - sizeof(PreviewScores) - records.size() * rowBytes) / cellBytes / candidates)
    {
        miss = PreviewCacheMiss::Size;
        return {};
    }
    const size_t cells = tiles * candidates;
    try
    {
        auto scores = std::make_shared<PreviewScores>();
        if (!scores->storage.grow(sizeof(PreviewScores) + cells * cellBytes + records.size() * rowBytes, policy, miss))
        {
            return {};
        }
        scores->candidates = candidates;
        scores->indices.resize(cells);
        scores->values.resize(cells);
        scores->records.resize(records.size());
        size_t bytes = sizeof(PreviewScores) + scores->indices.capacity() * sizeof(int) +
            scores->values.capacity() * sizeof(double) + scores->records.capacity() * sizeof(ImageRecord);
        for (size_t i = 0; i < records.size(); ++i)
        {
            scores->records[i].id = records[i].id;
            scores->records[i].gridLab = records[i].gridLab;
            bytes += scores->records[i].gridLab.capacity() * sizeof(float);
        }
        return scores->storage.grow(bytes, policy, miss) ? scores : nullptr;
    }
    catch (const std::bad_alloc &)
    {
        miss = PreviewCacheMiss::Allocation;
        return {};
    }
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

    std::shared_ptr<PreviewRecommendationCache> withoutRanking() const
    {
        return std::make_shared<PreviewRecommendationCache>(m_scores, m_window, m_penalty);
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

        const auto scores = detail::cachedBaseScoreRow(*m_scores, position);
        const auto ranked = detail::rankPreviewCandidates(scores, m_scores->records, selection, position,
            m_window, m_penalty, MosaicSelection::recommendationLimit);
        Row row{};
        for (size_t i = 0; i < ranked.size(); ++i)
        {
            row[i] = ranked[i].imageId;
        }
        m_recent.emplace_front(position, row);
        if (m_recent.size() > 128)
        {
            m_recent.pop_back();
        }
        return row;
    }
};

PreviewEditResult MosaicSelection::replaceAsset(size_t position, int imageId, std::string_view version)
{
    if (position >= indices.size() || position >= imageIds.size())
    {
        return PreviewEditResult::InvalidPosition;
    }
    if (imageId <= 0)
    {
        return PreviewEditResult::InvalidImageId;
    }
    const auto *entry = catalog ? catalog->find(imageId) : nullptr;
    if (!entry)
    {
        return PreviewEditResult::UnknownImage;
    }
    auto asset = previewAsset(*entry);
    if (!asset.available || asset.version() != version)
    {
        return PreviewEditResult::StaleAsset;
    }
    if (indices[position] == entry->index)
    {
        return PreviewEditResult::Unchanged;
    }

    // 编辑副本不能清掉原快照的排序，但两者仍可共用只读基础分。
    auto unranked = m_recommendations ? m_recommendations->withoutRanking() : nullptr;
    assets.insert_or_assign(entry->id, std::move(asset));
    indices[position] = entry->index;
    imageIds[position] = entry->id;
    ++manualEdits;
    m_recommendations = std::move(unranked);
    recommendations.clear();
    return PreviewEditResult::Applied;
}

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

uint64_t previewLibraryVersion(const std::string &assetKey, const std::vector<ImageRecord> &records,
                               int width, int height, bool includeUseCounts)
{
    Fingerprint version;
    version.add(assetKey);
    version.add(width);
    version.add(height);
    version.add(includeUseCounts);
    if (includeUseCounts)
    {
        for (const auto &record : records)
        {
            version.add(record.id);
            version.add(record.useCount);
        }
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
