#pragma once

#include <string>
#include <vector>

namespace mosaicraft
{

struct ApiEndpointMetadata
{
    std::string method;
    std::string path;
    std::string description;
    std::string category;
    std::vector<std::string> requestFields;
    bool legacy = false;
    bool enabled = true;
};

std::vector<ApiEndpointMetadata> apiEndpointMetadata(bool legacyRunEnabled);
std::vector<std::string> apiFeatureList();

} // namespace mosaicraft
