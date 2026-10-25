// Stands in for src/Core/ConfigDirOverride.cpp in the fuzz_user_config build (#1596).
//
// UserConfig.cpp asks Core::ConfigDirOverride::active() for the TASKSMACK_CONFIG_DIR test hook. The
// real active() reads the variable through SDL_getenv(), and the fuzz build has no SDL (see
// .clusterfuzzlite/fetch-deps.sh). The fuzzer exercises parseSettings(), never the config
// directory, so "no override" -- TaskSmack's behaviour with the variable unset -- is all it needs.

#include "Core/ConfigDirOverride.h"

#include <filesystem>
#include <optional>

namespace Core::ConfigDirOverride
{

const std::optional<std::filesystem::path>& active()
{
    static const std::optional<std::filesystem::path> none;
    return none;
}

} // namespace Core::ConfigDirOverride
