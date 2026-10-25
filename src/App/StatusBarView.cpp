#include "StatusBarView.h"

#include "App/ShellMetrics.h"
#include "App/StatusBarText.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace App::StatusBarView
{

Action render(const Input& input)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Calculate height dynamically based on font size for proper scaling
    const float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    // When native OS decorations replace the custom title bar (see #745), TitleBarLayer is
    // skipped and this status bar becomes the only place Settings/Help can be reached -- keep
    // it keyboard-navigable in that mode so those buttons stay reachable without a mouse.
    const bool showStatusBarControls = input.showControls;
    Action action = Action::None;
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!showStatusBarControls)
    {
        windowFlags |= ImGuiWindowFlags_NoNav;
    }

    // Use theme colors for status bar
    const auto& theme = UI::Theme::get();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.scheme().statusBarBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F); // Show top border
    // Center text vertically within the status bar
    const float verticalPadding = (statusBarHeight - ImGui::GetFontSize()) * 0.5F;
    const float statusBarPaddingX = ShellMetrics::STATUS_BAR_PADDING_X * UI::Theme::get().styleScale();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(statusBarPaddingX, verticalPadding));

    if (ImGui::Begin("##StatusBar", nullptr, windowFlags))
    {
        // Show a persistent lock icon when running without elevated privileges
        // Live, not a startup copy: a probe can withdraw a capability after the first sample (#1254).
        // ProcessesPanel keeps it with its cached snapshot generation, so reading it takes no lock.
        if (input.access.has_value())
        {
            // Standard or Administrator (#2728): what the session is, and Standard's way to Administrator.
            const bool standard = *input.access == AccessChip::Standard;
            ImGui::PushStyleColor(ImGuiCol_Text, standard ? theme.scheme().textWarning : theme.scheme().textPrimary);
            const bool pressed = ImGui::SmallButton(standard ? STANDARD_CHIP_LABEL : ADMINISTRATOR_CHIP_LABEL);
            ImGui::PopStyleColor();
            if (pressed && standard)
            {
                action = Action::Access;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s",
                                  standard ? "Standard: some details of other users' processes, or actions on them, are unavailable. "
                                             "Click for Administrator"
                                           : "Administrator: nothing is limited for this session");
            }
            ImGui::SameLine();
        }
        else if (input.reducedPrivileges)
        {
            ImGui::TextColored(theme.scheme().textWarning, ICON_FA_LOCK);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Limited: some details of other users' processes, or actions on them, are unavailable");
            }
            ImGui::SameLine();
        }
        // Per-process network counters denied although TaskSmack already has the rights they need
        // (#1358): the network columns are gone, and running as Administrator would not bring them
        // back, so this is not the lock icon's "limited data" notice.
        if (input.networkCountersBlocked)
        {
            ImGui::TextColored(theme.scheme().textWarning, ICON_FA_NETWORK_WIRED);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Per-process network counters are blocked on this system "
                                  "(TCP EStats access denied even when elevated)");
            }
            ImGui::SameLine();
        }

        // Live status (#1200): how many processes TaskSmack is tracking and how often it updates, in
        // that order of importance, then -- only while Render Metrics (Ctrl+Shift+M) is on -- the
        // frame rate at the right. Everything is measured before anything is drawn, so at the
        // minimum window size the lower-priority parts are left out (or the process count cut
        // short) rather than drawn over the buttons or each other (#1207).
        const ImGuiStyle& style = ImGui::GetStyle();
        const float textStartX = ImGui::GetCursorPosX();
        const float contentRightX = ImGui::GetWindowWidth() - statusBarPaddingX;

        // When native OS decorations replace the custom title bar (see #745), its Settings/Help
        // buttons don't exist -- equivalents follow the text here so those stay reachable.
        const auto smallButtonWidth = [&style](const char* label)
        {
            return ImGui::CalcTextSize(label, nullptr, true).x + (style.FramePadding.x * 2.0F);
        };
        const float buttonsWidth =
            showStatusBarControls ? (style.ItemSpacing.x * 2.0F) + smallButtonWidth(SETTINGS_LABEL) + smallButtonWidth(HELP_LABEL) : 0.0F;

        // The readout is formatted first and then measured, so it ends at the status bar's padding
        // at any font and any value (#971).
        std::array<char, 48> fpsText{};
        std::size_t fpsLength = 0;
        if (input.fps.has_value())
        {
            // "60.0 FPS (16.67 ms)" in the display locale's punctuation, without allocating.
            std::array<char, 24> fpsValue{};
            std::array<char, 24> msValue{};
            const std::size_t fpsValueLength =
                UI::Format::formatFixedLocalizedTo(fpsValue.data(), fpsValue.size(), static_cast<double>(*input.fps), 1);
            const std::size_t msValueLength = UI::Format::formatFixedLocalizedTo(
                msValue.data(), msValue.size(), static_cast<double>(input.frameTimeSeconds * 1000.0F), 2);
            if (fpsValueLength > 0 && msValueLength > 0)
            {
                const std::size_t capacity = fpsText.size() - 1;
                UI::Format::appendText(fpsText.data(), capacity, fpsLength, {fpsValue.data(), fpsValueLength});
                UI::Format::appendText(fpsText.data(), capacity, fpsLength, " FPS (");
                UI::Format::appendText(fpsText.data(), capacity, fpsLength, {msValue.data(), msValueLength});
                UI::Format::appendText(fpsText.data(), capacity, fpsLength, " ms)");
                fpsLength = (fpsLength <= capacity) ? fpsLength : 0;
            }
        }
        const float fpsWidth = (fpsLength > 0) ? ImGui::CalcTextSize(fpsText.data(), fpsText.data() + fpsLength).x : 0.0F;

        // Most important first; an empty segment (no interval yet) is left out.
        std::array<std::string_view, 2> segments{};
        std::array<float, 2> segmentWidths{};
        std::size_t segmentCount = 0;
        for (const std::string_view text : {input.processText, input.intervalText})
        {
            if (!text.empty())
            {
                segments.at(segmentCount) = text;
                segmentWidths.at(segmentCount) = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
                ++segmentCount;
            }
        }
        const float textBudget = contentRightX - textStartX - buttonsWidth;
        const auto fit = StatusBarText::fitStatusBar(std::span<const float>(segmentWidths.data(), segmentCount),
                                                     ImGui::CalcTextSize(StatusBarText::SEPARATOR).x,
                                                     fpsWidth,
                                                     style.ItemSpacing.x,
                                                     textBudget);

        bool drewText = false;
        if (fit.truncateFirst)
        {
            // Only at the very narrowest, so the cut copy is made only then. Empty when not even the
            // ellipsis fits: then nothing is drawn rather than an ellipsis past the budget.
            const std::string cut = StatusBarText::ellipsize(segments[0],
                                                             textBudget,
                                                             [](std::string_view text)
                                                             { return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x; });
            if (!cut.empty())
            {
                ImGui::TextUnformatted(cut.data(), cut.data() + cut.size());
                drewText = true;
            }
        }
        for (std::size_t i = 0; i < fit.segmentsShown; ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0F, 0.0F);
                ImGui::TextColored(theme.scheme().textMuted, "%s", StatusBarText::SEPARATOR);
                ImGui::SameLine(0.0F, 0.0F);
            }
            const std::string_view segment = segments.at(i);
            ImGui::TextUnformatted(segment.data(), segment.data() + segment.size());
            drewText = true;
        }

        if (showStatusBarControls)
        {
            if (drewText)
            {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton(SETTINGS_LABEL))
            {
                action = Action::Settings;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Settings (F2)");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(HELP_LABEL))
            {
                action = Action::Help;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Help: keyboard shortcuts, columns and tabs (F1)");
            }
        }

        if (fit.showReadout)
        {
            ImGui::SameLine(contentRightX - fpsWidth);
            ImGui::TextUnformatted(fpsText.data(), fpsText.data() + fpsLength);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    return action;
}

} // namespace App::StatusBarView
