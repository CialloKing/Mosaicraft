#pragma once

#include "core/MosaicService.h"

struct EditorCandidate
{
    int imageId;
    double score;
};

mosaicraft::ServiceResult editorCandidates(const mosaicraft::MosaicRequest &request,
                                         const mosaicraft::MosaicSelection &selection,
                                         const mosaicraft::PreviewWorkspace &workspace, size_t position,
                                         std::vector<EditorCandidate> &result);
