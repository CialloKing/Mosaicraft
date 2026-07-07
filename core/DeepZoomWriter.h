#pragma once

#include "UnicodeIO.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace mosaicraft
{

class DeepZoomWriter
{
public:
    static void buildPyramid(const std::string& level0Dir,
                             int tileW, int tileH,
                             int cols, int rows,
                             int quality)
    {
        namespace fs = std::filesystem;

        fs::path sourceLevelPath = u8path(level0Dir);
        fs::path pyramidDir = sourceLevelPath.parent_path();
        fs::path outputDir = pyramidDir.parent_path();

        std::string filesStem = pathToUtf8(pyramidDir.filename());
        std::string stem = filesStem;
        if (stem.size() > 6 && stem.substr(stem.size() - 6) == "_files")
            stem = stem.substr(0, stem.size() - 6);

        std::string basePath = pathToUtf8(outputDir / stem);
        std::string filesDir = pathToUtf8(pyramidDir.filename());
        std::error_code ec;

        const int totalW = cols * tileW;
        const int totalH = rows * tileH;
        const int dziTileSize = 256;
        auto ceilDiv = [](int a, int b) { return (a + b - 1) / b; };

        int maxLevel = 0;
        for (int dim = std::max(totalW, totalH); dim > 1; dim = (dim + 1) / 2)
            ++maxLevel;

        auto levelWidth = [&](int level) {
            return std::max(1, static_cast<int>(
                std::ceil(totalW / std::pow(2.0, maxLevel - level))));
        };
        auto levelHeight = [&](int level) {
            return std::max(1, static_cast<int>(
                std::ceil(totalH / std::pow(2.0, maxLevel - level))));
        };

        fs::path sourceTilesDir = pyramidDir / "_source_tiles";
        fs::remove_all(sourceTilesDir, ec);
        ec.clear();
        fs::rename(sourceLevelPath, sourceTilesDir, ec);
        if (ec)
        {
            std::cerr << "ERROR: DeepZoom cannot prepare source tiles: "
                      << ec.message() << std::endl;
            return;
        }

        for (int level = 0; level <= maxLevel; ++level)
        {
            fs::path levelDir = pyramidDir / std::to_string(level);
            fs::remove_all(levelDir, ec);
            fs::create_directories(levelDir, ec);
            if (ec)
            {
                std::cerr << "ERROR: DeepZoom cannot create directory "
                          << pathToUtf8(levelDir) << ": " << ec.message() << std::endl;
                return;
            }
        }

        auto readSourceTile = [&](int c, int r) {
            fs::path p = sourceTilesDir /
                         (std::to_string(c) + "_" + std::to_string(r) + ".jpg");
            cv::Mat tile = imreadUnicode(pathToUtf8(p), cv::IMREAD_COLOR);
            if (!tile.empty() && (tile.cols != tileW || tile.rows != tileH))
                cv::resize(tile, tile, cv::Size(tileW, tileH), 0, 0, cv::INTER_AREA);
            return tile;
        };

        auto renderFullRegion = [&](int x0, int y0, int w, int h) {
            cv::Mat canvas(h, w, CV_8UC3, cv::Scalar(0, 0, 0));
            int c0 = x0 / tileW;
            int c1 = (x0 + w - 1) / tileW;
            int r0 = y0 / tileH;
            int r1 = (y0 + h - 1) / tileH;

            for (int r = r0; r <= r1; ++r)
            {
                for (int c = c0; c <= c1; ++c)
                {
                    cv::Mat tile = readSourceTile(c, r);
                    if (tile.empty()) continue;

                    int sx0 = c * tileW;
                    int sy0 = r * tileH;
                    int ox0 = std::max(x0, sx0);
                    int oy0 = std::max(y0, sy0);
                    int ox1 = std::min(x0 + w, sx0 + tileW);
                    int oy1 = std::min(y0 + h, sy0 + tileH);
                    if (ox1 <= ox0 || oy1 <= oy0) continue;

                    cv::Rect src(ox0 - sx0, oy0 - sy0, ox1 - ox0, oy1 - oy0);
                    cv::Rect dst(ox0 - x0, oy0 - y0, ox1 - ox0, oy1 - oy0);
                    tile(src).copyTo(canvas(dst));
                }
            }

            return canvas;
        };

        auto renderLevelRegion = [&](int level, int x0, int y0, int w, int h) {
            cv::Mat canvas(h, w, CV_8UC3, cv::Scalar(0, 0, 0));
            int c0 = x0 / dziTileSize;
            int c1 = (x0 + w - 1) / dziTileSize;
            int r0 = y0 / dziTileSize;
            int r1 = (y0 + h - 1) / dziTileSize;

            for (int r = r0; r <= r1; ++r)
            {
                for (int c = c0; c <= c1; ++c)
                {
                    fs::path p = pyramidDir / std::to_string(level) /
                                 (std::to_string(c) + "_" + std::to_string(r) + ".jpg");
                    cv::Mat tile = imreadUnicode(pathToUtf8(p), cv::IMREAD_COLOR);
                    if (tile.empty()) continue;

                    int sx0 = c * dziTileSize;
                    int sy0 = r * dziTileSize;
                    int ox0 = std::max(x0, sx0);
                    int oy0 = std::max(y0, sy0);
                    int ox1 = std::min(x0 + w, sx0 + tile.cols);
                    int oy1 = std::min(y0 + h, sy0 + tile.rows);
                    if (ox1 <= ox0 || oy1 <= oy0) continue;

                    cv::Rect src(ox0 - sx0, oy0 - sy0, ox1 - ox0, oy1 - oy0);
                    cv::Rect dst(ox0 - x0, oy0 - y0, ox1 - ox0, oy1 - oy0);
                    tile(src).copyTo(canvas(dst));
                }
            }

            return canvas;
        };

        std::cout << "  DZI source: " << cols << "x" << rows
                  << " mosaic tiles (" << tileW << "x" << tileH << ")"
                  << std::endl;

        int fullCols = ceilDiv(totalW, dziTileSize);
        int fullRows = ceilDiv(totalH, dziTileSize);
        std::cout << "  DZI level " << maxLevel << ": " << fullCols << "x" << fullRows
                  << " tiles (" << totalW << "x" << totalH << ")" << std::flush;

        int written = 0;
        for (int r = 0; r < fullRows; ++r)
        {
            for (int c = 0; c < fullCols; ++c)
            {
                int x = c * dziTileSize;
                int y = r * dziTileSize;
                int w = std::min(dziTileSize, totalW - x);
                int h = std::min(dziTileSize, totalH - y);
                cv::Mat tile = renderFullRegion(x, y, w, h);
                fs::path dst = pyramidDir / std::to_string(maxLevel) /
                               (std::to_string(c) + "_" + std::to_string(r) + ".jpg");
                if (imwriteUnicode(pathToUtf8(dst), tile,
                                   {cv::IMWRITE_JPEG_QUALITY, quality}))
                    ++written;
                else
                    std::cerr << "ERROR: DeepZoom failed to write " << pathToUtf8(dst) << std::endl;
            }
        }
        std::cout << " (" << written << " tiles)" << std::endl;

        for (int level = maxLevel - 1; level >= 0; --level)
        {
            int levelW = levelWidth(level);
            int levelH = levelHeight(level);
            int prevW = levelWidth(level + 1);
            int prevH = levelHeight(level + 1);
            int levelCols = ceilDiv(levelW, dziTileSize);
            int levelRows = ceilDiv(levelH, dziTileSize);

            std::cout << "  DZI level " << level << ": " << levelCols << "x" << levelRows
                      << " tiles (" << levelW << "x" << levelH << ")" << std::flush;
            written = 0;

            for (int r = 0; r < levelRows; ++r)
            {
                for (int c = 0; c < levelCols; ++c)
                {
                    int x = c * dziTileSize;
                    int y = r * dziTileSize;
                    int w = std::min(dziTileSize, levelW - x);
                    int h = std::min(dziTileSize, levelH - y);
                    int srcX = x * 2;
                    int srcY = y * 2;
                    int srcW = std::min(w * 2, prevW - srcX);
                    int srcH = std::min(h * 2, prevH - srcY);

                    cv::Mat high = renderLevelRegion(level + 1, srcX, srcY, srcW, srcH);
                    cv::Mat tile;
                    cv::resize(high, tile, cv::Size(w, h), 0, 0, cv::INTER_AREA);

                    fs::path dst = pyramidDir / std::to_string(level) /
                                   (std::to_string(c) + "_" + std::to_string(r) + ".jpg");
                    if (imwriteUnicode(pathToUtf8(dst), tile,
                                       {cv::IMWRITE_JPEG_QUALITY, quality}))
                        ++written;
                    else
                        std::cerr << "ERROR: DeepZoom failed to write " << pathToUtf8(dst) << std::endl;
                }
            }

            std::cout << " (" << written << " tiles)" << std::endl;
        }

        fs::remove_all(sourceTilesDir, ec);

        std::string dziPath = basePath + ".dzi";
        std::ofstream dzi(u8path(dziPath));
        if (!dzi)
        {
            std::cerr << "ERROR: cannot open " << dziPath << std::endl;
            return;
        }
        dzi << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        dzi << "<Image xmlns=\"http://schemas.microsoft.com/deepzoom/2008\"\n";
        dzi << "       Format=\"jpg\" Overlap=\"0\" TileSize=\"" << dziTileSize << "\">\n";
        dzi << "  <Size Width=\"" << totalW << "\" Height=\"" << totalH << "\"/>\n";
        dzi << "</Image>\n";
        dzi.close();
        if (!dzi)
        {
            std::cerr << "ERROR: failed to write " << dziPath << std::endl;
            return;
        }
        std::cout << "  DZI manifest: " << dziPath
                  << " (" << (maxLevel + 1) << " levels)" << std::endl;

        std::string htmlPath = basePath + ".html";
        std::ofstream html(u8path(htmlPath));
        if (!html)
        {
            std::cerr << "ERROR: cannot open " << htmlPath << std::endl;
            return;
        }
        html << R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Mosaicraft - )" << stem << R"(</title>
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
  #fallback {
    display: none; position: fixed; top: 50%; left: 50%; transform: translate(-50%,-50%);
    background: rgba(0,0,0,0.85); color: #ccc; padding: 20px 30px; border-radius: 12px;
    text-align: center; z-index: 20; font-size: 14px; line-height: 1.8;
  }
  #fallback code { background: #333; padding: 2px 8px; border-radius: 4px; color: #fff; }
</style>
</head>
<body>
<div id="viewer"></div>
<div id="info">)" << totalW << " x " << totalH << R"( px  |  Levels: <span>)" << (maxLevel + 1) << R"(</span>  |  Scroll to zoom</div>
<div id="fallback">
  <p>OpenSeadragon failed to load.</p>
  <p>Run a local server in the output directory:</p>
  <p><code>python -m http.server 8080</code></p>
  <p>then open <code>http://localhost:8080/)" << stem << R"(.html</code></p>
</div>
<script src="https://unpkg.com/openseadragon@4.1.1/build/openseadragon/openseadragon.min.js"></script>
<script>
  var viewer = OpenSeadragon({
    id: "viewer",
    prefixUrl: "https://unpkg.com/openseadragon@4.1.1/build/openseadragon/images/",
    tileSources: {
      Image: {
        xmlns: "http://schemas.microsoft.com/deepzoom/2008",
        Url: ")" << filesDir << R"(/",
        Format: "jpg",
        Overlap: "0",
        TileSize: )" << dziTileSize << R"(,
        Size: { Width: ")" << totalW << R"(", Height: ")" << totalH << R"(" }
      }
    },
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
  viewer.addHandler("open-failed", function() {
    document.getElementById("fallback").style.display = "block";
  });
</script>
</body>
</html>
)";
        html.close();
        if (!html)
        {
            std::cerr << "ERROR: failed to write " << htmlPath << std::endl;
            return;
        }
        std::cout << "  HTML viewer: " << htmlPath << std::endl;
    }
};

} // namespace mosaicraft
