#pragma once

#include "json.hpp"

namespace mosaicraft
{
class RuntimeContext;
nlohmann::json maintainDesktopLibrary(const nlohmann::json &payload, RuntimeContext &runtime);
} // namespace mosaicraft
