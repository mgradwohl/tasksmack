#pragma once

// The one state-to-colour mapping the Processes table's State column and Process Details share, so
// the two views colour a process the same way (#1352, #1180). Keyed on Domain::processStateCode(),
// the same code the State column prints. Pure: it reads a ColorScheme and makes no ImGui calls.

#include "Domain/ProcessState.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <string_view>

namespace App
{

/// The theme colour for a kernel-style state code (see Domain::processStateCode()). Dead ('X') and
/// unknown states use the muted sleeping colour: neither is a state worth drawing the eye to.
[[nodiscard]] inline ImVec4 processStateColor(char stateCode, const UI::ColorScheme& scheme) noexcept
{
    switch (stateCode)
    {
    case 'R':
        return scheme.statusRunning;
    case 'D':
        return scheme.statusDiskSleep;
    case 'Z':
        return scheme.statusZombie;
    case 'T':
    case 't':
        return scheme.statusStopped;
    case 'I':
        return scheme.statusIdle;
    case 'S':
    default:
        return scheme.statusSleeping;
    }
}

/// The theme colour for a ProcessSnapshot::displayState name.
[[nodiscard]] inline ImVec4 processStateColor(std::string_view displayState, const UI::ColorScheme& scheme) noexcept
{
    return processStateColor(Domain::processStateCode(displayState), scheme);
}

} // namespace App
