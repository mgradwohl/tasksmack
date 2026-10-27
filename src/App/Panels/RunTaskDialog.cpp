#include "RunTaskDialog.h"

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Core/Application.h"
#include "Core/SaveFileDialog.h"
#include "Core/Window.h"
#include "Platform/CommandWords.h"
#include "Platform/IProcessActions.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <SDL3/SDL_video.h>
#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{

namespace
{

constexpr const char* POPUP_ID = "###RunNewTask";
constexpr const char* RUN_LABEL = "Run";
constexpr const char* CANCEL_LABEL = "Cancel";
constexpr const char* RECENT_POPUP_ID = "##RunTaskRecent";
constexpr float BUTTON_MIN_EM = 7.5F;
constexpr float DIALOG_WIDTH_EM = 34.0F;

/// The app's file dialog over its window, starting in /usr/bin, where most programs are; with no window
/// (headless), nothing is chosen.
void sdlBrowse(std::function<void(std::string)> chosen)
{
    SDL_Window* window = nullptr;
    try
    {
        const auto& app = Core::Application::get();
        if (app.hasWindow())
        {
            window = app.getWindow().getHandle();
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch): no Application (headless), so no window to show the dialog over
    {}
    if (window == nullptr)
    {
        chosen({});
        return;
    }
#ifdef _WIN32
    const std::string start;
#else
    const std::string start = "/usr/bin/";
#endif
    Core::showOpenFileDialog(window, start, [chosen = std::move(chosen)](const Core::SaveFileResult& result) { chosen(result.path); });
}

} // namespace

std::string runTaskShortName(std::string_view commandLine)
{
    constexpr std::size_t MAX_BYTES = 60;
    constexpr std::size_t KEPT_BYTES = 57;
    if (commandLine.size() <= MAX_BYTES)
    {
        return std::string(commandLine);
    }
    std::size_t end = KEPT_BYTES;
    while (end > 0 && (static_cast<unsigned char>(commandLine[end]) & 0xC0U) == 0x80U) // a continuation byte
    {
        --end;
    }
    return std::string(commandLine.substr(0, end)) + "...";
}

Detail::ActionResultMessage runTaskResultMessage(std::string_view shown, bool asAdministrator, const Platform::ProcessActionResult& result)
{
    if (!result.success)
    {
        return {.ok = false, .text = std::format("Could not start {}: {}", shown, result.errorMessage)};
    }
    return {.ok = true, .text = std::format("Started {}{}", shown, asAdministrator ? " as administrator" : "")};
}

RunTaskDialog::RunTaskDialog() : RunTaskDialog(&sdlBrowse)
{}

RunTaskDialog::RunTaskDialog(Browse browse) : m_Browse(std::move(browse))
{}

void RunTaskDialog::open(std::vector<std::string> recent, bool canRunAsAdministrator)
{
    m_Recent = std::move(recent);
    m_CanRunAsAdministrator = canRunAsAdministrator;
    if (!m_CanRunAsAdministrator)
    {
        m_AsAdministrator = false;
    }
    if (m_Command.empty() && !m_Recent.empty())
    {
        m_Command = m_Recent.front(); // the last one run, ready to run again
    }
    m_ShowRequested = true;
    m_FocusCommand = true;
}

void RunTaskDialog::takeBrowseResult()
{
    const std::scoped_lock lock(m_BrowseSlot->mutex);
    if (m_BrowseSlot->path.has_value())
    {
        if (!m_BrowseSlot->path->empty())
        {
            m_Command = Platform::CommandWords::quote(*m_BrowseSlot->path); // a chosen file is one word
            m_FocusCommand = true;
        }
        m_BrowseSlot->path.reset();
    }
}

bool RunTaskDialog::renderCommandRow()
{
    const float browseWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN " Browse...").x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    const float recentWidth = m_Recent.empty() ? 0.0F : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.0F,
                                     ImGui::GetContentRegionAvail().x - browseWidth - recentWidth - ImGui::GetStyle().ItemSpacing.x));
    if (std::exchange(m_FocusCommand, false))
    {
        ImGui::SetKeyboardFocusHere();
    }
    const bool entered =
        ImGui::InputTextWithHint("##command", "konsole, or /usr/bin/foot --title Rescue", &m_Command, ImGuiInputTextFlags_EnterReturnsTrue);
    if (!m_Recent.empty())
    {
        ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::ArrowButton("##recentButton", ImGuiDir_Down))
        {
            ImGui::OpenPopup(RECENT_POPUP_ID);
        }
        ImGui::SetItemTooltip("Recent commands");
        if (ImGui::BeginPopup(RECENT_POPUP_ID))
        {
            for (const std::string& command : m_Recent)
            {
                if (ImGui::Selectable(command.c_str(), command == m_Command))
                {
                    m_Command = command;
                    m_FocusCommand = true;
                }
            }
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    bool browsing = false;
    {
        const std::scoped_lock lock(m_BrowseSlot->mutex);
        browsing = m_BrowseSlot->pending;
    }
    ImGui::BeginDisabled(browsing);
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse...") && m_Browse)
    {
        {
            const std::scoped_lock lock(m_BrowseSlot->mutex);
            m_BrowseSlot->pending = true;
        }
        // The slot outlives the dialog if the file dialog answers after it has gone.
        m_Browse(
            [slot = m_BrowseSlot](std::string path)
            {
                const std::scoped_lock lock(slot->mutex);
                slot->path = std::move(path);
                slot->pending = false;
            });
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Choose a program file");
    return entered;
}

std::optional<Platform::RunProgramRequest> RunTaskDialog::render()
{
    if (m_ShowRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        m_ShowRequested = false;
    }
    m_Open = ImGui::IsPopupOpen(POPUP_ID);
    if (!m_Open)
    {
        return std::nullopt;
    }
    takeBrowseResult();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const std::string title = std::string(ICON_FA_ROCKET "  Run new task") + POPUP_ID;
    UI::Widgets::centerNextDialog(m_LastWorkSize);
    const float width = std::min((DIALOG_WIDTH_EM * ImGui::GetFontSize()) + (ImGui::GetStyle().WindowPadding.x * 2.0F),
                                 UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0F));
    std::optional<Platform::RunProgramRequest> run;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return run;
    }
    UI::Widgets::keepCurrentWindowInViewport();
    const bool escaped = UI::Widgets::dialogEscapePressed();
    const auto& theme = UI::Theme::get();

    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted("The program to start, and its arguments. It runs on its own, apart from TaskSmack.");
    ImGui::TextColored(theme.scheme().textMuted,
                       "No shell runs it, so $VARIABLES, ~ and wildcards stay as typed. Quote a word with spaces in it.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const bool entered = renderCommandRow();

    if (m_CanRunAsAdministrator)
    {
        UI::Widgets::outlinedCheckbox("Run as administrator", &m_AsAdministrator);
        ImGui::SetItemTooltip("Asks for an administrator's password first (Linux: through pkexec). A program with a window may not "
                              "open this way, because pkexec doesn't pass on your display.");
    }

    const bool typed = m_Command.find_first_not_of(" \t\n") != std::string::npos;
    const UI::Widgets::ButtonFills runFills{
        .resting = theme.scheme().successButton,
        .hovered = theme.scheme().successButtonHovered,
        .pressed = theme.scheme().successButtonActive,
    };
    const UI::Widgets::DialogFooterButton runButton{
        .label = RUN_LABEL, .fills = &runFills, .tooltip = typed ? nullptr : "Type the program to run", .disabled = !typed};
    const UI::Widgets::DialogFooterButton cancelButton{.label = CANCEL_LABEL, .fills = nullptr, .tooltip = nullptr};
    UI::Widgets::DialogFooterAction pressed =
        UI::Widgets::dialogFooter(runButton, cancelButton, UI::Widgets::footerButtonWidth({RUN_LABEL, CANCEL_LABEL}, BUTTON_MIN_EM));
    if (entered && typed)
    {
        pressed = UI::Widgets::DialogFooterAction::Primary;
    }
    if (escaped)
    {
        pressed = UI::Widgets::DialogFooterAction::Secondary;
    }
    switch (pressed)
    {
    case UI::Widgets::DialogFooterAction::Primary:
        if (typed)
        {
            run = Platform::RunProgramRequest{.commandLine = m_Command, .asAdministrator = m_AsAdministrator};
            m_Open = false;
            ImGui::CloseCurrentPopup();
        }
        break;
    case UI::Widgets::DialogFooterAction::Secondary:
        m_Open = false;
        ImGui::CloseCurrentPopup();
        break;
    case UI::Widgets::DialogFooterAction::None:
    case UI::Widgets::DialogFooterAction::Leading:
        break;
    }
    ImGui::EndPopup();
    return run;
}

} // namespace App
