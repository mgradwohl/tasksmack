#include "TaskManagerReplacementRows.h"

#include "App/TaskManagerReplacementController.h"
#include "App/TaskManagerReplacementText.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/TaskManagerReplacement.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <string>
#include <string_view>

namespace App::TaskManagerReplacementRows
{

namespace
{

using Platform::TaskManagerReplacement::State;

/// @p text wrapped at @p wrapWidth from the current position, in @p color.
void wrapped(std::string_view text, const ImVec4& color, float wrapWidth)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

/// The last change's result, in the success or error colour.
void lastChange(const TaskManagerReplacementView& view, float wrapWidth)
{
    if (!view.lastChange.has_value())
    {
        return;
    }
    const auto& scheme = UI::Theme::get().scheme();
    const bool ok = view.lastChange->success;
    const std::string& text = ok ? view.lastChange->successNote : view.lastChange->errorMessage;
    if (!text.empty())
    {
        wrapped(text, ok ? scheme.textSuccess : scheme.textError, wrapWidth);
    }
}

} // namespace

TaskManagerRequest renderSettings(const TaskManagerReplacementView& view, RowState& state, float wrapWidth)
{
    if (!view.supported)
    {
        return TaskManagerRequest::None;
    }
    const auto& scheme = UI::Theme::get().scheme();
    TaskManagerRequest request = TaskManagerRequest::None;
    ImGui::PushID("TaskManagerReplacement");

    // What the last read found, when it found anything.
    const Platform::TaskManagerReplacementInfo* const info = (view.info.has_value() && !view.info->readFailed) ? &*view.info : nullptr;
    const State current = (info != nullptr) ? info->status.state : State::Off;
    const bool canChange = info != nullptr && info->unavailableReason.empty();

    // Checked from the registry, never from the click: the box shows what Task Manager does now, and a
    // click asks for the change, which the next read then shows.
    bool checked = TaskManagerReplacementText::toggleChecked(current);
    ImGui::BeginDisabled(!canChange || view.busy || state.confirmingReplaceOther);
    if (UI::Widgets::outlinedCheckbox(TaskManagerReplacementText::TOGGLE_LABEL, &checked))
    {
        const ToggleOutcome outcome = toggleOutcome(current);
        request = outcome.request;
        state.confirmingReplaceOther = outcome.confirm;
    }
    ImGui::EndDisabled();
    wrapped(TaskManagerReplacementText::NOTE, scheme.textMuted, wrapWidth);
    wrapped(TaskManagerReplacementText::stateText(view), scheme.textPrimary, wrapWidth);

    if (current == State::OtherTaskSmack)
    {
        ImGui::BeginDisabled(!canChange || view.busy);
        if (ImGui::Button(TaskManagerReplacementText::REPAIR_LABEL))
        {
            request = TaskManagerRequest::Repair;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Point Task Manager at this copy of TaskSmack");
    }
    if (info != nullptr && !info->installedCopy && TaskManagerReplacementText::toggleChecked(current))
    {
        wrapped(TaskManagerReplacementText::PORTABLE_WARNING, scheme.textWarning, wrapWidth);
    }
    if (info != nullptr && !info->unavailableReason.empty())
    {
        wrapped(info->unavailableReason, scheme.textMuted, wrapWidth);
    }

    if (state.confirmingReplaceOther && info != nullptr && current == State::OtherProgram)
    {
        wrapped(TaskManagerReplacementText::confirmText(info->status.program), scheme.textWarning, wrapWidth);
        if (ImGui::Button(TaskManagerReplacementText::REPLACE_LABEL))
        {
            request = TaskManagerRequest::TurnOnReplacingOther;
            state.confirmingReplaceOther = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(TaskManagerReplacementText::CANCEL_LABEL))
        {
            state.confirmingReplaceOther = false;
        }
    }
    else
    {
        state.confirmingReplaceOther = false; // the state changed under it: nothing left to confirm
    }

    if (view.busy)
    {
        wrapped(TaskManagerReplacementText::BUSY, scheme.textMuted, wrapWidth);
    }
    else
    {
        lastChange(view, wrapWidth);
    }
    ImGui::PopID();
    return request;
}

TaskManagerRequest renderNotice(const TaskManagerReplacementView& view, NoticeState& state)
{
    if (!state.open || !view.supported || !view.info.has_value())
    {
        return TaskManagerRequest::None;
    }
    TaskManagerRequest request = TaskManagerRequest::None;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = ImGui::GetFontSize();
    // The bottom right of the window, out of the way of the tables: it blocks nothing.
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin, viewport->WorkPos.y + viewport->WorkSize.y - margin),
        ImGuiCond_Always,
        ImVec2(1.0F, 1.0F));
    const float wrapWidth = ImGui::GetFontSize() * 26.0F;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    const std::string title =
        std::string(ICON_FA_TRIANGLE_EXCLAMATION "  ") + TaskManagerReplacementText::NOTICE_TITLE + "###TaskManagerNotice";
    if (ImGui::Begin(title.c_str(), &state.open, flags))
    {
        UI::Widgets::keepCurrentWindowInViewport();
        const auto& scheme = UI::Theme::get().scheme();
        if (!state.acted)
        {
            wrapped(TaskManagerReplacementText::noticeText(*view.info), scheme.textPrimary, wrapWidth);
            if (!view.info->unavailableReason.empty())
            {
                wrapped(view.info->unavailableReason, scheme.textMuted, wrapWidth);
            }
            ImGui::Spacing();
            const bool canChange = view.info->unavailableReason.empty() && !view.busy;
            ImGui::BeginDisabled(!canChange);
            if (ImGui::Button(TaskManagerReplacementText::REPAIR_LABEL))
            {
                request = TaskManagerRequest::Repair;
                state.acted = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(TaskManagerReplacementText::TURN_OFF_LABEL))
            {
                request = TaskManagerRequest::TurnOff;
                state.acted = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(TaskManagerReplacementText::NOT_NOW_LABEL))
            {
                state.open = false;
            }
        }
        else
        {
            if (view.busy)
            {
                wrapped(TaskManagerReplacementText::BUSY, scheme.textMuted, wrapWidth);
            }
            else
            {
                lastChange(view, wrapWidth);
            }
            ImGui::BeginDisabled(view.busy);
            if (ImGui::Button(TaskManagerReplacementText::CLOSE_LABEL))
            {
                state.open = false;
            }
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
    return request;
}

} // namespace App::TaskManagerReplacementRows
