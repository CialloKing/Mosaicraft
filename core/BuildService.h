#pragma once

#include "LibraryEncodingOptions.h"
#include "MosaicService.h"
#include "SmartCrop.h"

#include <string>

namespace mosaicraft
{

struct BuildRequest
{
    std::string inputDir;
    std::string outputDir = "library";
    std::string dbPath;
    int threads = 0;
    BackendPolicy backend = BackendPolicy::Auto;
    std::string device = "auto";
    bool appendMode = false;
    bool normalizeOnly = false;
    CropMode cropMode = CropMode::Center;
    std::optional<CropDevice> cropDevice;
    bool forceMode = false;
    bool recursive = false;
    bool allowPrompt = false;
    int normalizeWidth = 180;
    int normalizeHeight = 320;
    bool resourceMonitor = false;
    bool benchmark = false;
    LibraryEncodingOptions encoding;
};

class BuildService
{
public:
    ServiceResult run(const BuildRequest& request) const;
    ServiceResult run(const BuildRequest &request, RuntimeContext &runtime) const;
};

} // namespace mosaicraft
