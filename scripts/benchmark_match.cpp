#include "core/ApiRequestParser.h"
#include "core/MatchingAccess.h"
#include "core/MosaicService.h"
#include "core/UnicodeIO.h"
#include <fstream>
#include <iostream>
#include <iterator>

using namespace mosaicraft;

// 复用 API 配置以指定小输出 tile，不给生产 CLI 增加基准专用参数。
int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
    {
        std::cerr << "usage: benchmark_match <mosaic-request.json> [small|indices|small-indices]\n";
        return 1;
    }
    std::ifstream input(u8path(argv[1]));
    const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    MosaicRequest request;
    std::string error;
    if (!input || !parseMosaicRequestJson(body, request, error))
    {
        std::cerr << "invalid benchmark request: " << error << '\n';
        return 1;
    }
    request.config.benchmark = true;
    if (argc == 3)
    {
        const std::string profile = argv[2];
        if (profile != "small" && profile != "indices" && profile != "small-indices")
        {
            std::cerr << "unknown benchmark budget profile\n";
            return 1;
        }
        MosaicEngine engine;
        if (profile != "indices")
        {
            MatchingAccess::configure(engine, MatchingLimits{64ULL * 1024 * 1024, 64ULL * 1024 * 1024, 256});
        }
        std::vector<int> selected;
        if (profile != "small")
        {
            MatchingAccess::captureSelections(engine, selected);
        }
        if (!engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config))
        {
            return 1;
        }
        if (profile != "small")
        {
            std::ofstream output(u8path(request.outputPath + ".indices.bin"), std::ios::binary);
            output.write(reinterpret_cast<const char *>(selected.data()), selected.size() * sizeof(int));
            if (!output)
            {
                return 1;
            }
        }
        return 0;
    }
    const auto result = MosaicService().run(request);
    if (!result.ok)
    {
        std::cerr << result.message << '\n';
    }
    return result.exitCode;
}
