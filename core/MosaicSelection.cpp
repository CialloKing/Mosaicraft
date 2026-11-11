#include "MosaicSelection.h"
#include "Database.h"
#include "UnicodeIO.h"

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
    return hash + ":" + std::to_string(bytes) + ":" + std::to_string(modified.time_since_epoch().count());
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
