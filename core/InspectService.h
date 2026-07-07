#pragma once

#include "MosaicService.h"

#include <string>

namespace mosaicraft
{

struct InspectRequest
{
    std::string imagePath;
    std::string dbPath = "library/mosaicraft.db";
};

struct InspectResult
{
    ServiceResult status;
    std::string imagePath;
    int width = 0;
    int height = 0;
    double avgL = 0.0;
    double avgA = 0.0;
    double avgB = 0.0;
    double edgeDensity = 0.0;
    double lbpEntropy = 0.0;
    bool databaseAvailable = false;
    int databaseTotal = 0;
    double candidateMinL = 0.0;
    double candidateMaxL = 0.0;
    int candidateCount = 0;
    double libraryMinL = 0.0;
    double libraryMaxL = 0.0;
    double libraryAvgL = 0.0;
    int libraryDark = 0;
    int libraryMid = 0;
    int libraryBright = 0;
};

class InspectService
{
public:
    InspectResult inspect(const InspectRequest& request) const;
};

} // namespace mosaicraft
