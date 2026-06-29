#include "core/Database.h"
#include "core/BuildService.h"
#include "core/DatabaseService.h"
#include "core/InspectService.h"
#include "core/MosaicEngine.h"
#include "core/MosaicService.h"
#include "core/UnicodeIO.h"
#include "core/Version.h"
#include "core/ConsoleGuard.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>

#include <ctime>

#include <algorithm>
#include <cctype>
#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <unistd.h>
#endif
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// 退出码
#define EXIT_OK          0
#define EXIT_ERR_GENERAL 1
#define EXIT_ERR_DB      2
#define EXIT_ERR_MEMORY  3
#define EXIT_ERR_GPU     4

static bool isKnownOptionToken(const std::string& token)
{
    static const char* kOptions[] = {
        "-i", "--input", "-o", "--output", "-d", "--db", "-t", "--threads",
        "--append", "-r", "--recursive", "--normalize-size", "--normalize-only",
        "-y", "--yes", "--force", "-h", "--help",
        "--tile-w", "--tile-h", "-W", "--out-w", "-H", "--out-h",
        "--lab-weight", "--grid-weight", "--tiny-weight", "--edge-weight",
        "--lbp-weight", "--penalty", "--l-range", "--candidates",
        "--topn-random", "-f", "--format", "--cpu", "-q", "--quality",
        "--png-level", "--write-mode", "--tiled", "--deepzoom",
        "--neighbor-window", "--neighbor-penalty", "-U", "--upscale",
        "--output-tile", "--color-adjust", "--adaptive-weights", "--analyze",
        "--color-strength", "--benchmark", "-n", "--unused", "--export"
    };
    for (const char* option : kOptions)
    {
        if (token == option)
            return true;
    }
    return false;
}

static bool readOptionValue(int argc, char* argv[], int& i, const std::string& option, const char*& value)
{
    if (i + 1 >= argc || isKnownOptionToken(argv[i + 1]))
    {
        std::cerr << "ERROR: " << option << " requires a value." << std::endl;
        return false;
    }
    value = argv[++i];
    return true;
}

// 带数值校验的版本 — 检测 "abc" 或 "12abc" 等非法输入
static bool readOptionInt(int argc, char* argv[], int& i, const std::string& option, int& out)
{
    const char* raw = nullptr;
    if (!readOptionValue(argc, argv, i, option, raw)) return false;
    char* end = nullptr;
    long v = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0') {
        std::cerr << "ERROR: " << option << " expects an integer, got: " << raw << std::endl;
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

static bool readOptionDouble(int argc, char* argv[], int& i, const std::string& option, double& out)
{
    const char* raw = nullptr;
    if (!readOptionValue(argc, argv, i, option, raw)) return false;
    char* end = nullptr;
    double v = std::strtod(raw, &end);
    if (end == raw || *end != '\0') {
        std::cerr << "ERROR: " << option << " expects a number, got: " << raw << std::endl;
        return false;
    }
    out = v;
    return true;
}

namespace fs = std::filesystem;

using namespace mosaicraft;

// ============================================================
// 工具
// ============================================================

static void printHelp()
{
    std::cout << R"(Mosaicraft — Image mosaic generator

Usage:
  mosaicraft build    [options]    Build image database
  mosaicraft mosaic   [options]    Create mosaic from target image
  mosaicraft inspect  [options]    Inspect image features
  mosaicraft db-stats [options]    Show database statistics
  mosaicraft db-purge [options]    Remove orphan records
  mosaicraft db-health [options]   Database health diagnostics
  mosaicraft db-usage [options]    Image usage statistics

Build options:
  -i, --input  <dir>     Source image directory (required)
  -o, --output <dir>     Output directory (default: library, DB stored inside)
  -d, --db     <path>    Database path (default: <output>/mosaicraft.db)
  -t, --threads <n>      Worker threads (default: auto)
      --normalize-size <WxH>  Normalized image size (default: 180x320)
      --append           Append mode: add new images without rebuilding
  -r, --recursive        Scan subdirectories for source images
      --normalize-only   Only normalize images, don't build database

Mosaic options:
  -i, --input  <path>    Target image to mosaicify (required)
  -d, --db     <path>    Database path (default: <output>/mosaicraft.db)
  -o, --output <path>    Output path or directory (default: output/output.jpg)
      --tile-w     <n>   Tile width in pixels (default: 9)
      --tile-h     <n>   Tile height in pixels (default: 16)
      --candidates <n>   ANN query candidates (default: 150)
      --format     <ext> jpg, png, webp, tiff (default: from extension)
      --quality    <n>   JPEG/WebP quality 1-100 (default: 100)
      --png-level  <n>   PNG compression 1-9 (default: 1=fastest)
      --write-mode <m>   Write mode: auto/stream/batch (default: auto)
      --out-w      <n>   Target output width in pixels
      --out-h      <n>   Target output height in pixels
      --upscale    <n>   Upscale target nX before tiling
      --output-tile <WxH>  Output tile pixel size (default: 180x320)
      --l-range     <f>  L brightness search range (default: 20)
      --topn-random <n>  Top-N random selection (default: 10)
      --neighbor-window <n>  Neighborhood window (default: 0=auto)
      --neighbor-penalty <f> Base penalty value (default: 100)
      --tiled            Output tiles as separate files (no size limit)
      --deepzoom         Generate Deep Zoom pyramid + HTML viewer
      --color-adjust     Enable per-tile color jitter (off by default)
      --color-strength <v>  Color jitter intensity (default: 0.04)
      --adaptive-weights  Enable adaptive feature weights
      --lab-weight   <w> LAB distance weight (default: 0.20)
      --grid-weight  <w> Grid 8x8 distance weight (default: 0.45)
      --tiny-weight  <w> Tiny feature weight (default: 0.25)
      --edge-weight  <w> Edge density weight (default: 0.05)
      --lbp-weight   <w> LBP histogram weight (default: 0.05)
      --penalty      <w> Reuse penalty per use (default: 0.01)
      --analyze          Generate quality analysis report
      --benchmark        Show phase timing
      --cpu              Force CPU, no GPU acceleration

Output modes (choose one):
  -o, --output <path>   Single image output (default)
      --tiled            Tile set output (one file per tile)
      --deepzoom         Deep Zoom pyramid + HTML viewer (implies --tiled)

Common options:
  -h, --help             Show this help

Inspect options:
  -i, --input  <path>    Image to inspect (required)
  -d, --db     <path>    Database path (default: <output>/mosaicraft.db)

Exit status:
  0  success
  1  general error (bad path, invalid arguments)
  2  database error (locked, corrupt, or missing)
  3  memory / allocation failure
  4  GPU / CUDA error
)";
}

// ============================================================
// build 子命�?
// ============================================================
static int cmdBuild(int argc, char* argv[])
{
    std::string inputDir;
    std::string outputDir = "library";
    std::string dbPath = "mosaicraft.db";  // 哨兵值，见下方替换
    int threads = 0;
    bool appendMode = false;
    bool normOnly = false;
    bool forceMode = false;
    bool recursive = false;
    int normW = 180;
    int normH = 320;

    // 解析参数
    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-i" || arg == "--input")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            inputDir = value;
        }
        else if (arg == "-o" || arg == "--output")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            outputDir = value;
        }
        else if (arg == "-d" || arg == "--db")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            dbPath = value;
        }
        else if (arg == "-t" || arg == "--threads")
        {
            int _v = 0;
            if (!readOptionInt(argc, argv, i, arg, _v)) return 1;
            threads = _v;
            if (threads < 0)
            {
                threads = 0;
            }
        }
        else if (arg == "--append")
        {
            appendMode = true;
        }
        else if (arg == "-r" || arg == "--recursive")
        {
            recursive = true;
        }
        else if (arg == "--normalize-size")
        {
            const char* val = nullptr;
            if (!readOptionValue(argc, argv, i, arg, val)) return 1;
            const char* sep = strchr(val, 'x');
            if (sep && sep != val && *(sep+1) != '\0' && !strchr(sep+1, 'x')) {
                normW = std::max(1, std::atoi(val));
                normH = std::max(1, std::atoi(sep + 1));
            } else {
                std::cerr << "ERROR: --normalize-size expected WxH format (e.g. 180x320), got: " << val << std::endl;
                return 1;
            }
        }
        else if (arg == "--normalize-only")
        {
            normOnly = true;
        }
        else if (arg == "-y" || arg == "--yes" || arg == "--force")
        {
            forceMode = true;
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: mosaicraft build -i <dir> [-o <dir>] [-d <db>] [-t <n>] [--append] [-r] [--normalize-only]" << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << std::endl;
            return 1;
        }
    }

    if (inputDir.empty())
    {
        std::cerr << "ERROR: --input is required for build." << std::endl;
        return 1;
    }

    BuildRequest request;
    request.inputDir = inputDir;
    request.outputDir = outputDir;
    request.dbPath = dbPath;
    request.threads = threads;
    request.appendMode = appendMode;
    request.normalizeOnly = normOnly;
    request.forceMode = forceMode;
    request.recursive = recursive;
    request.allowPrompt = true;
    request.normalizeWidth = normW;
    request.normalizeHeight = normH;

    BuildService service;
    ServiceResult result = service.run(request);
    if (!result.ok && !result.message.empty())
    {
        std::cerr << "ERROR: " << result.message << std::endl;
    }
    return result.exitCode;
}

// ============================================================
// mosaic 子命�?
// ============================================================
static int cmdMosaic(int argc, char* argv[])
{
    std::string inputPath;
    std::string dbPath = "library/mosaicraft.db";
    std::string outputPath = "output/output.jpg";
    MosaicEngine::Config cfg;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-i" || arg == "--input")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            inputPath = value;
        }
        else if (arg == "-d" || arg == "--db")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            dbPath = value;
        }
        else if (arg == "-o" || arg == "--output")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            outputPath = value;
        }
        else if (arg == "--tile-w")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            cfg.tileW = std::max(4, std::atoi(value));
        }
        else if (arg == "--tile-h")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            cfg.tileH = std::max(4, std::atoi(value));
        }
        else if (arg == "-W" || arg == "--out-w")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            cfg.outW = std::max(1, std::atoi(value));
        }
        else if (arg == "-H" || arg == "--out-h")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            cfg.outH = std::max(1, std::atoi(value));
        }
        else if (arg == "--lab-weight")
        {
            double _v = 0.0;
            if (!readOptionDouble(argc, argv, i, arg, _v)) return 1;
            cfg.labWeight = _v;
        }
        else if (arg == "--grid-weight")
        {
            double _v = 0.0;
            if (!readOptionDouble(argc, argv, i, arg, _v)) return 1;
            cfg.gridWeight = _v;
        }
        else if (arg == "--tiny-weight")
        {
            double _v = 0.0;
            if (!readOptionDouble(argc, argv, i, arg, _v)) return 1;
            cfg.tinyWeight = _v;
        }
        else if (arg == "--edge-weight")
        {
            double _v = 0.0;
            if (!readOptionDouble(argc, argv, i, arg, _v)) return 1;
            cfg.edgeWeight = _v;
        }
        else if (arg == "--lbp-weight")
        {
            double _v = 0.0;
            if (!readOptionDouble(argc, argv, i, arg, _v)) return 1;
            cfg.lbpWeight = _v;
        }
        else if (arg == "--penalty")
        {
            double _v = 0.0;
            if (!readOptionDouble(argc, argv, i, arg, _v)) return 1;
            cfg.usePenalty = _v;
        }
        else if (arg == "--l-range")
        {
            double _v = 0.0;
            if (!readOptionDouble(argc, argv, i, arg, _v)) return 1;
            cfg.lRange = _v;
        }
        else if (arg == "--candidates")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            cfg.candidates = std::max(10, std::atoi(value));
        }
        else if (arg == "--topn-random")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            int v = std::atoi(value);
            cfg.topNrandom = std::max(1, v);
        }
        else if (arg == "-f" || arg == "--format")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            cfg.outputFormat = value;
            // 规范化: jpeg → jpg, tif → tiff
            if (cfg.outputFormat == "jpeg") cfg.outputFormat = "jpg";
            else if (cfg.outputFormat == "tif") cfg.outputFormat = "tiff";
            cfg.formatExplicit = true;
        }
        else if (arg == "--cpu")
        {
            cfg.useGpu = false;
        }
        else if (arg == "-q" || arg == "--quality")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            int q = std::atoi(value);
            cfg.jpegQuality = std::max(1, std::min(100, q));
        }
        else if (arg == "--png-level")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            int lvl = std::atoi(value);
            cfg.pngCompressionLevel = std::max(1, std::min(9, lvl));
        }
        else if (arg == "--write-mode")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            std::string mode = value;
            if (mode == "auto" || mode == "stream" || mode == "batch")
                cfg.writeMode = mode;
            else {
                std::cerr << "ERROR: --write-mode must be auto|stream|batch, got: " << mode << std::endl;
                return 1;
            }
        }
        else if (arg == "--tiled")
        {
            cfg.tiledOutput = true;
        }
        else if (arg == "--deepzoom")
        {
            cfg.tiledOutput = true;   // deepzoom 隐含 tiled
            cfg.deepZoom = true;
        }
        else if (arg == "--neighbor-window")
        {
            int _v = 0;
            if (!readOptionInt(argc, argv, i, arg, _v)) return 1;
            cfg.neighborWindow = _v;
        }
        else if (arg == "--neighbor-penalty")
        {
            double _v = 0.0;
            if (!readOptionDouble(argc, argv, i, arg, _v)) return 1;
            cfg.neighborPenalty = _v;
        }
        else if (arg == "-U" || arg == "--upscale")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            cfg.upscale = std::max(1, std::atoi(value));
        }
        else if (arg == "--output-tile")
        {
            // 格式: 180x320
            const char* val = nullptr;
            if (!readOptionValue(argc, argv, i, arg, val)) return 1;
            const char* sep = strchr(val, 'x');
            if (sep && sep != val && *(sep+1) != '\0' && !strchr(sep+1, 'x')) {
                cfg.nativeTileW = std::max(1, std::atoi(val));
                cfg.nativeTileH = std::max(1, std::atoi(sep + 1));
            } else {
                std::cerr << "ERROR: --output-tile expected WxH format (e.g. 180x320), got: " << val << std::endl;
                return 1;
            }
        }
        else if (arg == "--color-adjust")
        {
            cfg.colorAdjust = true;
        }
        else if (arg == "--adaptive-weights")
        {
            cfg.adaptiveWeights = true;
        }
        else if (arg == "--analyze")
        {
            cfg.analyze = true;
        }
        else if (arg == "--color-strength")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            double v = std::atof(value);
            cfg.colorStrength = std::max(0.0, std::min(0.5, v));
        }
        else if (arg == "--benchmark")
        {
            cfg.benchmark = true;
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: mosaicraft mosaic -i <image> -d <db> [options]" << std::endl;
            std::cout << "  -i, --input <path>    Target image (required)" << std::endl;
            std::cout << "  -d, --db <path>       Database (default: <output>/mosaicraft.db)" << std::endl;
            std::cout << "  -o, --output <path>   Output path (default: output/output.jpg)" << std::endl;
            std::cout << "  --tile-w <n>          Tile width (default: 9, min: 4)" << std::endl;
            std::cout << "  --tile-h <n>          Tile height (default: 16, min: 4)" << std::endl;
            std::cout << "  --candidates <n>      ANN query candidates (default: 150)" << std::endl;
            std::cout << "  --format <ext>        jpg / png / webp / tiff (default: from extension)" << std::endl;
            std::cout << "  --quality <n>         JPEG/WebP quality 1-100 (default: 100)" << std::endl;
            std::cout << "  --png-level <n>       PNG compression 1-9 (default: 1=fastest)" << std::endl;
            std::cout << "  --write-mode <m>      auto / stream / batch (default: auto)" << std::endl;
            std::cout << "  --out-w <n>           Target output width (0=auto)" << std::endl;
            std::cout << "  --out-h <n>           Target output height (0=auto)" << std::endl;
            std::cout << "  --upscale <n>         Upscale target nX before tiling" << std::endl;
            std::cout << "  --output-tile <WxH>   Output tile pixel size (default: 180x320)" << std::endl;
            std::cout << "  --l-range <f>         L brightness search range (default: 20)" << std::endl;
            std::cout << "  --topn-random <n>     Pick from top-N for variety (default: 10)" << std::endl;
            std::cout << "  --neighbor-window <n> Neighborhood window (default: 0=auto)" << std::endl;
            std::cout << "  --neighbor-penalty <f> Base penalty value (default: 100)" << std::endl;
            std::cout << "  --lab-weight <f>      LAB weight (default: 0.20)" << std::endl;
            std::cout << "  --grid-weight <f>     Grid weight (default: 0.45)" << std::endl;
            std::cout << "  --tiny-weight <f>     TinyImage weight (default: 0.25)" << std::endl;
            std::cout << "  --edge-weight <f>     Edge density weight (default: 0.05)" << std::endl;
            std::cout << "  --lbp-weight <f>      LBP histogram weight (default: 0.05)" << std::endl;
            std::cout << "  --penalty <f>         Use-count penalty (default: 0.01)" << std::endl;
            std::cout << "  --adaptive-weights    Enable adaptive feature weights" << std::endl;
            std::cout << "  --color-adjust        Enable per-tile color jitter (off by default)" << std::endl;
            std::cout << "  --color-strength <f>  Color jitter intensity (default: 0.04)" << std::endl;
            std::cout << "  --analyze             Generate quality report + heatmap + worst tiles" << std::endl;
            std::cout << "  --benchmark           Print per-phase timing breakdown" << std::endl;
            std::cout << "  --cpu                 Force CPU (no GPU)" << std::endl;
            std::cout << "  --tiled               Output tiles as separate files (no size limit)" << std::endl;
            std::cout << "  --deepzoom            Generate Deep Zoom pyramid + HTML viewer" << std::endl;
            std::cout << std::endl;
            std::cout << "  Output modes (choose one): -o (single), --tiled, --deepzoom" << std::endl;
            std::cout << "  Exit status: 0=success 1=param/IO 2=database 3=memory 4=GPU" << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << std::endl;
            return 1;
        }
    }

    if (inputPath.empty())
    {
        std::cerr << "ERROR: --input is required for mosaic." << std::endl;
        return 1;
    }

    // 输出模式互斥检测：单图输出不能同时指定分块/金字塔
    if (cfg.tiledOutput || cfg.deepZoom)
    {
        // 分块模式可以指定 -o 作为输出目录前缀
        // 不报错，但提醒用户
        if (!outputPath.empty() && outputPath.find('_') == std::string::npos)
            std::cout << "  Note: --tiled/--deepzoom outputs to " << outputPath << "_files/" << std::endl;
    }

    // 输出路径校验：拒绝已存在的目录，提示应为文件路径
    std::error_code ec;
    if (!outputPath.empty() && fs::is_directory(u8path(outputPath), ec)) {
        std::cerr << "ERROR: -o is a directory, not a file path: " << outputPath << std::endl;
        std::cerr << "  Hint: append a filename, e.g. " << outputPath << "/output.jpg" << std::endl;
        std::cerr << "  Note: --format overrides the file extension from -o" << std::endl;
        return 1;
    }

    MosaicRequest request;
    request.inputPath = inputPath;
    request.dbPath = dbPath;
    request.outputPath = outputPath;
    request.config = cfg;

    MosaicService service;
    ServiceResult result = service.run(request);
    if (!result.ok && !result.message.empty())
    {
        std::cerr << "ERROR: " << result.message << std::endl;
    }
    return result.exitCode;
}

// ============================================================
// inspect 子命�?�?特征诊断
// ============================================================
static int cmdInspect(int argc, char* argv[])
{
    std::string imagePath;
    std::string dbPath = "library/mosaicraft.db";

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-i" || arg == "--input")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            imagePath = value;
        }
        else if (arg == "-d" || arg == "--db")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            dbPath = value;
        }
    }

    if (imagePath.empty())
    {
        std::cerr << "Usage: mosaicraft inspect -i <image> [-d <db>]" << std::endl;
        return 1;
    }

    InspectService service;
    auto result = service.inspect({imagePath, dbPath});
    if (!result.status.ok)
    {
        std::cerr << "ERROR: " << result.status.message << std::endl;
        return result.status.exitCode;
    }
    const auto& info = result;
    std::cout << "Image: " << info.imagePath << " (" << info.width << "x" << info.height << ")" << std::endl;

    std::cout << "\nAvgLAB:  L=" << std::fixed << std::setprecision(1) << info.avgL
              << "  A=" << info.avgA << "  B=" << info.avgB << std::endl;
    std::cout << "Edge density: " << std::setprecision(4) << info.edgeDensity << std::endl;
    std::cout << "LBP entropy:  " << std::setprecision(4) << info.lbpEntropy << std::endl;

    if (info.databaseAvailable)
    {
        std::cout << "\nDatabase: " << info.databaseTotal << " images" << std::endl;
        std::cout << "L-range [" << info.candidateMinL << ", " << info.candidateMaxL << "]: "
                  << info.candidateCount << " candidates"
                  << (info.databaseTotal > 0 ? " (" + std::to_string(100 * info.candidateCount / info.databaseTotal) + "%)" : "")
                  << std::endl;
        std::cout << "Library L range: [" << std::setprecision(1) << info.libraryMinL
                  << ", " << info.libraryMaxL << "]  avg=" << info.libraryAvgL << std::endl;
        std::cout << "Distribution: dark=" << info.libraryDark << " (" << (info.databaseTotal > 0 ? 100.0*info.libraryDark/info.databaseTotal : 0)
                  << "%)  mid=" << info.libraryMid << " (" << (info.databaseTotal > 0 ? 100.0*info.libraryMid/info.databaseTotal : 0)
                  << "%)  bright=" << info.libraryBright << " (" << (info.databaseTotal > 0 ? 100.0*info.libraryBright/info.databaseTotal : 0) << "%)" << std::endl;
    }

    return 0;
}

// ============================================================
// db-stats 子命令：输出图库统�??信息
// ============================================================
static int cmdDbStats(int argc, char* argv[])
{
    std::string dbPath = "library/mosaicraft.db";

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-d" || arg == "--db")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            dbPath = value;
        }
    }

    DatabaseService service;
    auto result = service.stats({dbPath});
    if (!result.status.ok)
    {
        std::cerr << "ERROR: " << result.status.message << std::endl;
        return result.status.exitCode;
    }

    const auto& stats = result.stats;
    int total = stats.total;
    if (stats.empty)
    {
        std::cout << "Database is empty." << std::endl;
        return 0;
    }

    if (!stats.featureWidth.empty() && !stats.featureHeight.empty())
        std::cout << "Normalized size: " << stats.featureWidth << "x" << stats.featureHeight << std::endl;

    std::cout << "=== Database Statistics ===\n";
    std::cout << "  Images: " << total << "\n";
    std::cout << "  Grid dim: " << stats.gridDim << " (" << (stats.gridDim/3) << " cells)\n";
    std::cout << "  LAB: L[" << std::fixed << std::setprecision(1) << stats.minL
              << "," << stats.maxL << "] avg=" << stats.avgL
              << "  A[" << stats.minA << "," << stats.maxA << "] avg=" << stats.avgA
              << "  B[" << stats.minB << "," << stats.maxB << "] avg=" << stats.avgB << "\n";
    std::cout << "  Brightness: dark=" << stats.dark << " (" << (100.0*stats.dark/total)
              << "%) mid=" << stats.mid << " (" << (100.0*stats.mid/total)
              << "%) bright=" << stats.bright << " (" << (100.0*stats.bright/total) << "%)\n";

    std::cout << "  L-histogram:\n";
    for (const auto& bin : stats.lHistogram)
    {
        std::string bar(bin.count * 50 / total, '#');
        std::cout << "    " << std::setw(3) << bin.lo << "-" << std::setw(3) << bin.hi
                  << " | " << bar << " " << bin.count << "\n";
    }

    std::cout << "  Coverage gaps:";
    if (stats.coverageGaps.empty()) {
        std::cout << " none significant";
    } else {
        for (const auto& gap : stats.coverageGaps) std::cout << " " << gap;
    }
    std::cout << "\n";

    return 0;
}

// ============================================================
// db-purge 子命令：清除归一化目录中不存在的孤儿记录
static int cmdDbPurge(int argc, char* argv[])
{
    std::string dbPath = "library/mosaicraft.db";
    bool forceMode = false;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-d" || arg == "--db")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            dbPath = value;
        }
        else if (arg == "-y" || arg == "--yes" || arg == "--force")
            forceMode = true;
    }

    DatabaseService service;
    auto preview = service.purge({dbPath, true, false});
    if (!preview.status.ok)
    {
        std::cerr << "ERROR: " << preview.status.message << std::endl;
        return preview.status.exitCode;
    }

    if (preview.purge.orphanCount == 0)
    {
        std::cout << "No orphan records found." << std::endl;
        return 0;
    }

    if (!forceMode)
    {
#ifdef _WIN32
        bool isTTY = _isatty(_fileno(stdin));
#else
        bool isTTY = isatty(fileno(stdin));
#endif
        if (!isTTY)
        {
            std::cerr << "ERROR: " << preview.purge.orphanCount << " orphan records. Use -y to purge." << std::endl;
            return 1;
        }
        std::cout << "Found " << preview.purge.orphanCount << " orphan records. Purge? [y/N] " << std::flush;
        char answer = static_cast<char>(std::getchar());
        if (answer != 'y' && answer != 'Y') { std::cout << "Aborted." << std::endl; return 0; }
    }

    auto result = service.purge({dbPath, false, true});
    if (!result.status.ok && result.purge.failedCount == 0)
    {
        std::cerr << "ERROR: " << result.status.message << std::endl;
        return result.status.exitCode;
    }

    std::cout << "Purged " << result.purge.removedCount << " / " << result.purge.total << " orphan records." << std::endl;
    for (const auto& recommendation : result.purge.recommendations)
        std::cout << "  Note: " << recommendation << std::endl;
    for (const auto& error : result.purge.errors)
        std::cerr << "  ERROR: " << error << std::endl;
    return result.status.ok ? 0 : result.status.exitCode;
}

// ============================================================
// db-usage ������鿴ͼƬʹ��ͳ��
// ============================================================
static int cmdDbUsage(int argc, char* argv[])
{
    std::string dbPath = "library/mosaicraft.db";
    int limit = 50;
    std::string exportDir;
    bool showUnused = false;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-d" || arg == "--db")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            dbPath = value;
        }
        else if (arg == "-n")
        {
            int _v = 0;
            if (!readOptionInt(argc, argv, i, arg, _v)) return 1;
            limit = _v;
        }
        else if (arg == "--unused")
            showUnused = true;
        else if (arg == "--export" || arg == "-o")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            exportDir = value;
        }
    }

    DatabaseService usageService;
    auto usageResult = usageService.usage({dbPath, limit, showUnused});
    if (!usageResult.status.ok)
    {
        std::cerr << "ERROR: " << usageResult.status.message << std::endl;
        return usageResult.status.exitCode;
    }

    const auto& usage = usageResult.usage;
    if (usage.top.empty())
    {
        std::cout << "No usage data yet. Run a mosaic with --analyze to start tracking." << std::endl;
        return 0;
    }

    std::cout << "Top " << usage.top.size() << " most used images (by mosaic runs):\n";
    std::cout << "  Rank  Image ID    Runs    Tiles\n";
    for (size_t i = 0; i < usage.top.size(); ++i)
    {
        const auto& item = usage.top[i];
        std::cout << "  " << std::setw(4) << (i+1) << "  "
                  << std::setw(8) << item.id << "  "
                  << std::setw(6) << item.runs << "  "
                  << std::setw(8) << item.tiles << "\n";
    }

    // --unused: �г���δʹ�õ�ͼƬ
    if (showUnused)
    {
        for (const auto& item : usage.unusedPreview)
        {
            std::cout << "  unused: id=" << item.id << "  " << item.filePath << "\n";
        }
        std::cout << "\n  Total unused: " << usage.unusedCount << " / " << usage.total
                  << " (" << std::fixed << std::setprecision(1) << (usage.total > 0 ? 100.0*usage.unusedCount/usage.total : 0) << "%)\n";
        if (usage.unusedCount > 0 && exportDir.empty())
            std::cout << "  Tip: use --export <dir> to export used images\n";
    }

    // ��������ȫ�� tile ʹ�������򣬸��ƹ�һ��ͼ������Ŀ¼
    if (!exportDir.empty())
    {
        auto exportResult = usageService.exportUsage({dbPath, exportDir, true});
        if (!exportResult.status.ok && exportResult.exportInfo.failedCount == 0)
        {
            std::cerr << "ERROR: " << exportResult.status.message << std::endl;
            return exportResult.status.exitCode;
        }
        for (const auto& error : exportResult.exportInfo.errors)
            std::cerr << "  WARN: " << error << "\n";
        std::cout << "\n  Exported " << exportResult.exportInfo.exportedCount << " images to " << exportDir << "\n";
        if (exportResult.exportInfo.skippedCount > 0 || exportResult.exportInfo.failedCount > 0)
            std::cout << "  Skipped " << exportResult.exportInfo.skippedCount
                      << ", failed " << exportResult.exportInfo.failedCount << "\n";
        if (!exportResult.status.ok)
            return exportResult.status.exitCode;
    }
    return 0;
}

// ============================================================
// db-health �����ͼ�⽡�������
// ============================================================
static int cmdDbHealth(int argc, char* argv[])
{
    std::string dbPath = "library/mosaicraft.db";
    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-d" || arg == "--db")
        {
            const char* value = nullptr;
            if (!readOptionValue(argc, argv, i, arg, value)) return 1;
            dbPath = value;
        }
    }

    DatabaseService service;
    auto result = service.health({dbPath});
    if (!result.status.ok)
    {
        std::cerr << "ERROR: " << result.status.message << std::endl;
        return result.status.exitCode;
    }

    const auto& health = result.health;
    int total = health.total;
    if (health.empty) { std::cout << "Database is empty.\n"; return 0; }

    std::cout << "=== Database Health ===\n\n";
    std::cout << "Images: " << total << "\n";

    std::cout << "\n--- Brightness Coverage ---\n";
    std::cout << "  Dark  (<50L):  " << std::fixed << std::setprecision(1) << health.darkPct << "% (" << health.dark << ")\n";
    std::cout << "  Mid   (50-150L): " << health.midPct << "% (" << health.mid << ")\n";
    std::cout << "  Bright(>150L): " << health.brightPct << "% (" << health.bright << ")\n";
    for (const auto& warning : health.warnings)
    {
        if (warning.find("Dark") != std::string::npos || warning.find("Mid") != std::string::npos)
            std::cout << "  WARN: " << warning << "\n";
    }

    std::cout << "\n--- Color Gamut ---\n";
    std::cout << "  A (green-red): " << std::setprecision(0) << health.minA << "-" << health.maxA << "\n";
    std::cout << "  B (blue-yellow): " << std::setprecision(0) << health.minB << "-" << health.maxB << "\n";
    for (const auto& warning : health.warnings)
    {
        if (warning.find("Lacking") != std::string::npos)
            std::cout << "  WARN: " << warning << "\n";
    }

    std::cout << "\n--- Usage ---\n";
    std::cout << "  Used:   " << health.usedCount << " (" << std::setprecision(1) << health.usedPct << "%)\n";
    std::cout << "  Unused: " << health.unusedCount << " (" << health.unusedPct << "%)\n";
    for (const auto& warning : health.warnings)
    {
        if (warning.find("never used") != std::string::npos)
            std::cout << "  WARN: " << warning << "\n";
    }

    if (health.topHotspotCount > 0) {
        std::cout << "\n--- Hotspot Concentration ---\n";
        std::cout << "  Top 1% (" << health.topHotspotCount << " images): " << std::setprecision(1)
                  << health.topHotspotTilePct << "% of all tiles\n";
        for (const auto& warning : health.warnings)
        {
            if (warning.find("dominates") != std::string::npos)
                std::cout << "  WARN: " << warning << "\n";
        }
    }

    std::cout << "\n--- Recommendations ---\n";
    if (health.recommendations.empty()) {
        std::cout << "  Database looks well-balanced!\n";
    } else {
        for (const auto& rec : health.recommendations) std::cout << "  + " << rec << "\n";
    }

    return 0;
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[])
{
    srand(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(reinterpret_cast<uintptr_t>(&argc)));

    // Windows: 禁用控制台快速选择模式，防止鼠标误触导致程序挂起
    // Guard 析构时自动恢复原模式
    ConsoleQuickEditGuard quickEditGuard;

    // 交互式终端退出前暂停，双击exe时不闪退
    // 注意：仅当stdin和stdout都是TTY时才暂停（popen管道不会触发）
    struct AutoPause {
        ~AutoPause() {
#ifdef _WIN32
            if (_isatty(_fileno(stdin)) && _isatty(_fileno(stdout))) {
                std::cout << std::endl << "Press any key to exit..." << std::flush;
                _getch();
            }
#else
            if (isatty(fileno(stdin)) && isatty(fileno(stdout))) {
                std::cout << std::endl << "Press any key to exit..." << std::flush;
                getchar();
            }
#endif
        }
    } autoPause;

    // Windows: argv ʹ��ϵͳ ANSI ���루����ϵͳΪ GBK����תΪ UTF-8 ԭ���滻
    std::vector<std::string> utf8Args(argc);
    for (int i = 0; i < argc; ++i)
    {
        utf8Args[i] = localToUtf8(argv[i]);
        argv[i] = &utf8Args[i][0];
    }
    if (argc < 2)
    {
        printHelp();
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "-V" || cmd == "--version")
    {
        std::cout << "Mosaicraft " << mosaicraft::kVersion << "\n";
        return 0;
    }
    if (cmd == "build")
    {
        return cmdBuild(argc, argv);
    }
    else if (cmd == "mosaic")
    {
        return cmdMosaic(argc, argv);
    }
    else if (cmd == "inspect")
    {
        return cmdInspect(argc, argv);
    }
    else if (cmd == "db-stats")
    {
        return cmdDbStats(argc, argv);
    }
    else if (cmd == "db-purge")
    {
        return cmdDbPurge(argc, argv);
    }
    else if (cmd == "db-usage")
    {
        return cmdDbUsage(argc, argv);
    }
    else if (cmd == "db-health")
    {
        return cmdDbHealth(argc, argv);
    }
    else if (cmd == "-h" || cmd == "--help")
    {
        printHelp();
        return 0;
    }
    else
    {
        std::cerr << "Unknown command: " << cmd << std::endl;
        std::cerr << "Use 'mosaicraft --help' for usage." << std::endl;
        return 1;
    }
}
