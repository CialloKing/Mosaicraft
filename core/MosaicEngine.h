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
        int tileW = 9;          // 默认 9×16（图库 180×320 的等比缩小），最大化 tile 数量
        int tileH = 16;
        int candidates = 200;
        double lRange = 20.0;
        double usePenalty = 0.01;

        // 输出尺寸（0 = 用原始目标图分辨率；>0 则将目标图缩放到指定尺寸，输出 tile 始终为原生 180×320）
        int outW = 0;
        int outH = 0;

        // 原生 tile 尺寸（图库归一化规格，outW=0 时生效）
        int nativeTileW = 180;   // 输出 tile 尺寸（与图库归一化尺寸匹配）
        int nativeTileH = 320;

        // 五特征权重（Grid4x4 最重，结构匹配核心；Edge 辅助过滤纹理）
        double labWeight  = 0.20;
        double gridWeight = 0.45;
        double tinyWeight = 0.25;
        double edgeWeight = 0.05;
        double lbpWeight  = 0.05;

        // 邻域去重：滑动窗口内频率分级惩罚（1次轻罚/2次中罚/3+次重罚）
        int neighborWindow = 0;    // 邻域窗口：0=自动（≥2×tilesX），覆盖垂直邻域
        double neighborPenalty = 100.0;  // 基础惩罚值

        // Top-N 随机：>1 从 Top-N 选最优打破纯色区规则图案
        int topNrandom = 3;

        bool useGpu = true;
        bool tiledOutput = false;   // 分块输出：每 tile 独立文件，消除输出尺寸限制
        bool deepZoom    = false;   // 生成 Deep Zoom 金字塔（含多级缩放 + .dzi 清单）
        int  jpegQuality = 95;      // JPEG 质量（1-100），分块模式用 95，单图用 100
        std::string outputFormat = "jpg";  // 输出格式：jpg / png / webp / tiff
        bool   formatExplicit = false;    // 是否显式指定 --format（否则允许自动切换）

        // 局部颜色校正：对每张候选图随机微调亮度/饱和度，减少视觉重复感
        bool   colorAdjust = false;        // 默认关闭（远看有摩尔纹，待进一步优化）
        double colorStrength = 0.04;       // LAB L 通道微调幅度（0.04 = ±4% 亮度）

        // Benchmark：输出各阶段耗时与统计（用于性能分析）
        bool benchmark = false;

        // 目标图上采样因子：>1 时先将原图放大，再以 9×16 格分割
        // 2× = 4× tile 密度，输出分辨率不变（配合 nativeTile 缩半）
        int upscale = 0;   // 0=自动（nativeTile<180 时自动 2×）

        void print() const
        {
            std::cout << "  tile: " << tileW << "x" << tileH
                      << "  candidates: " << candidates
                      << "  L range: " << lRange
                      << "  quality: " << jpegQuality
                      << "  format: " << outputFormat
                      << (tiledOutput ? (deepZoom ? "  output: deepzoom" : "  output: tiled") : "")
                      << std::endl;
            std::cout << "  weights: LAB=" << labWeight
                      << " Grid=" << gridWeight
                      << " Tiny=" << tinyWeight
                      << " Edge=" << edgeWeight
                      << " LBP=" << lbpWeight
                      << "  penalty=" << usePenalty
                      << (colorAdjust ? "  colorAdj=on" : "")
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
