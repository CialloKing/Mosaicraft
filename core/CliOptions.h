#pragma once
#include "CliOptionsData.h"
#include "json.hpp"
#include <string>
#include <vector>

namespace mosaicraft
{
struct CliOption
{
    const char *name, *shortName, *type, *field, *commands, *help;
    bool advanced = false;
};
inline const std::vector<CliOption> &cliOptions()
{
    static const auto definitions = nlohmann::json::parse(mosaicCliOptionsJson);
    static const auto options = []()
    {
        std::vector<CliOption> result;
        for (const auto &definition : definitions)
        {
            const auto text = [&](const char *key)
            {
                return definition.at(key).get_ref<const std::string &>().c_str();
            };
            result.push_back({text("name"), text("short"), text("type"), text("field"), text("commands"), text("help"),
                              definition.at("advanced")});
        }
        return result;
    }();
    return options;
}
inline bool cliOptionSupports(const CliOption &option, const std::string &command)
{
    return std::string(option.commands) == "*" ||
           (" " + std::string(option.commands) + " ").find(" " + command + " ") != std::string::npos;
}
inline nlohmann::json cliOptionsToJson()
{
    auto result = nlohmann::json::array();
    for (const auto &option : cliOptions())
    {
        result.push_back({{"name", option.name},
                          {"short", option.shortName},
                          {"type", option.type},
                          {"field", option.field},
                          {"commands", option.commands},
                          {"help", option.help},
                          {"advanced", option.advanced}});
    }
    return result;
}
} // namespace mosaicraft
