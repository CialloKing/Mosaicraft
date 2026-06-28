#include "LegacyRun.h"

namespace mosaicraft
{

LegacyCommandValidation validateLegacyRunCommand(const std::string& command)
{
    LegacyCommandValidation result;
    if (command.empty()) {
        result.error = "ERROR: empty command";
        return result;
    }

    const std::string forbidden = "&|;$`(){}<>";
    bool hasControlChar = false;
    for (unsigned char ch : command) {
        if (ch < 0x20 || ch == 0x7f) {
            hasControlChar = true;
            break;
        }
    }
    if (command.find_first_of(forbidden) != std::string::npos || hasControlChar) {
        result.error = "ERROR: invalid characters in command";
        return result;
    }

    const std::string prefix = "mosaicraft ";
    if (command.compare(0, prefix.size(), prefix) != 0) {
        result.error = "ERROR: command must start with 'mosaicraft'";
        return result;
    }

    result.subCommand = command.substr(prefix.size());
    auto spacePos = result.subCommand.find(' ');
    result.commandName = (spacePos != std::string::npos)
        ? result.subCommand.substr(0, spacePos)
        : result.subCommand;

    const std::string validCommands[] = {
        "build",
        "mosaic",
        "inspect",
        "db-stats",
        "db-purge",
        "db-usage",
        "db-health"
    };
    for (const auto& valid : validCommands) {
        if (result.commandName == valid) {
            result.ok = true;
            return result;
        }
    }

    result.error = "ERROR: unknown command: " + result.commandName;
    return result;
}

} // namespace mosaicraft
