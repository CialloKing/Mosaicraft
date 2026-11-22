#include "LibraryFeatureStore.h"
#include <chrono>

namespace mosaicraft
{
void LibraryFeatureStore::Reader::prepare(const int *candidates, size_t count)
{
    m_batch.clear();
    m_needed.clear();
    for (size_t i = 0; i < count; ++i)
    {
        const int index = candidates[i];
        if (index < 0)
        {
            continue;
        }
        const int id = m_records.at(index).id;
        if (m_batch.insert(id).second)
        {
            if (!m_session.rows.count(id))
            {
                m_needed.push_back(id);
            }
            if (m_batch.size() == m_limit)
            {
                break;
            }
        }
    }
    if (m_needed.empty())
    {
        return;
    }
    // 只淘汰批次外的行，保证评分工作线程读取的地址在整批内有效。
    for (auto row = m_session.rows.begin(); row != m_session.rows.end() &&
         m_session.rows.size() + m_needed.size() > m_limit;)
    {
        if (!m_batch.count(row->first))
        {
            row = m_session.rows.erase(row);
            ++m_stats.evictions;
        }
        else
        {
            ++row;
        }
    }
    std::sort(m_needed.begin(), m_needed.end());
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const bool loaded = m_db.readTextures(m_needed, m_session.rows);
    m_stats.sqliteMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (!loaded)
    {
        m_session.rows.clear();
        throw InvalidLibraryFeatures(m_db.error());
    }
    m_stats.misses += m_needed.size();
    m_stats.sourceReads += m_needed.size();
    m_stats.peakBytes = std::max(m_stats.peakBytes, m_session.rows.size() * rowBytes);
    m_stats.temporaryPeakBytes = std::max(m_stats.temporaryPeakBytes,
        m_needed.capacity() * sizeof(int) + m_batch.size() * (sizeof(int) + 3 * sizeof(void *)));
}

const LibraryFeatureStore::Reader::Row *LibraryFeatureStore::Reader::preparedRow(int index) const
{
    const auto row = m_session.rows.find(m_records.at(index).id);
    return row == m_session.rows.end() ? nullptr : &row->second;
}

const LibraryFeatureStore::Reader::Row &LibraryFeatureStore::Reader::get(int index)
{
    if (const auto *row = preparedRow(index))
    {
        ++m_stats.hits;
        return *row;
    }
    prepare({index});
    return *preparedRow(index);
}

bool LibraryFeatureStore::load(Database &db, const std::vector<ImageRecord> &records,
                               const std::vector<int> &needed, std::vector<uint8_t> &tiny, std::vector<float> &lbp,
                               std::vector<uint8_t> &tinyValid, std::vector<uint8_t> &lbpValid, Reader::ReadStats *stats)
{
    Reader reader(db, records, 128 * Reader::rowBytes);
    tiny.resize(needed.size() * 256);
    lbp.resize(needed.size() * 256);
    tinyValid.assign(needed.size(), 1);
    lbpValid.assign(needed.size(), 1);
    try
    {
        for (size_t first = 0; first < needed.size(); first += reader.capacity())
        {
            const auto last = std::min(first + reader.capacity(), needed.size());
            reader.prepare({needed.begin() + first, needed.begin() + last});
            for (size_t pos = first; pos < last; ++pos)
            {
                const auto &row = reader.get(needed[pos]);
                std::copy(row.tiny.begin(), row.tiny.end(), tiny.data() + pos * 256);
                std::copy(row.lbp.begin(), row.lbp.end(), lbp.data() + pos * 256);
            }
        }
    }
    catch (const InvalidLibraryFeatures &)
    {
        return false;
    }
    if (stats)
    {
        *stats = reader.stats();
    }
    return true;
}
}
