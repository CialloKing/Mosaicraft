#pragma once
#include "BuildService.h"
namespace mosaicraft
{
int normalizationWorkerCount(int requestedThreads);
ServiceResult normalizeImages(const BuildRequest &request, RuntimeContext &runtime);
}
