#pragma once

#include "UnicodeIO.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace mosaicraft
{

// ============================================================
// Deep Zoom 金字塔生成器
// 输入: level 0 目录 (已包含 {col}_{row}.jpg 全分辨率 tiles)
// 输出: 更高级别 + .dzi 清单，兼容 OpenSeadragon
// ============================================================
class DeepZoomWriter
{
public:
    // level0Dir: {name}_files/0/ (已包含 level 0 tiles)
    // tileW/H: level 0 单 tile 像素尺寸
    // cols/rows: level 0 列/行 tile 数
    static void buildPyramid(const std::string& level0Dir,
                             int tileW, int tileH,
                             int cols, int rows,
                             int quality)
    {
        namespace fs = std::filesystem;

        // level0Dir 形如 "output_files/0" → 提取基础名 "output"
        fs::path l0Path(level0Dir);
        fs::path pyramidDir = l0Path.parent_path();  // "output_files"
        std::string baseName = pyramidDir.parent_path().string() + "/"
                             + pyramidDir.stem().string();  // 去掉 "_files" 后缀?

        // 实际 base: 去掉 "_files" 后缀
        std::string stem = pyramidDir.stem().string();
        if (stem.size() > 6 && stem.substr(stem.size() - 6) == "_files")
            stem = stem.substr(0, stem.size() - 6);
        std::string basePath = pyramidDir.parent_path().string() + "/" + stem;
        std::error_code ec;

        std::cout << "  DZI level 0: " << cols << "x" << rows << " tiles (" << tileW << "x" << tileH << ")" << std::endl;

        // 生成更高层级
        int curCols = cols;
        int curRows = rows;
        int curW = tileW;
        int curH = tileH;
        int maxLevel = 0;

        while (curCols > 1 || curRows > 1)
        {
            maxLevel++;
            int nextCols = std::max(1, (curCols + 1) / 2);
            int nextRows = std::max(1, (curRows + 1) / 2);
            int nextW = curW * 2;
            int nextH = curH * 2;
            std::string levelDir = pyramidDir.string() + "/" + std::to_string(maxLevel);
            fs::create_directories(levelDir, ec);

            std::cout << "  DZI level " << maxLevel << ": " << nextCols << "x" << nextRows
                      << " tiles (" << nextW << "x" << nextH << ")" << std::flush;

            int written = 0;
            cv::Mat canvas(nextH, nextW, CV_8UC3);

            for (int r = 0; r < nextRows; ++r)
            {
                for (int c = 0; c < nextCols; ++c)
                {
                    bool hasAny = false;
                    canvas.setTo(cv::Scalar(0, 0, 0));

                    for (int dr = 0; dr < 2; ++dr)
                    {
                        for (int dc = 0; dc < 2; ++dc)
                        {
                            int srcR = r * 2 + dr;
                            int srcC = c * 2 + dc;
                            if (srcR >= curRows || srcC >= curCols) continue;

                            std::string srcLevel = std::to_string(maxLevel - 1);
                            char srcPath[512];
                            snprintf(srcPath, sizeof(srcPath), "%s/%s/%d_%d.jpg",
                                     pyramidDir.string().c_str(), srcLevel.c_str(), srcC, srcR);
                            cv::Mat tile = imreadUnicode(srcPath, cv::IMREAD_COLOR);
                            if (tile.empty()) continue;

                            cv::Rect roi(dc * curW, dr * curH, curW, curH);
                            if (tile.cols != curW || tile.rows != curH)
                                cv::resize(tile, tile, cv::Size(curW, curH), 0, 0, cv::INTER_AREA);
                            tile.copyTo(canvas(roi));
                            hasAny = true;
                        }
                    }

                    if (hasAny)
                    {
                        char dstPath[512];
                        snprintf(dstPath, sizeof(dstPath), "%s/%d/%d_%d.jpg",
                                 pyramidDir.string().c_str(), maxLevel, c, r);
                        imwriteUnicode(dstPath, canvas,
                                       {cv::IMWRITE_JPEG_QUALITY, quality});
                        written++;
                    }
                }
            }
            std::cout << " (" << written << " tiles)" << std::endl;

            curCols = nextCols;
            curRows = nextRows;
            curW = nextW;
            curH = nextH;
        }

        // 生成 .dzi 清单
        std::string dziPath = basePath + ".dzi";
        std::ofstream dzi(dziPath);
        int totalW = cols * tileW;
        int totalH = rows * tileH;
        dzi << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        dzi << "<Image xmlns=\"http://schemas.microsoft.com/deepzoom/2008\"\n";
        dzi << "       Format=\"jpg\" Overlap=\"0\" TileSize=\"" << tileW << "\">\n";
        dzi << "  <Size Width=\"" << totalW << "\" Height=\"" << totalH << "\"/>\n";
        dzi << "</Image>\n";
        dzi.close();
        std::cout << "  DZI manifest: " << dziPath << " (" << (maxLevel + 1) << " levels)" << std::endl;

        // 生成 index.html — 双击即可在浏览器中浏览 Deep Zoom 马赛克
        std::string htmlPath = basePath + ".html";
        std::ofstream html(htmlPath);
        // dzi 文件名（不含路径，因为 html 和 .dzi 在同一目录）
        std::string dziName = stem + ".dzi";
        // _files 目录名
        std::string filesDir = stem + "_files";
        html << R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Mosaicraft — )" << stem << R"(</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { background: #1a1a1a; font-family: system-ui, sans-serif; }
  #viewer { width: 100vw; height: 100vh; }
  #info {
    position: fixed; bottom: 12px; left: 50%; transform: translateX(-50%);
    background: rgba(0,0,0,0.7); color: #ccc; padding: 6px 16px;
    border-radius: 20px; font-size: 13px; pointer-events: none; z-index: 10;
  }
  #info span { color: #fff; font-weight: 600; }
</style>
</head>
<body>
<div id="viewer"></div>
<div id="info">)" << totalW << " × " << totalH << R"( px  |  Level 0: <span>)" << cols << "×" << rows << R"(</span> tiles  |  Scroll to zoom</div>
<script src="https://unpkg.com/openseadragon@4.1.1/build/openseadragon/openseadragon.min.js"></script>
<script>
  OpenSeadragon({
    id: "viewer",
    prefixUrl: "https://unpkg.com/openseadragon@4.1.1/build/openseadragon/images/",
    tileSources: ")" << dziName << R"(",
    showNavigator: true,
    navigatorPosition: "BOTTOM_RIGHT",
    navigatorHeight: 140,
    navigatorWidth: 180,
    showZoomControl: true,
    showHomeControl: true,
    showFullPageControl: true,
    homeFillsViewer: true,
    visibilityRatio: 0.5,
    minZoomImageRatio: 0.1,
    maxZoomPixelRatio: 4
  });
</script>
</body>
</html>
)";
        html.close();
        std::cout << "  HTML viewer: " << htmlPath << std::endl;
    }
};

} // namespace mosaicraft
