#pragma once

#include <string_view>

namespace mosaicraft::managed_library
{
inline constexpr char managedDirectory[] = ".mosaicraft-desktop";
inline constexpr char activeLibraryFile[] = "active-library";
inline constexpr char databaseFile[] = "mosaicraft.db";
inline constexpr std::string_view generationPrefix = "library-";

inline bool isLibraryGeneration(std::string_view name)
{
    const auto prefix = generationPrefix.size();
    return name.size() > prefix && name.size() <= prefix + 20 && name.substr(0, prefix) == generationPrefix &&
           name.find_first_not_of("0123456789", prefix) == std::string_view::npos;
}
} // namespace mosaicraft::managed_library
