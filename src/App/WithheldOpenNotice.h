#pragma once

// The line a view shows when an elevated TaskSmack didn't open a link, file or folder (#3658): Windows'
// shell couldn't open it as the signed-in user, and TaskSmack never opens it as administrator instead
// (PlatformOpen::open(), BackgroundOpen::withheld()). It names the target, wrapped, in the warning colour,
// with a Copy button that puts the target on the clipboard, so the user can open it themselves.

#include "App/PlatformOpen.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <string>
#include <string_view>

namespace App::WithheldOpenNotice
{

/// The Copy button's label, with an ID of its own.
inline constexpr const char* COPY_LABEL = ICON_FA_COPY " Copy##WithheldOpen";

/// The Copy button's label for draw(target, notice), with an ID apart from COPY_LABEL's, as both may show at once.
inline constexpr const char* COPY_FOLDER_LABEL = ICON_FA_COPY " Copy##AdminOnlyFolder";

/// Draws @p notice, then @p target with a Copy button, or nothing when @p target is empty (#4188: a folder only an
/// elevated Explorer can open). Wraps at the caller's wrap position, or at the window's edge when none is pushed.
inline void draw(std::string_view target, std::string_view notice)
{
    if (target.empty())
    {
        return;
    }
    const std::string text = std::string(notice) + "\n" + std::string(target);
    ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textWarning);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton(COPY_FOLDER_LABEL))
    {
        const std::string copied(target);
        ImGui::SetClipboardText(copied.c_str());
    }
    ImGui::SetItemTooltip("Copy it, to open it yourself");
}

/// Draws the notice for @p target, or nothing when it is empty. Wraps at the caller's wrap position, or at
/// the window's edge when none is pushed.
inline void draw(std::string_view target)
{
    if (target.empty())
    {
        return;
    }
    const std::string notice = PlatformOpen::withheldNotice(target);
    ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textWarning);
    ImGui::TextWrapped("%s", notice.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton(COPY_LABEL))
    {
        const std::string text(target);
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SetItemTooltip("Copy it, to open it yourself");
}

} // namespace App::WithheldOpenNotice
