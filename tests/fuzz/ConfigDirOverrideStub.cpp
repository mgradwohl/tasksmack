// Stands in for src/Core/ConfigDirOverride.cpp in the fuzz_user_config build (#1596).
//
// UserConfig.cpp asks Core::ConfigDirOverride::active() for the TASKSMACK_CONFIG_DIR test hook. The
// real active() reads the variable through SDL_getenv(), and the fuzz build has no SDL (see
// .clusterfuzzlite/fetch-deps.sh). The fuzzer exercises parseSettings(), never the config
// directory, so "no override" -- TaskSmack's behaviour with the variable unset -- is all it needs. With no
// override, nothing was handed over by an elevated restart either (handedOver(), #2728), which UserConfig's save asks,
// so there is no held folder to read or write through, nor one let go (handedOverFolder(), #3774, #3787).

#include "Core/ConfigDirOverride.h"
#include "Platform/IHandedOverFolder.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace Core::ConfigDirOverride
{

const std::optional<std::filesystem::path>& active()
{
    static const std::optional<std::filesystem::path> none;
    return none;
}

bool handedOver()
{
    return false;
}

std::shared_ptr<const Platform::IHandedOverFolder> handedOverFolder()
{
    return nullptr;
}

bool handedOverFolderReleased()
{
    return false;
}

} // namespace Core::ConfigDirOverride
