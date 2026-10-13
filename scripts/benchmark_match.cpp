#include "core/ApiRequestParser.h"
#include "core/MosaicService.h"
#include "core/UnicodeIO.h"
#include <fstream>
#include <iostream>
#include <iterator>

using namespace mosaicraft;

// 复用 API 配置以指定小输出 tile，不给生产 CLI 增加基准专用参数。
int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: benchmark_match <mosaic-request.json>\n";
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
    const auto result = MosaicService().run(request);
    if (!result.ok)
    {
        std::cerr << result.message << '\n';
    }
    return result.exitCode;
}
