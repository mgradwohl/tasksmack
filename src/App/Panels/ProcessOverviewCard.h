#pragma once

// One of the Process Details Overview's section cards (#1537): Identity, Runtime and Actions are each
// a card (UI/Card.h, the CPU Cores grid's cell style) with the section's header inside it, at the top,
// as a core's name heads its cell. ProcessDetailsPanel draws Identity and Runtime with it and
// ProcessActionsBlock draws Actions, so the three are drawn alike.

#include "UI/Card.h"
#include "UI/ChromeWidgets.h"

#include <imgui.h>

#include <concepts>
#include <string_view>

namespace App::ProcessOverviewCard
{

/// Draws a card @p size big (ImGui::BeginChild()'s size rules; @p extraChildFlags as for
/// UI::Widgets::beginCard()) holding the section header (@p icon, @p title) and then @p body's rows.
/// Neither runs while the card is scrolled out of view. Returns whether it was drawn.
template<typename Body>
    requires std::invocable<const Body&>
inline bool
render(const char* id, const char* icon, std::string_view title, const ImVec2& size, ImGuiChildFlags extraChildFlags, const Body& body)
{
    const bool visible = UI::Widgets::beginCard(id, size, extraChildFlags);
    if (visible)
    {
        (void) UI::Widgets::sectionHeader(icon, title);
        body();
    }
    UI::Widgets::endCard();
    return visible;
}

} // namespace App::ProcessOverviewCard
