// The Processes table's grouped rows (#3815): Group By's groups, rebuilt when stale, and their header rows. Split
// from ProcessesPanel.cpp, which draws every other row; the User groups' sessions are ProcessesPanelUserGroups.cpp.

#include "App/Panels/ProcessGroupTotals.h"
#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/Panels/SessionActionView.h"
#include "App/Panels/UserGrouping.h"
#include "App/Panels/UserSessionsFeed.h"
#include "App/ProcessColumnConfig.h"
#include "App/UserConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/UsersModel.h"
#include "Platform/IUserSessions.h"
#include "Platform/ProcessTypes.h"
#include "UI/CellText.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"

// clang-format off
#include <imgui.h>
#include <imgui_internal.h> // ImGuiTable: the laid-out column widths, and the table background channel, have no public accessor
#include <misc/cpp/imgui_stdlib.h>
#include <spdlog/spdlog.h>
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace App
{

namespace
{

/// A group header's total, right-aligned in its cell, with the clipped-text tooltip (as ProcessesPanel.cpp draws a
/// row's numbers).
void renderRightAlignedGroupText(std::string_view text, float textWidth)
{
    static_cast<void>(UI::Widgets::cellText(text, textWidth, {.rightAligned = true, .clippedTooltip = true, .layoutFullText = false}));
}

} // namespace

void ProcessesPanel::setGroupBy(ProcessGrouping::GroupBy groupBy)
{
    // Choosing None while a saved mode this system doesn't offer stands in for it (#3428) replaces the
    // saved mode, so the comparison is with both.
    ProcessGrouping::GroupBy& saved = UserConfig::get().settings().processGroupBy;
    if (groupBy == m_GroupBy && groupBy == saved)
    {
        return;
    }
    m_GroupBy = groupBy;
    saved = groupBy; // saved with the other settings
    m_GroupsBuiltFor.reset();
    if (groupBy != ProcessGrouping::GroupBy::None && m_TreeViewEnabled)
    {
        // A group's members are listed flat under its header: back to the List view, as its control does.
        m_TreeViewEnabled = false;
        m_NameWidthSyncPending = true;
        m_SortPending = true;
    }
    spdlog::debug("ProcessesPanel: grouped by {}", ProcessGrouping::groupByLabel(groupBy));
}

void ProcessesPanel::rebuildGroupsIfStale(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    // Group: User's sessions (#3377): the publication refreshSessions() took, when it was read.
    const Domain::UsersPublication* const sessions = (m_GroupBy == ProcessGrouping::GroupBy::User && m_SessionsPublication &&
                                                      m_SessionsPublication->status == Platform::UserSessionsStatus::Ok)
                                                       ? m_SessionsPublication.get()
                                                       : nullptr;
    const GroupsKey key{
        .displayVersion = m_DisplayVersion,
        .filterGeneration = m_FilterGeneration,
        .sortGeneration = m_SortGeneration,
        .collapseGeneration = m_GroupCollapseGeneration,
        .sessionsVersion = sessions != nullptr ? sessions->version : 0,
        .groupBy = m_GroupBy,
    };
    if (m_GroupsBuiltFor == key)
    {
        return;
    }
    m_GroupsBuiltFor = key;
    // The sorted, filtered rows (pinned first, #3061) gathered by group; an exited row (#3060) is listed
    // under its group but not counted in the totals. Grouped by User, each signed-in account with its sessions.
    m_Groups = sessions != nullptr
                 ? ProcessGrouping::buildGroups(
                       snapshots, m_CachedSortedIndices, m_GroupBy, m_LiveRowCount, m_SortColumn, m_SortAscending, sessions->sessions)
                 : ProcessGrouping::buildGroups(snapshots, m_CachedSortedIndices, m_GroupBy, m_LiveRowCount, m_SortColumn, m_SortAscending);
    m_GroupRowTexts.assign(m_Groups.size(), {}); // their text is rebuilt as each is drawn (#3807)
    const bool filtered = !m_CachedSearchTerm.empty();
    m_GroupedRows.clear();
    m_GroupedRows.reserve(m_CachedSortedIndices.size() + (m_Groups.size() * 2));
    for (std::size_t g = 0; g < m_Groups.size(); ++g)
    {
        const ProcessGrouping::Group& group = m_Groups[g];
        m_GroupedRows.push_back({.kind = GroupedRowKind::Header, .group = g, .procIdx = 0});
        if (m_CollapsedGroups.contains(group.key))
        {
            continue;
        }
        // Its sessions first, collapsing with it (#3377).
        for (std::size_t s = 0; s < group.sessions.size(); ++s)
        {
            m_GroupedRows.push_back({.kind = GroupedRowKind::Session, .group = g, .procIdx = s});
        }
        // A signed-in account the filter leaves no process of still shows, and says why it has none.
        if (filtered && group.members.empty() && !group.sessions.empty() && !group.notReadable)
        {
            m_GroupedRows.push_back({.kind = GroupedRowKind::NoMatches, .group = g, .procIdx = 0});
        }
        for (const std::size_t member : group.members)
        {
            m_GroupedRows.push_back({.kind = GroupedRowKind::Process, .group = g, .procIdx = member});
        }
    }
}

void ProcessesPanel::renderGroupedRows(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    rebuildGroupsIfStale(snapshots);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_GroupedRows.size()));
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const GroupedRow& row = m_GroupedRows[static_cast<std::size_t>(i)];
            switch (row.kind)
            {
            case GroupedRowKind::Header:
                renderGroupHeaderRow(row.group);
                break;
            case GroupedRowKind::Session:
                if (const auto& groupSessions = m_Groups[row.group].sessions; row.procIdx < groupSessions.size())
                {
                    renderSessionRow(row.group, row.procIdx);
                }
                break;
            case GroupedRowKind::NoMatches:
                renderNoMatchesRow();
                break;
            case GroupedRowKind::Process:
                if (row.procIdx < snapshots.size())
                {
                    renderProcessRow(snapshots[row.procIdx],
                                     {.procIdx = row.procIdx, .depth = 1, .hasChildren = false, .isExpanded = false, .isContext = false});
                }
                break;
            }
        }
    }
}

ProcessesPanel::GroupRowTexts& ProcessesPanel::groupRowTexts(std::size_t groupIndex)
{
    GroupRowTexts& texts = m_GroupRowTexts[groupIndex];
    const ProcessGrouping::Group& group = m_Groups[groupIndex];
    // Idle times are told in minutes: their text changes at most once a minute.
    const std::uint64_t now = UserGrouping::nowUsec();
    const std::uint64_t minute = now / 60'000'000U;
    if (texts.statusMinute != minute)
    {
        texts.statusMinute = minute;
        texts.headerStatus = group.sessions.empty() ? std::string{} : UserGrouping::groupStatus(group, now);
        texts.sessionStatus.clear();
        texts.sessionStatus.reserve(group.sessions.size());
        for (const Platform::UserSession& session : group.sessions)
        {
            texts.sessionStatus.push_back(UserGrouping::sessionStatus(session, now));
        }
    }
    return texts;
}

void ProcessesPanel::renderGroupHeaderRow(std::size_t groupIndex)
{
    const ProcessGrouping::Group& group = m_Groups[groupIndex];
    const bool collapsed = m_CollapsedGroups.contains(group.key);
    GroupRowTexts& texts = groupRowTexts(groupIndex);
    ImGui::TableNextRow();
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
    int colIdx = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        const bool columnVisible = ImGui::TableSetColumnIndex(colIdx);
        ++colIdx;
        if (col == ProcessColumn::PID)
        {
            // The row-wide click target, entered even when PID is scrolled out of view, as a process row's is.
            ImGui::PushID(static_cast<int>(groupIndex));
            if (ImGui::Selectable("##group", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                if (collapsed)
                {
                    m_CollapsedGroups.erase(group.key);
                }
                else
                {
                    m_CollapsedGroups.insert(group.key);
                }
                ++m_GroupCollapseGeneration;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) // built only when shown, not every frame
            {
                const bool othersHidden = (processCapabilities().missingPrivileges & Platform::MissingPrivilege::PROCESSES_HIDDEN) != 0;
                ImGui::SetTooltip("%s", UserGrouping::groupHeaderTooltip(group, collapsed, othersHidden).c_str());
            }
            // Group: User (#3377): an account with one sign-in session offers its menu on the header too.
            if (const Platform::UserSession* const session = UserGrouping::headerMenuSession(group);
                session != nullptr && m_UserSessions != nullptr && m_UserSessions->sessionMenu().any() &&
                ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                m_SessionMenuTarget = *session;
                ImGui::OpenPopup(m_SessionMenuPopupId);
            }
            ImGui::PopID();
            continue;
        }
        if (!columnVisible)
        {
            continue;
        }
        if (col == ProcessColumn::Name)
        {
            // A signed-in account (#3377): its icon, "(you)" for TaskSmack's own, and its process count. Not
            // readable has sessions and never a process, so no count.
            // Built once per groups rebuild (#3807): a collapse or expand rebuilds them, so the caret is current.
            std::string& label = texts.headerName;
            const char* const caret = collapsed ? ICON_FA_CARET_RIGHT : ICON_FA_CARET_DOWN;
            if (label.empty())
            {
                ++m_GroupHeaderTextBuilds;
                if (group.key.empty())
                {
                    label = std::format("{} {} ({})", caret, ProcessGrouping::UNGROUPED_LABEL, group.members.size());
                }
                else if (group.notReadable)
                {
                    label = std::format("{} {}", caret, group.label);
                }
                else
                {
                    label = std::format("{} {}{} ({})",
                                        caret,
                                        group.sessions.empty() ? "" : ICON_FA_USER "  ",
                                        UserGrouping::groupHeaderName(group),
                                        group.members.size());
                }
            }
            static_cast<void>(
                UI::Widgets::cellText(label,
                                      ImGui::CalcTextSize(label.c_str()).x,
                                      {.rightAligned = false, .clippedTooltip = true, .layoutFullText = false, .trailingReservePx = 0.0F}));
            continue;
        }
        if (col == ProcessColumn::Status && !group.sessions.empty())
        {
            // Its one session's status, or how many it has (#3377), rebuilt once a minute (#3807).
            UI::Widgets::cellText(texts.headerStatus);
            continue;
        }
        std::optional<std::string>& cached = texts.totals[toIndex(col)];
        if (cached.has_value())
        {
            renderRightAlignedGroupText(*cached, ImGui::CalcTextSize(cached->c_str()).x);
            continue;
        }
        const std::optional<double> total = ProcessGrouping::totalFor(group.totals, col);
        if (!total.has_value())
        {
            continue;
        }
        // Marked with Σ: a rollup must never read as one process's figure (#838). Formatted once per groups rebuild.
        std::string text;
        switch (col)
        {
        case ProcessColumn::CpuPercent:
        case ProcessColumn::MemPercent:
        case ProcessColumn::GpuPercent:
        case ProcessColumn::NpuPercent:
            text = UI::Format::formatPercent(*total);
            break;
        case ProcessColumn::IoRead:
        case ProcessColumn::IoWrite:
        case ProcessColumn::NetSent:
        case ProcessColumn::NetReceived:
            text = UI::Format::formatBytesPerSec(*total);
            break;
        case ProcessColumn::Threads:
        case ProcessColumn::Handles:
            text = UI::Format::formatIntLocalized(static_cast<std::int64_t>(*total));
            break;
        case ProcessColumn::CpuTime:
            text = UI::Format::formatDuration(*total);
            break;
        case ProcessColumn::Power:
            text = UI::Format::formatWatts(*total);
            break;
        default:
            text = UI::Format::formatBytes(*total);
            break;
        }
        text.insert(0, "\xCE\xA3 "); // U+03A3
        renderRightAlignedGroupText(text, ImGui::CalcTextSize(text.c_str()).x);
        cached = std::move(text);
    }
}

} // namespace App
