#pragma once
#include "MosaicOutput.h"
#include "MosaicService.h"
#include "json.hpp"

namespace mosaicraft
{
struct ProjectRequest
{
    std::string projectPath, outputPath;
    MosaicEngine::Config config;
    bool deep = false, overwrite = false, resourceMonitor = false;
    bool outputTileExplicit = false, blendExplicit = false, jitterExplicit = false;
};

class ProjectService
{
  public:
    nlohmann::json info(const std::string &input) const;
    ServiceResult check(const ProjectRequest &request, RuntimeContext &runtime) const;
    ServiceResult pack(const ProjectRequest &request, RuntimeContext &runtime) const;
    ServiceResult render(const ProjectRequest &request, RuntimeContext &runtime) const;
};

bool writeHtmlProject(const MosaicEngine::Config &config, const std::string &directory, int columns, int rows,
                      const std::vector<ImageRecord> &library, const std::vector<int> &selection, const cv::Mat &target,
                      OutputStats &stats, RuntimeContext &runtime);
std::string sha256File(const std::filesystem::path &path);
void finalizeHtmlProject(const std::string &directory, RuntimeContext &runtime);
} // namespace mosaicraft
