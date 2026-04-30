#pragma once

#include <string_view>

namespace App::PlatformOpen
{

/// Open a URL or file path with the system's default handler.
/// On Linux, uses xdg-open via a double-fork to avoid zombie processes.
/// On Windows, uses ShellExecuteW.
/// Returns true on success, false on failure (with a logged warning).
[[nodiscard]] bool openWithSystemHandler(std::string_view target);

} // namespace App::PlatformOpen
