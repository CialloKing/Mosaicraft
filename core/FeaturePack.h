#pragma once

#include "Database.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mosaicraft
{
// Cache is an acceleration layer; missing source features retain their validity flags.
class FeaturePack
{
  public:
    struct Stats
    {
        bool reused = false;
        double validationMs = 0;
        double writingMs = 0;
    };
    static bool load(const std::string &directory, const std::vector<ImageRecord> &records,
                     const std::vector<int> &needed, std::vector<uint8_t> &tiny, std::vector<float> &lbp,
                     std::vector<uint8_t> &tinyValid, std::vector<uint8_t> &lbpValid);
    static bool buildCache(const std::string &directory, const std::vector<ImageRecord> &records,
                           Stats *stats = nullptr);
};
} // namespace mosaicraft
