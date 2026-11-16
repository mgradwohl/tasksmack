#pragma once

// One of the Process Details Overview's section cards (#1537): Identity, Runtime, Actions, Priority and
// I/O priority are each a card (UI/Card.h, the CPU Cores grid's cell style) with the section's header
// inside it, at the top, as a core's name heads its cell. ProcessDetailsPanel draws Identity and Runtime
// with it and ProcessActionsBlock draws Actions, Priority and I/O priority (#1789, #3741), so all are
// drawn alike. Identity and Runtime also carry a Copy button in their header (#4034).

#include "SectionCopyButton.h"
#include "UI/Card.h"
#include "UI/ChromeWidgets.h"

#include <imgui.h>

#include <concepts>
#include <string>
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

/// render() with a Copy button at the right of the header ("Copy this card as plain text", #4034):
/// @p onCopy runs on the frame it is pressed, before @p body.
template<typename Body, typename OnCopy>
    requires std::invocable<const Body&> && std::invocable<const OnCopy&>
inline bool renderWithCopy(const char* id,
                           const char* icon,
                           std::string_view title,
                           const ImVec2& size,
                           ImGuiChildFlags extraChildFlags,
                           const Body& body,
                           const OnCopy& onCopy)
{
    const bool visible = UI::Widgets::beginCard(id, size, extraChildFlags);
    if (visible)
    {
        (void) UI::Widgets::sectionHeader(icon, title);
        if (SectionCopyButton::render("Copy this card as plain text"))
        {
            onCopy();
        }
        body();
    }
    UI::Widgets::endCard();
    return visible;
}

/// A card's text as Copy puts it on the clipboard (#4034): @p title, then one "label: value" line per
/// row of @p rows (anything with string_view-like label and value members), as System Information's
/// section Copy writes a section.
template<typename Rows> [[nodiscard]] std::string cardText(std::string_view title, const Rows& rows)
{
    std::string text(title);
    text += '\n';
    for (const auto& row : rows)
    {
        text.append(row.label);
        text += ": ";
        text.append(row.value);
        text += '\n';
    }
    return text;
}

} // namespace App::ProcessOverviewCard
