#pragma once
#include "MosaicEngine.h"

namespace mosaicraft
{
// 仅显式基准和测试注入预算/随机种子，生产 CLI 与 API 不暴露这些选项。
struct MatchingAccess
{
    static void configure(MosaicEngine &engine, MatchingLimits limits, bool fixedSeed = false, uint32_t seed = 0)
    {
        engine.m_limits = limits;
        engine.m_fixedSeed = fixedSeed;
        engine.m_seed = seed;
    }
    static void captureSelections(MosaicEngine &engine, std::vector<int> &selected)
    {
        engine.m_selectedObserver = &selected;
    }
    static size_t compatibleFeatureImages(const MosaicEngine &engine)
    {
        return engine.m_compatibleFeatureImages;
    }
    static size_t compatibleScoreTiles(const MosaicEngine &engine)
    {
        return engine.m_compatibleScoreTiles;
    }
    static size_t compatibleTemporaryBytes(const MosaicEngine &engine)
    {
        return engine.m_compatibleTemporaryBytes;
    }
};
} // namespace mosaicraft
