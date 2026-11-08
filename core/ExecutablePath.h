#pragma once

#include <filesystem>

namespace mosaicraft
{
// Resolve the running executable independently of the caller's working directory.
std::filesystem::path executableDirectory();
}
