#include "FindHandlesDialog.h"

#include "Domain/HandleSearch.h"
#include "Platform/AsciiCase.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/ThreadName.h"
#include "UI/CellText.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <numeric>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{

namespace
{

constexpr const char* POPUP_ID = "###FindHandles";
constexpr const char* CLOSE_LABEL = "Close";
constexpr const char* QUERY_HINT = "A file, folder or library name, or part of its path";
constexpr float BUTTON_MIN_EM = 7.5F;
constexpr float DIALOG_WIDTH_EM = 56.0F;
constexpr float RESULTS_HEIGHT_EM = 20.0F;

using Domain::HandleSearch::HandleMatch;
using Domain::HandleSearch::SearchPublication;

[[nodiscard]] std::strong_ordering compareOn(FindHandlesColumn column, const HandleMatch& a, const HandleMatch& b)
{
    switch (column)
    {
    case FindHandlesColumn::Process:
        return Platform::compareIgnoreCase(a.process, b.process);
    case FindHandlesColumn::Pid:
        return a.target.pid <=> b.target.pid;
    case FindHandlesColumn::Kind:
        return Domain::HandleSearch::kindLabel(a.kind) <=> Domain::HandleSearch::kindLabel(b.kind);
    case FindHandlesColumn::Path:
        return Platform::compareIgnoreCase(a.path, b.path);
    }
    return std::strong_ordering::equal;
}

} // namespace

std::string findHandlesStatus(const SearchPublication& publication)
{
    if (publication.version == 0)
    {
        return "Type a name and press Find.";
    }
    const std::size_t found = publication.matches.size();
    std::string status = publication.done ? std::format("{} processes searched", publication.searched)
                                          : std::format("Searching... {} of {} processes", publication.searched, publication.total);
    status += std::format(" \xC2\xB7 {} found{}", found, publication.truncated ? " (the first ones)" : "");
    if (!publication.denied.empty())
    {
        status += std::format(" \xC2\xB7 {} couldn't be read", publication.denied.size());
    }
    if (publication.failed > 0)
    {
        status += std::format(" \xC2\xB7 {} failed", publication.failed);
    }
    if (publication.exited > 0)
    {
        status += std::format(" \xC2\xB7 {} ended first", publication.exited);
    }
    return status;
}

std::vector<std::size_t> orderFindHandlesMatches(const std::vector<HandleMatch>& matches, FindHandlesColumn column, bool ascending)
{
    std::vector<std::size_t> order(matches.size());
    std::ranges::iota(order, std::size_t{0});
    std::ranges::stable_sort(order,
                             [&matches, column, ascending](std::size_t ia, std::size_t ib)
                             {
                                 const HandleMatch& a = matches[ia];
                                 const HandleMatch& b = matches[ib];
                                 std::strong_ordering order = compareOn(column, a, b);
                                 if (order == 0)
                                 {
                                     order = a.target.pid <=> b.target.pid;
                                 }
                                 if (order == 0)
                                 {
                                     return a.path < b.path; // ties stay A-Z whichever way the column sorts
                                 }
                                 return ascending ? order < 0 : order > 0;
                             });
    return order;
}

FindHandlesDialog::~FindHandlesDialog()
{
    // The worker stops between processes (or inside a password prompt) and is joined before the readers go.
    if (m_Worker.joinable())
    {
        m_Worker.request_stop();
        m_Worker.join();
    }
}

FindHandlesDialog::Capabilities FindHandlesDialog::capabilitiesOf(const Platform::IProcessOpenFilesReader* openFiles,
                                                                  const Platform::IProcessModulesReader* modules)
{
    return Capabilities{
        .openFiles = openFiles != nullptr && openFiles->hasOpenFiles(),
        .modules = modules != nullptr && modules->hasModules(),
        .authorize = (openFiles != nullptr && openFiles->canReadAuthorized()) || (modules != nullptr && modules->canReadAuthorized()),
    };
}

void FindHandlesDialog::setReaders(std::unique_ptr<Platform::IProcessOpenFilesReader> openFiles,
                                   std::unique_ptr<Platform::IProcessModulesReader> modules,
                                   Capabilities capabilities)
{
    m_OpenFiles = std::move(openFiles);
    m_Modules = std::move(modules);
    m_Capabilities = capabilities;
}

bool FindHandlesDialog::searching() const noexcept
{
    const std::shared_ptr<const SearchPublication> current = m_Slot->load();
    return current != nullptr && current->version > 0 && !current->done;
}

void FindHandlesDialog::startSearch(std::vector<Domain::HandleSearch::SearchProcess> processes, bool authorized, SearchPublication start)
{
    if (m_Worker.joinable())
    {
        m_Worker.request_stop();
        m_Worker.join(); // it has already published "done"; this only waits for it to return
    }
    // Published here, so searching() is true from this frame on.
    m_Slot->commit(std::make_shared<const SearchPublication>(start));
    const Domain::HandleSearch::Readers readers{
        .openFiles = m_Capabilities.openFiles ? m_OpenFiles.get() : nullptr,
        .modules = m_Capabilities.modules ? m_Modules.get() : nullptr,
    };
    m_Worker = std::jthread(
        [slot = m_Slot, readers, authorized, processes = std::move(processes), start = std::move(start)](
            const std::stop_token& stopToken) mutable
        {
            static_cast<void>(Platform::setCurrentThreadName(Platform::FIND_HANDLES_THREAD_NAME));
            Domain::HandleSearch::run(processes, readers, authorized, std::move(start), *slot, stopToken);
        });
}

bool FindHandlesDialog::renderQueryRow()
{
    const bool running = searching();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonWidth = UI::Widgets::footerButtonWidth({"Find", "Stop"}, BUTTON_MIN_EM);
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 8.0F, ImGui::GetContentRegionAvail().x - buttonWidth - style.ItemSpacing.x));
    if (std::exchange(m_FocusQuery, false))
    {
        ImGui::SetKeyboardFocusHere();
    }
    bool entered = false;
    {
        const UI::Widgets::FrameOutlineScope frameOutline;
        entered = ImGui::InputTextWithHint("##query", QUERY_HINT, &m_Query, ImGuiInputTextFlags_EnterReturnsTrue);
    }
    ImGui::SameLine();
    if (running)
    {
        if (ImGui::Button("Stop", ImVec2(buttonWidth, 0.0F)))
        {
            m_Worker.request_stop();
        }
        return false;
    }
    const bool typed = m_Query.find_first_not_of(" \t") != std::string::npos;
    ImGui::BeginDisabled(!typed);
    const bool pressed = ImGui::Button("Find", ImVec2(buttonWidth, 0.0F));
    ImGui::EndDisabled();
    return typed && (pressed || entered);
}

std::optional<FindHandlesAction> FindHandlesDialog::renderResults(const SearchPublication& publication)
{
    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY |
                                            ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
    const float em = ImGui::GetFontSize();
    if (!ImGui::BeginTable("##FindHandlesResults", 4, TABLE_FLAGS, ImVec2(0.0F, em * RESULTS_HEIGHT_EM)))
    {
        return std::nullopt;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_DefaultSort, em * 10.0F, static_cast<ImGuiID>(FindHandlesColumn::Process));
    ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_None, em * 4.5F, static_cast<ImGuiID>(FindHandlesColumn::Pid));
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_None, em * 5.0F, static_cast<ImGuiID>(FindHandlesColumn::Kind));
    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.0F, static_cast<ImGuiID>(FindHandlesColumn::Path));
    ImGui::TableHeadersRow();
    bool reorder = m_OrderVersion != publication.version;
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            m_SortColumn = static_cast<FindHandlesColumn>(specs->Specs[0].ColumnUserID);
            m_SortAscending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
        }
        specs->SpecsDirty = false;
        reorder = true;
    }
    if (reorder)
    {
        m_Order = orderFindHandlesMatches(publication.matches, m_SortColumn, m_SortAscending);
        m_OrderVersion = publication.version;
    }

    std::optional<FindHandlesAction> action;
    const auto& scheme = UI::Theme::get().scheme();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_Order.size()));
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const HandleMatch& match = publication.matches[m_Order[static_cast<std::size_t>(i)]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Selectable(
                    "##row", m_Selected == match.target, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
            {
                // Selecting a row selects its process; a double-click shows it in Process Details.
                m_Selected = match.target;
                action = FindHandlesAction{.target = match.target, .showDetails = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)};
            }
            ImGui::PopID();
            ImGui::SameLine(0.0F, 0.0F);
            UI::Widgets::cellText(match.process);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(std::to_string(match.target.pid));
            ImGui::TableNextColumn();
            UI::Widgets::cellText(Domain::HandleSearch::kindLabel(match.kind));
            ImGui::TableNextColumn();
            if (match.deleted)
            {
                ImGui::TextColored(scheme.textMuted, "%s (deleted)", match.path.c_str());
            }
            else
            {
                UI::Widgets::cellText(match.path);
            }
        }
    }
    ImGui::EndTable();
    return action;
}

std::optional<FindHandlesAction> FindHandlesDialog::render(const ProcessList& processes)
{
    if (m_ShowRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        m_ShowRequested = false;
        m_FocusQuery = true;
    }
    m_Open = ImGui::IsPopupOpen(POPUP_ID);
    if (!m_Open)
    {
        return std::nullopt;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const std::string title = std::string(ICON_FA_MAGNIFYING_GLASS "  Find handles") + POPUP_ID;
    UI::Widgets::centerNextDialog(m_LastWorkSize);
    const float width = std::min((DIALOG_WIDTH_EM * ImGui::GetFontSize()) + (ImGui::GetStyle().WindowPadding.x * 2.0F),
                                 UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0F));
    std::optional<FindHandlesAction> action;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return action;
    }
    UI::Widgets::keepCurrentWindowInViewport();
    const bool escaped = UI::Widgets::dialogEscapePressed();
    const auto& theme = UI::Theme::get();

    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted("Which processes have a file or folder open, or a library loaded, whose path contains this text.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (renderQueryRow())
    {
        std::vector<Domain::HandleSearch::SearchProcess> all = processes ? processes() : std::vector<Domain::HandleSearch::SearchProcess>{};
        const std::uint64_t version = m_Slot->version() + 1;
        Domain::HandleSearch::SearchPublication start = Domain::HandleSearch::startFor(m_Query, all.size(), version);
        m_Selected.reset();
        startSearch(std::move(all), false, std::move(start));
    }

    const std::shared_ptr<const SearchPublication> current = m_Slot->load();
    const SearchPublication empty;
    const SearchPublication& shown = current != nullptr ? *current : empty;
    action = renderResults(shown);
    ImGui::TextColored(theme.scheme().textMuted, "%s", findHandlesStatus(shown).c_str());

    // Another user's processes: an administrator can search them, after one password prompt.
    if (shown.done && !shown.denied.empty() && m_Capabilities.authorize)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_LOCK " Authenticate to search them too"))
        {
            std::vector<Domain::HandleSearch::SearchProcess> denied = shown.denied;
            startSearch(std::move(denied), true, Domain::HandleSearch::startAuthorizedAfter(shown, m_Slot->version() + 1));
        }
        ImGui::SetItemTooltip("Asks for an administrator's password, then searches the processes this run may not read");
    }

    const UI::Widgets::DialogFooterButton closeButton{.label = CLOSE_LABEL, .fills = nullptr, .tooltip = nullptr};
    const UI::Widgets::DialogFooterAction pressed =
        UI::Widgets::dialogFooter(closeButton, {}, UI::Widgets::footerButtonWidth({CLOSE_LABEL}, BUTTON_MIN_EM));
    // A double-click goes to Process Details, another tab: the dialog closes on the way.
    if (pressed == UI::Widgets::DialogFooterAction::Primary || escaped || (action.has_value() && action->showDetails))
    {
        // A search still running keeps going: its results are there when the dialog opens again.
        m_Open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return action;
}

} // namespace App
