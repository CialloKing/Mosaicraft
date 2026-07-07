#pragma once

#include "MosaicService.h"

#include <string>

namespace mosaicraft
{

struct BuildRequest
{
    std::string inputDir;
    std::string outputDir = "library";
    std::string dbPath = "mosaicraft.db";
    int threads = 0;
    bool appendMode = false;
    bool normalizeOnly = false;
    bool forceMode = false;
    bool recursive = false;
    bool allowPrompt = false;
    int normalizeWidth = 180;
    int normalizeHeight = 320;
};

class BuildService
{
public:
    ServiceResult run(const BuildRequest& request) const;
};

} // namespace mosaicraft
