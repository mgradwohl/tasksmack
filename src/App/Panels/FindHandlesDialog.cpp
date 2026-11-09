#include "FindHandlesDialog.h"

#include "AuthorizeWording.h"
#include "Domain/HandleSearch.h"
#include "Domain/Worker.h"
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
#include <span>
#include <stop_token>
#include <string>
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

std::string findHandlesStatus(const SearchPublication& publication, bool workerReturned)
{
    if (publication.version == 0)
    {
        return "Type a name and press Find.";
    }
    const std::size_t found = publication.matches.size();
    const bool stoppedUnpublished = !publication.done && workerReturned;
    std::string status = (publication.done || stoppedUnpublished)
                           ? std::format("{} processes searched", publication.searched)
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
    if (publication.partlyNamed > 0)
    {
        status += std::format(" \xC2\xB7 {} only partly read", publication.partlyNamed); // a handle didn't answer (Windows)
    }
    if (!publication.failure.empty())
    {
        status += " \xC2\xB7 " + publication.failure; // a reader threw or memory ran out (#3335)
    }
    else if (stoppedUnpublished)
    {
        status += " \xC2\xB7 The search stopped before it finished"; // not even its failure could be published
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

void runFindHandlesSearch(std::span<const Domain::HandleSearch::SearchProcess> processes,
                          const Domain::HandleSearch::Readers& readers,
                          bool authorized,
                          SearchPublication start,
                          const std::stop_source& cancel,
                          const std::stop_token& workerStop,
                          const Domain::HandleSearch::Publish& publish) noexcept
{
    // Stop pressed while the job was queued signalled only the search's own source: then it runs with that token,
    // already signalled, and reads nothing. Otherwise it runs with the worker's, which Stop signals too
    // (cancelCurrent()) once the job is in flight, as do the worker's stop and destructor.
    const std::stop_token stop = cancel.stop_requested() ? cancel.get_token() : workerStop;
    Domain::HandleSearch::run(processes, readers, authorized, std::move(start), publish, stop);
}

FindHandlesDialog::~FindHandlesDialog()
{
    // The worker stops between processes (or inside a password prompt) and is joined before the readers go.
    m_Worker.reset();
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

std::shared_ptr<const SearchPublication> FindHandlesDialog::publication() const noexcept
{
    if (m_Worker != nullptr)
    {
        // The worker's generations carry the search's own version, so one older than the search just begun is the
        // last search's: the begun one shows until the worker gets to it.
        if (const auto latest = m_Worker->latest();
            latest != nullptr && latest->version > 0 && (m_Begun == nullptr || latest->value.version >= m_Begun->version))
        {
            return {latest, &latest->value}; // shares the publication's ownership
        }
    }
    return m_Begun;
}

bool FindHandlesDialog::jobActive() const noexcept
{
    return m_Worker != nullptr && (m_Worker->queued() > 0 || m_Worker->busy());
}

bool FindHandlesDialog::searching() const noexcept
{
    // The job first: it ends after its last publication, so once it has, that publication is the one read.
    if (!jobActive())
    {
        return false;
    }
    const std::shared_ptr<const SearchPublication> current = publication();
    return current != nullptr && current->version > 0 && !current->done;
}

void FindHandlesDialog::failToStart() noexcept
{
    // No thread, or no memory for the job (#3335): the search just begun ends as one that couldn't start, if even
    // that can be shown. When it can't, the begun one stays, and with no job behind it, it doesn't read as running.
    try
    {
        if (m_Begun != nullptr)
        {
            SearchPublication failed = *m_Begun;
            failed.done = true;
            failed.failure = "The search couldn't start";
            ++failed.version;
            m_Begun = std::make_shared<const SearchPublication>(std::move(failed));
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch) - nothing is left to report with; see above
    {}
}

void FindHandlesDialog::startSearch(std::vector<Domain::HandleSearch::SearchProcess> processes, bool authorized, SearchPublication start)
{
    try
    {
        m_Begun = std::make_shared<const SearchPublication>(start); // so searching() is true from this frame on
    }
    catch (...)
    {
        return; // no memory even to show it begin: the search doesn't, and the last results stay
    }
    // A failed worker takes no more work: the next search gets a new one.
    if (m_Worker != nullptr && m_Worker->state() != Domain::WorkerState::Running)
    {
        m_Worker.reset();
    }
    try
    {
        if (m_Worker == nullptr)
        {
            m_Worker = std::make_unique<SearchWorker>(
                SearchWorker::Options{
                    .threadName = Platform::FIND_HANDLES_THREAD_NAME,
                    .capacity = 1,
                    .admission = Domain::Admission::DropOldest,
                },
                [](SearchJob& job, const std::stop_token& stop, SearchWorker::JobContext& context)
                {
                    // Every generation wakes the frame loop (the worker's finish() and progress()),
                    // so the progress line moves as the walk does. run() catches what publishing
                    // throws.
                    runFindHandlesSearch(job.processes,
                                         job.readers,
                                         job.authorized,
                                         std::move(job.start),
                                         job.cancel,
                                         stop,
                                         [&context](SearchPublication generation)
                                         {
                                             if (generation.done)
                                             {
                                                 context.finish(std::move(generation));
                                             }
                                             else
                                             {
                                                 context.progress(std::move(generation));
                                             }
                                         });
                });
        }
        SearchJob job{
            .processes = std::move(processes),
            .readers =
                {
                    .openFiles = m_Capabilities.openFiles ? m_OpenFiles.get() : nullptr,
                    .modules = m_Capabilities.modules ? m_Modules.get() : nullptr,
                },
            .authorized = authorized,
            .start = std::move(start),
            .cancel = std::stop_source{}, // allocates its shared state: inside the try
        };
        m_SearchCancel = job.cancel;
        // A search starts only once the last one ended (Find and Authenticate show only then), so nothing is in
        // flight to cancel: the worker's capacity of one and DropOldest keep it so regardless.
        const Domain::SubmitResult submitted = m_Worker->submit(std::move(job));
        if (submitted == Domain::SubmitResult::Refused || submitted == Domain::SubmitResult::Closed)
        {
            failToStart();
        }
    }
    catch (...)
    {
        failToStart();
    }
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
            // The search's own stop, for a job still queued, and the worker's job in flight: the walk stops between
            // processes and publishes what it has as done.
            m_SearchCancel.request_stop();
            if (m_Worker != nullptr)
            {
                m_Worker->cancelCurrent(); // the job in flight, if it is no longer queued
            }
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
        const std::shared_ptr<const SearchPublication> last = publication();
        const std::uint64_t version = (last != nullptr ? last->version : 0) + 1;
        Domain::HandleSearch::SearchPublication start = Domain::HandleSearch::startFor(m_Query, all.size(), version);
        m_Selected.reset();
        startSearch(std::move(all), false, std::move(start));
    }

    const bool workerReturned = !jobActive(); // before the load, as in searching()
    const std::shared_ptr<const SearchPublication> current = publication();
    const SearchPublication empty;
    const SearchPublication& shown = current != nullptr ? *current : empty;
    action = renderResults(shown);
    ImGui::TextColored(theme.scheme().textMuted, "%s", findHandlesStatus(shown, workerReturned).c_str());

    // Another user's processes (or, on Windows, an elevated or protected one): an administrator can search them,
    // after one prompt.
    if (shown.done && !shown.denied.empty() && m_Capabilities.authorize)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(AuthorizeWording::SEARCH_LABEL))
        {
            std::vector<Domain::HandleSearch::SearchProcess> denied = shown.denied;
            startSearch(std::move(denied), true, Domain::HandleSearch::startAuthorizedAfter(shown, shown.version + 1));
        }
        ImGui::SetItemTooltip("%s", AuthorizeWording::SEARCH_TOOLTIP);
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
