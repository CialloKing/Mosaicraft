#pragma once

#include "Database.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace mosaicraft
{
class RuntimeContext;
class InvalidLibraryFeatures : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class LibraryFeatureStore
{
public:
    class Reader
    {
    public:
        using Row = TextureFeatures;
        struct Session
        {
            std::string key;
            std::unordered_map<int, Row> rows;
        };
        struct ReadStats
        {
            size_t hits = 0, misses = 0, sourceReads = 0, evictions = 0, peakBytes = 0;
            size_t temporaryPeakBytes = 0;
            double sqliteMs = 0;
            bool sessionHit = false;
        };
        Reader(Database &db, const std::vector<ImageRecord> &records,
               size_t budget = 256ULL * 1024 * 1024, RuntimeContext * = nullptr, Session *session = nullptr)
            : m_db(db), m_records(records), m_session(session ? *session : m_local),
              m_limit(std::max<size_t>(1, budget / rowBytes))
        {
            const auto key = db.featureKey();
            if (key != m_session.key || m_session.rows.size() > m_limit)
            {
                m_session = {key, {}};
            }
            m_stats.sessionHit = !m_session.rows.empty();
            m_stats.peakBytes = m_session.rows.size() * rowBytes;
        }
        void prepare(const int *candidates, size_t count);
        void prepare(const std::vector<int> &candidates) { prepare(candidates.data(), candidates.size()); }
        template<class Score>
        void scoreBatches(const int *candidates, size_t count, bool alreadyPrepared, Score score)
        {
            // 预算不足一行时，仍只在小批之间访问数据库；评分本身只读内存。
            const size_t step = alreadyPrepared ? std::max<size_t>(count, 1) : capacity();
            for (size_t first = 0; first < count; first += step)
            {
                const auto size = std::min(step, count - first);
                if (!alreadyPrepared)
                {
                    prepare(candidates + first, size);
                }
                for (size_t i = first; i < first + size; ++i)
                {
                    if (candidates[i] >= 0)
                    {
                        score(i, candidates[i], *preparedRow(candidates[i]));
                    }
                }
            }
        }
        const Row &get(int index);
        const Row *preparedRow(int index) const;
        const ReadStats &stats() const { return m_stats; }
        size_t capacity() const { return m_limit; }
        size_t retainedRows() const { return m_session.rows.size(); }
        static constexpr size_t rowBytes = sizeof(Row) + 4 * sizeof(void *);

    private:
        Database &m_db;
        const std::vector<ImageRecord> &m_records;
        Session m_local;
        Session &m_session;
        ReadStats m_stats;
        size_t m_limit;
        std::vector<int> m_needed;
        std::unordered_set<int> m_batch;
    };
    static bool load(Database &db, const std::vector<ImageRecord> &records,
                     const std::vector<int> &needed, std::vector<uint8_t> &tiny, std::vector<float> &lbp,
                     std::vector<uint8_t> &tinyValid, std::vector<uint8_t> &lbpValid,
                     Reader::ReadStats *stats = nullptr);
};
}
