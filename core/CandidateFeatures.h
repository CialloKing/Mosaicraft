#pragma once
#include "FeatureMatrix.h"
#include "FeaturePack.h"

namespace mosaicraft
{
// Both backends share source validation; CPU storage contains only requested candidates.
class CandidateFeatures
{
    std::vector<int> m_slots;
    std::vector<uint8_t> m_tiny, m_tinyValid, m_lbpValid;
    std::vector<float> m_lbp;

  public:
    CandidateFeatures(const std::vector<ImageRecord> &records, const std::vector<int> &candidates,
                      const std::string &directory)
        : m_slots(records.size(), -1)
    {
        std::vector<int> needed;
        for (int index : candidates)
        {
            if (index >= 0 && static_cast<size_t>(index) < records.size() && m_slots[index] < 0)
            {
                m_slots[index] = static_cast<int>(needed.size());
                needed.push_back(index);
            }
        }
        FeaturePack::load(directory, records, needed, m_tiny, m_lbp, m_tinyValid, m_lbpValid);
    }
    bool hasTiny(int index) const
    {
        return m_tinyValid[m_slots[index]] != 0;
    }
    bool hasLbp(int index) const
    {
        return m_lbpValid[m_slots[index]] != 0;
    }
    auto tiny(int index) const
    {
        return FeatureMatrix<uint8_t, 256>::Row<const uint8_t>(m_tiny.data() +
                                                               static_cast<size_t>(m_slots[index]) * 256);
    }
    auto lbp(int index) const
    {
        return FeatureMatrix<float, 256>::Row<const float>(m_lbp.data() + static_cast<size_t>(m_slots[index]) * 256);
    }
};
} // namespace mosaicraft
