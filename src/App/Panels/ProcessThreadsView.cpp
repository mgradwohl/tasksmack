#include "ProcessThreadsView.h"

#include "AuthenticateToReadButton.h"
#include "AuthorizeWording.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"
#include "ProcessActionConfirm.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "ThreadActionControl.h"
#include "UI/CellText.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{

namespace
{

/// The row menu's popup ID (#3069), got outside the table so the rows open the popup drawn after it.
constexpr const char* ROW_MENU_ID = "##ThreadRowMenu";

} // namespace

void ProcessThreadsView::render(bool hasThreads)
{
    if (!hasThreads)
    {
        return; // synthetic runs, and Windows for now: no section at all
    }
    // The count, formatted only when it changes; the ### keeps the header's ID (and its open state).
    const bool listed = m_HasRead && m_Status == Platform::ThreadsReadStatus::Ok;
    if (listed && m_LabelCount != m_Rows.size())
    {
        m_LabelCount = m_Rows.size();
        m_CountLabel = std::format(ICON_FA_LIST "  Threads ({})###ProcessThreads", m_LabelCount);
    }
    // Collapsed by default: nothing is read until it is opened.
    if (!ImGui::CollapsingHeader(listed ? m_CountLabel.c_str() : ICON_FA_LIST "  Threads###ProcessThreads"))
    {
        return;
    }
    markDrawnOpen();
    renderActionResult();

    const auto& scheme = UI::Theme::get().scheme();
    if (!m_HasRead)
    {
        ImGui::TextColored(scheme.textMuted, "Reading...");
        return;
    }
    if (m_Status != Platform::ThreadsReadStatus::Ok)
    {
        // A muted note, not an empty table, which would read as "no threads".
        std::string text(Detail::threadsStatusText(m_Status));
        if (m_Status == Platform::ThreadsReadStatus::Failed && !m_Detail.empty())
        {
            text += ": " + m_Detail;
        }
        const bool refused = m_Status == Platform::ThreadsReadStatus::PermissionDenied;
        ImGui::PushStyleColor(ImGuiCol_Text, refused ? scheme.textWarning : scheme.textMuted);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        // Another user's process: an administrator can read it (#3069).
        Detail::drawAuthenticateToReadButton(*this);
        return;
    }
    if (m_Truncated)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
        ImGui::TextWrapped("Showing the first %zu threads", m_Rows.size());
        ImGui::PopStyleColor();
    }
    renderTable();
}

void ProcessThreadsView::renderTable()
{
    ImGui::SetNextItemWidth(-1.0F);
    {
        const UI::Widgets::FrameOutlineScope frameOutline;
        ImGui::InputTextWithHint("##ThreadsFilter", ICON_FA_FILTER "  Filter by name or TID", &m_Filter);
    }
    const std::span<const std::size_t> visible = filteredRows();
    if (visible.empty())
    {
        ImGui::TextColored(UI::Theme::get().scheme().textMuted, "No threads match the filter");
        return;
    }

    // Tall enough for the rows up to a cap, plus the header; beyond it the table scrolls on its own.
    const std::size_t shownRows = std::min(visible.size(), Detail::THREADS_TABLE_MAX_VISIBLE_ROWS);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const float tableHeight = rowHeight * static_cast<float>(shownRows + 1); // + 1: the header row

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    // Got here, outside the table, so the popup the rows open is the one renderRowMenu() draws after it.
    const ImGuiID menuId = ImGui::GetID(ROW_MENU_ID);
    if (!ImGui::BeginTable("##ThreadsTable", 7, TABLE_FLAGS, ImVec2(0.0F, tableHeight)))
    {
        return;
    }
    using Detail::ThreadsColumn;
    const auto setupColumn = [](const char* name, float weight, ThreadsColumn column, ImGuiTableColumnFlags flags = 0)
    {
        ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthStretch | flags, weight, static_cast<ImGuiID>(column));
    };
    ImGui::TableSetupScrollFreeze(0, 1);
    setupColumn("TID", 0.10F, ThreadsColumn::Tid);
    setupColumn("NAME", 0.24F, ThreadsColumn::Name);
    setupColumn("STATE", 0.13F, ThreadsColumn::State);
    setupColumn("CPU %", 0.10F, ThreadsColumn::Cpu, ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
    setupColumn("CPU TIME", 0.14F, ThreadsColumn::CpuTime, ImGuiTableColumnFlags_PreferSortDescending);
    // Windows' priorities (0 to 31) aren't nice values (#3069): the same column, its own name.
    setupColumn(m_WindowsPriorities ? "PRIORITY" : "NICE", 0.08F, ThreadsColumn::Nice);
    setupColumn("STARTED", 0.21F, ThreadsColumn::Started); // #3069
    ImGui::TableHeadersRow();

    // A header click (or the first frame's default sort): re-sort the rows once, not every frame.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
            setSort(static_cast<ThreadsColumn>(spec.ColumnUserID), spec.SortDirection != ImGuiSortDirection_Descending);
        }
        specs->SpecsDirty = false;
    }

    const std::span<const std::size_t> ordered = filteredRows();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(ordered.size()), rowHeight);
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const Row& row = m_Rows[ordered[static_cast<std::size_t>(i)]];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableNextColumn();
            if (m_CanActOnThreads)
            {
                // The row menu (#3069), on a copy of the thread taken now: the next read may reorder the rows.
                ImGui::PushID(row.thread.tid);
                static_cast<void>(
                    ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap));
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                {
                    m_MenuThread = row.thread;
                    ImGui::OpenPopup(menuId);
                }
                ImGui::PopID();
                ImGui::SameLine(0.0F, 0.0F);
            }
            UI::Widgets::cellText(row.tid);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.thread.name);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.state);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.cpu);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.cpuTime);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.nice);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.started);
        }
    }
    ImGui::EndTable();
    if (m_CanActOnThreads)
    {
        if (std::exchange(m_OpenRowMenu, false))
        {
            ImGui::OpenPopup(menuId);
        }
        renderRowMenu();
    }
}

void ProcessThreadsView::renderRowMenu()
{
    if (!ImGui::BeginPopup(ROW_MENU_ID))
    {
        return;
    }
    if (m_MenuThread.has_value())
    {
        const Platform::ProcessThread thread = *m_MenuThread; // the item's request may close the menu
        ImGui::TextDisabled("%s", threadLabel(thread.tid, thread.name).c_str());
        ImGui::Separator();
        const bool idle = !m_ActionControl.busy();
        if (ImGui::MenuItem(ICON_FA_PAUSE "  Suspend thread...", nullptr, false, idle))
        {
            requestThreadAction(thread, Platform::ThreadAction::Suspend);
        }
        const bool suspended = Detail::threadCanResume(thread);
        if (ImGui::MenuItem(ICON_FA_PLAY "  Resume thread...", nullptr, false, idle && suspended))
        {
            requestThreadAction(thread, Platform::ThreadAction::Resume);
        }
        if (!suspended)
        {
            ImGui::SetItemTooltip("This thread isn't suspended");
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_SKULL "  End thread...", nullptr, false, idle))
        {
            requestThreadAction(thread, Platform::ThreadAction::End);
        }
        ImGui::SetItemTooltip("Ends this thread alone, at once: it can corrupt the process");
    }
    ImGui::EndPopup();
}

void ProcessThreadsView::renderActionResult()
{
    const auto& scheme = UI::Theme::get().scheme();
    if (m_ActionControl.busy())
    {
        ImGui::TextColored(scheme.textMuted, "%s", m_ActionControl.pendingText().c_str());
        return;
    }
    const Detail::ActionResultMessage& result = m_ActionControl.result();
    if (result.empty())
    {
        return;
    }
    // The colour comes from the result's flag, not its text (#1203).
    ImGui::PushStyleColor(ImGuiCol_Text, result.ok ? scheme.textSuccess : scheme.textError);
    ImGui::TextWrapped("%s", result.text.c_str());
    ImGui::PopStyleColor();
    // A permission refusal an administrator could redo (#2308): the lock and the words, never colour alone.
    if (m_ActionControl.offersAuthenticateAndRetry())
    {
        if (ImGui::SmallButton(AuthorizeWording::RETRY_LABEL))
        {
            m_ActionControl.retryAuthorized();
        }
        ImGui::SetItemTooltip("%s", AuthorizeWording::RETRY_TOOLTIP);
    }
}

void ProcessThreadsView::renderConfirmation(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget)
{
    const std::optional<PendingThreadAction>& pending = m_ActionControl.pending();
    if (!pending.has_value())
    {
        return;
    }
    const Platform::ThreadAction action = pending->action;
    const bool dismiss = pending->target != liveTarget; // another process shown now, or none
    // The shared confirm dialog under an ID of its own, as the dump's is: the Actions block's confirm (Kill,
    // Suspend...) is submitted from the same window every frame, and must never see this one open as its own.
    ImGui::PushID("ThreadAction");
    const ProcessActionConfirm::Outcome outcome = ProcessActionConfirm::renderLabelled(m_ActionControl.showConfirm(),
                                                                                       threadActionConfirmLabel(action),
                                                                                       threadActionIsDestructive(action),
                                                                                       threadActionTitle(action),
                                                                                       m_ActionControl.question(),
                                                                                       dismiss);
    ImGui::PopID();
    switch (outcome)
    {
    case ProcessActionConfirm::Outcome::Confirmed:
        m_ActionControl.confirm(actions);
        break;
    case ProcessActionConfirm::Outcome::Cancelled:
        m_ActionControl.cancel();
        break;
    case ProcessActionConfirm::Outcome::None:
        if (dismiss)
        {
            m_ActionControl.cancel(); // never drawn open: nothing to close
        }
        break;
    }
}

} // namespace App
