#pragma once

#include <string>

namespace mosaicraft
{

struct LegacyCommandValidation
{
    bool ok = false;
    std::string subCommand;
    std::string commandName;
    std::string error;
};

LegacyCommandValidation validateLegacyRunCommand(const std::string& command);

} // namespace mosaicraft
