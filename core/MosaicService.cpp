#include "MosaicService.h"

#include "UnicodeIO.h"

#include <filesystem>

namespace mosaicraft
{

std::string resolveDbPathForService(const std::string& rawPath)
{
    std::error_code ec;
    if (std::filesystem::is_directory(u8path(rawPath), ec))
    {
        return rawPath + "/mosaicraft.db";
    }
    return rawPath;
}

ServiceResult MosaicService::run(const MosaicRequest& request) const
{
    if (request.inputPath.empty())
    {
        return ServiceResult::failure(1, "inputPath is required");
    }
    if (request.dbPath.empty())
    {
        return ServiceResult::failure(1, "dbPath is required");
    }
    if (request.outputPath.empty())
    {
        return ServiceResult::failure(1, "outputPath is required");
    }

    std::error_code ec;
    if (std::filesystem::is_directory(u8path(request.outputPath), ec))
    {
        return ServiceResult::failure(1, "outputPath is a directory, expected a file path");
    }

    MosaicEngine engine;
    bool ok = engine.generate(
        request.inputPath,
        resolveDbPathForService(request.dbPath),
        request.outputPath,
        request.config);

    return ok
        ? ServiceResult::success("mosaic generated")
        : ServiceResult::failure(1, "mosaic generation failed");
}

} // namespace mosaicraft
