#pragma once

#include <filesystem>
#include <iosfwd>

namespace mosaicraft
{
int runEditorSession(std::istream &input, std::ostream &output, const std::filesystem::path &cancelFile = {});
} // namespace mosaicraft
