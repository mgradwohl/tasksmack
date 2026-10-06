#pragma once

// The one process-type-to-colour mapping the Processes table's Type column and Process Details share,
// so the two views colour a process type the same way (#1180). Keyed on ProcessSnapshot::processType,
// the Windows-only "App" / "Background Process" / "Windows Process" classification (empty elsewhere).
// Pure: it reads a ColorScheme and makes no ImGui calls.

#include "UI/Theme.h"

#include <imgui.h>

#include <string_view>

namespace App
{

/// The theme colour for a ProcessSnapshot::processType name: an App in the running colour, a Windows
/// Process in the info colour, and anything else -- a Background Process, or a type this build does
/// not know -- muted. Callers show "-" for an empty type rather than colouring it.
[[nodiscard]] inline ImVec4 processTypeColor(std::string_view processType, const UI::ColorScheme& scheme) noexcept
{
    if (processType == "App")
    {
        return scheme.statusRunning;
    }
    if (processType == "Windows Process")
    {
        return scheme.textInfo;
    }
    return scheme.textMuted;
}

} // namespace App
