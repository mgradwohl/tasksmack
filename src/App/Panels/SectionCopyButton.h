#pragma once

// The Copy button a section header carries at the right end of its line: System Information's
// sections (#1395) and Process Details' Identity and Runtime cards (#4034) draw the same one, so they
// look and sit alike. Pressing it is the caller's to act on: build the text then, not every frame.

#include "UI/IconsFontAwesome6.h"

#include <imgui.h>

#include <algorithm>

namespace App::SectionCopyButton
{

/// The button's label, also its ImGui ID within the window it is drawn in.
inline constexpr const char* LABEL = ICON_FA_COPY "  Copy";

/// Draws the button on the line just drawn (a section header), right-aligned in the content region,
/// with @p tooltip on hover. @return Whether it was pressed this frame.
[[nodiscard]] inline bool render(const char* tooltip)
{
    const float width = ImGui::CalcTextSize(LABEL).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    ImGui::SameLine(std::max(ImGui::GetContentRegionMax().x - width, 0.0F));
    const bool pressed = ImGui::SmallButton(LABEL);
    ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

} // namespace App::SectionCopyButton
