#pragma once

#include "DeepZoomLayout.h"
#include "UnicodeIO.h"
#include "RuntimeContext.h"
#include "WorkerPool.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace mosaicraft
{

class DeepZoomWriter
{
public:
    using WriteImage = decltype(&imwriteUnicode);

    static bool buildPyramid(const std::string& level0Dir,
                             int tileW, int tileH,
                             int cols, int rows,
                             int quality, WorkerPool &workers, bool benchmark, RuntimeContext *runtime = nullptr,
                             WriteImage writeImage = imwriteUnicode)
    {
        namespace fs = std::filesystem;
        auto fail = [&](const std::string &message, const std::string &path)
        {
            if (runtime)
            {
                return runtime->fail("output_write_failed", message, "outputPath", path);
            }
            std::cerr << "ERROR: " << message << std::endl;
            return false;
        };

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
        constexpr int dziTileSize = deepZoomTileSize;
        const auto levels = deepZoomLevels(totalW, totalH);
        const int maxLevel = static_cast<int>(levels.size()) - 1;

        fs::path sourceTilesDir = pyramidDir / "_source_tiles";
        fs::remove_all(sourceTilesDir, ec);
        ec.clear();
        fs::rename(sourceLevelPath, sourceTilesDir, ec);
        if (ec)
        {
            return fail("DeepZoom cannot prepare source tiles: " + ec.message(), level0Dir);
        }

        for (int level = 0; level <= maxLevel; ++level)
        {
            fs::path levelDir = pyramidDir / std::to_string(level);
            fs::remove_all(levelDir, ec);
            fs::create_directories(levelDir, ec);
            if (ec)
            {
                return fail("DeepZoom cannot create directory: " + ec.message(), pathToUtf8(levelDir));
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

        struct TileWriteFailure
        {
            std::string path;
        };
        using Clock = std::chrono::steady_clock;
        for (int level = maxLevel; level >= 0; --level)
        {
            const auto &current = levels[maxLevel - level];
            const int columns = DeepZoomLevel::tiles(current.width);
            const int rows = DeepZoomLevel::tiles(current.height);
            const size_t count = current.files();
            std::cout << "  DZI level " << level << ": " << columns << "x" << rows
                      << " tiles (" << current.width << "x" << current.height << ")" << std::flush;
            const auto started = benchmark ? Clock::now() : Clock::time_point{};
            auto writeTile = [&](size_t i)
            {
                const int c = static_cast<int>(i % columns), r = static_cast<int>(i / columns);
                const int x = c * dziTileSize, y = r * dziTileSize;
                const int w = std::min(dziTileSize, current.width - x);
                const int h = std::min(dziTileSize, current.height - y);
                cv::Mat tile;
                if (level == maxLevel)
                {
                    tile = renderFullRegion(x, y, w, h);
                }
                else
                {
                    const auto &previous = levels[maxLevel - level - 1];
                    const int srcX = x * 2, srcY = y * 2;
                    cv::Mat high = renderLevelRegion(level + 1, srcX, srcY,
                        std::min(w * 2, previous.width - srcX), std::min(h * 2, previous.height - srcY));
                    cv::resize(high, tile, cv::Size(w, h), 0, 0, cv::INTER_AREA);
                }
                const auto dst = pathToUtf8(pyramidDir / std::to_string(level) /
                                           (std::to_string(c) + "_" + std::to_string(r) + ".jpg"));
                try
                {
                    if (writeImage(dst, tile, {cv::IMWRITE_JPEG_QUALITY, quality}))
                    {
                        return;
                    }
                }
                catch (const cv::Exception &)
                {
                    // 编码异常与落盘失败走同一条路径，保留失败切片的文件名。
                }
                throw TileWriteFailure{dst};
            };
            try
            {
                if (count >= 16 && workers.size() > 1)
                {
                    // run 等到本层所有写入收束，下一层才能安全读取。
                    workers.run(count, writeTile);
                }
                else
                {
                    for (size_t i = 0; i < count; ++i)
                    {
                        writeTile(i);
                    }
                }
            }
            catch (const TileWriteFailure &error)
            {
                return fail("DeepZoom failed to write tile", error.path);
            }
            if (benchmark && runtime)
            {
                runtime->metric("deepZoomLevel" + std::to_string(level) + "Ms",
                    std::chrono::duration<double, std::milli>(Clock::now() - started).count());
            }
            std::cout << " (" << count << " tiles)" << std::endl;
        }

        // 所有层、manifest 和 viewer 均成功后才清理原始小块，失败时可从 _source_tiles 恢复。

        std::string dziPath = basePath + ".dzi";
        std::ofstream dzi(u8path(dziPath));
        if (!dzi)
        {
            return fail("DeepZoom cannot open file", dziPath);
        }
        dzi << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        dzi << "<Image xmlns=\"http://schemas.microsoft.com/deepzoom/2008\"\n";
        dzi << "       Format=\"jpg\" Overlap=\"0\" TileSize=\"" << dziTileSize << "\">\n";
        dzi << "  <Size Width=\"" << totalW << "\" Height=\"" << totalH << "\"/>\n";
        dzi << "</Image>\n";
        dzi.close();
        if (!dzi)
        {
            return fail("DeepZoom failed to write file", dziPath);
        }
        if (runtime)
        {
            runtime->artifact("manifest", "manifest", dziPath, "application/xml");
        }
        std::cout << "  DZI manifest: " << dziPath
                  << " (" << (maxLevel + 1) << " levels)" << std::endl;

        std::string htmlPath = basePath + ".html";
        std::ofstream html(u8path(htmlPath));
        if (!html)
        {
            return fail("DeepZoom cannot open file", htmlPath);
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
            return fail("DeepZoom failed to write file", htmlPath);
        }
        if (runtime)
        {
            runtime->artifact("viewer", "report", htmlPath, "text/html; charset=utf-8");
        }
        std::cout << "  HTML viewer: " << htmlPath << std::endl;
        fs::remove_all(sourceTilesDir, ec);
        return true;
    }
};

} // namespace mosaicraft
