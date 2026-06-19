#pragma once

#include <iostream>
#include <string>

namespace mosaicraft
{

class MosaicEngine
{
public:
    struct Config
    {
        int tileW = 45;         // 默认 45×80，保持 9:16 比例；增大 tile 确保特征与图库同尺度，输出可控
        int tileH = 80;
        int candidates = 200;
        double lRange = 20.0;
        double usePenalty = 0.01;

        // 输出尺寸（0 = 用原始目标图分辨率；>0 则将目标图缩放到指定尺寸，输出 tile 始终为原生 180×320）
        int outW = 0;
        int outH = 0;

        // 原生 tile 尺寸（图库归一化规格，outW=0 时生效）
        int nativeTileW = 180;
        int nativeTileH = 320;

        // 五特征权重（Grid4x4 最重，结构匹配核心；Edge 辅助过滤纹理）
        double labWeight  = 0.20;
        double gridWeight = 0.45;
        double tinyWeight = 0.25;
        double edgeWeight = 0.05;
        double lbpWeight  = 0.05;

        // 邻域去重：滑动窗口内频率分级惩罚（1次轻罚/2次中罚/3+次重罚）
        int neighborWindow = 300;    // 覆盖约 2.7 行，超出窗口后可重用
        double neighborPenalty = 100.0;  // 基础惩罚值

        // Top-N 随机：1 = 始终选最优匹配（邻域惩罚已保证多样性）
        int topNrandom = 1;

        bool useGpu = true;

        void print() const
        {
            std::cout << "  tile: " << tileW << "x" << tileH
                      << "  out: " << (outW > 0 ? std::to_string(outW) + "x" + std::to_string(outH) : "native " + std::to_string(nativeTileW) + "x" + std::to_string(nativeTileH) + " per tile")
                      << "  candidates: " << candidates
                      << "  L range: " << lRange
                      << std::endl;
            std::cout << "  weights: LAB=" << labWeight
                      << " Grid=" << gridWeight
                      << " Tiny=" << tinyWeight
                      << " Edge=" << edgeWeight
                      << " LBP=" << lbpWeight
                      << "  penalty=" << usePenalty
                      << std::endl;
        }
    };

    MosaicEngine() = default;

    bool generate(const std::string& targetPath,
                  const std::string& dbPath,
                  const std::string& outputPath,
                  const Config& config = Config());
};

} // namespace mosaicraft
