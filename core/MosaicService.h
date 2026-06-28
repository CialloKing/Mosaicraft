#pragma once

#include "MosaicEngine.h"

#include <string>

namespace mosaicraft
{

struct MosaicRequest
{
    std::string inputPath;
    std::string dbPath = "library/mosaicraft.db";
    std::string outputPath = "output/output.jpg";
    MosaicEngine::Config config;
};

struct ServiceResult
{
    bool ok = false;
    int exitCode = 1;
    std::string message;

    static ServiceResult success(const std::string& msg = {})
    {
        return {true, 0, msg};
    }

    static ServiceResult failure(int code, const std::string& msg)
    {
        return {false, code, msg};
    }
};

class MosaicService
{
public:
    ServiceResult run(const MosaicRequest& request) const;
};

std::string resolveDbPathForService(const std::string& rawPath);

} // namespace mosaicraft
