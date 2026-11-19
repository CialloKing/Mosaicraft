#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace mosaicraft
{
class RuntimeContext;

enum class PreviewCacheKind { Scores, Features, Candidates };
enum class PreviewCacheMiss { None, Memory, ProcessBudget, Size, SourceVersion, Key, Allocation };

struct PreviewCachePolicy
{
    static constexpr size_t MiB = 1024 * 1024;
    enum class Tier { Disabled, Standard, Full };
    static PreviewCachePolicy select(uint64_t available, size_t hostBudget, bool largeScores, bool largeFeatures);
    static PreviewCachePolicy forTask(size_t hostBudget, bool expand);

    Tier tier = Tier::Standard;
    std::array<size_t, 3> limits{128 * MiB, 64 * MiB, 64 * MiB};
    size_t totalLimit = 256 * MiB;
    size_t incrementalLimit = 256 * MiB;
};

class PreviewCacheReservation
{
  public:
    explicit PreviewCacheReservation(PreviewCacheKind kind) : m_kind(kind) {}
    ~PreviewCacheReservation();
    PreviewCacheReservation(const PreviewCacheReservation &) = delete;
    PreviewCacheReservation &operator=(const PreviewCacheReservation &) = delete;

    bool grow(size_t bytes, const PreviewCachePolicy &policy, PreviewCacheMiss &miss);
    size_t bytes() const { return m_bytes; }
    static size_t totalBytes();

  private:
    PreviewCacheKind m_kind;
    size_t m_bytes = 0;
};

template <typename T> struct PreviewCacheRows
{
    explicit PreviewCacheRows(PreviewCacheKind kind) : storage(kind) {}
    PreviewCacheReservation storage;
    std::vector<T> data;

    static std::unique_ptr<PreviewCacheRows> create(size_t count, PreviewCacheKind kind,
        const PreviewCachePolicy &policy, PreviewCacheMiss &miss)
    {
        if (count > (SIZE_MAX - sizeof(PreviewCacheRows)) / sizeof(T))
        {
            miss = PreviewCacheMiss::Size;
            return {};
        }
        try
        {
            auto rows = std::make_unique<PreviewCacheRows>(kind);
            if (!rows->storage.grow(sizeof(*rows) + count * sizeof(T), policy, miss))
            {
                return {};
            }
            rows->data.resize(count);
            return rows->storage.grow(sizeof(*rows) + rows->data.capacity() * sizeof(T), policy, miss)
                ? std::move(rows) : nullptr;
        }
        catch (const std::bad_alloc &)
        {
            miss = PreviewCacheMiss::Allocation;
            return {};
        }
    }
};
} // namespace mosaicraft
