#pragma once

// Find handles (#3067): "which process is holding this file?". A dialog from the Processes toolbar (or
// Ctrl+Shift+F there) that searches every process's open files and loaded modules for a name, on its own
// worker (Domain/HandleSearch.h on a Domain::Worker, #3343), and lists each process and what it holds. Processes this run may not
// read (another user's) can be searched again as an administrator, after one password prompt.

#include "Domain/HandleSearch.h"
#include "Domain/Worker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace App
{

/// What a click in the results asks the Processes panel to do.
struct FindHandlesAction
{
    Platform::ProcessTarget target;
    bool showDetails = false; ///< A double-click: show the process in Process Details too.
};

/// The status line under the results: how far the search has got, what it found, and what it couldn't read.
/// @p workerReturned: the search's job on the worker has ended, so a search that isn't done stopped without its last publication
/// (#3335), and is shown as stopped with its last counts rather than as searching.
[[nodiscard]] std::string findHandlesStatus(const Domain::HandleSearch::SearchPublication& publication, bool workerReturned);

/// One Find handles search as its worker runs it (#3343): HandleSearch::run() over @p processes, handing each
/// generation to @p publish. @p cancel is the search's own: the dialog's Stop signals it as well as the worker's job
/// in flight, so a search stopped while still queued on the worker reads nothing (and asks for no password) once it
/// runs. Otherwise the walk stops on @p workerStop: the worker's cancel (Stop, once the job is in flight), its stop
/// and its destructor. @p sources is what to read for each process (HandleSearch::run()): an administrator's search reads only
/// the sources the first search was refused (#3357); empty reads both.
void runFindHandlesSearch(std::span<const Domain::HandleSearch::SearchProcess> processes,
                          const Domain::HandleSearch::Readers& readers,
                          bool authorized,
                          Domain::HandleSearch::SearchPublication start,
                          const std::stop_source& cancel,
                          const std::stop_token& workerStop,
                          const Domain::HandleSearch::Publish& publish,
                          std::span<const Domain::HandleSearch::SearchSources> sources = {}) noexcept;

/// A results column of the Find handles table.
enum class FindHandlesColumn : std::uint8_t
{
    Process,
    Pid,
    Kind,
    Path,
};

/// The order to list @p matches in: by @p column, ties by PID then path.
[[nodiscard]] std::vector<std::size_t>
orderFindHandlesMatches(const std::vector<Domain::HandleSearch::HandleMatch>& matches, FindHandlesColumn column, bool ascending);

class FindHandlesDialog
{
  public:
    /// The processes to search: the latest snapshot's, read when Find is pressed.
    using ProcessList = std::function<std::vector<Domain::HandleSearch::SearchProcess>()>;

    FindHandlesDialog() = default;
    ~FindHandlesDialog();

    FindHandlesDialog(const FindHandlesDialog&) = delete;
    FindHandlesDialog& operator=(const FindHandlesDialog&) = delete;
    FindHandlesDialog(FindHandlesDialog&&) = delete;
    FindHandlesDialog& operator=(FindHandlesDialog&&) = delete;

    /// What the readers can do, asked where they were made (the Processes sampler thread), so render()
    /// never calls into a reader on the UI thread.
    struct Capabilities
    {
        bool openFiles = false; ///< openFiles->hasOpenFiles()
        bool modules = false;   ///< modules->hasModules()
        bool authorize = false; ///< either reader's canReadAuthorized()
    };

    /// @p readers' capabilities, asked now: call it on the thread that made them.
    [[nodiscard]] static Capabilities capabilitiesOf(const Platform::IProcessOpenFilesReader* openFiles,
                                                     const Platform::IProcessModulesReader* modules);

    /// The readers searches use (either may be null) and what they can do. Set once, before the first open().
    void setReaders(std::unique_ptr<Platform::IProcessOpenFilesReader> openFiles,
                    std::unique_ptr<Platform::IProcessModulesReader> modules,
                    Capabilities capabilities);

    /// Whether this run can search anything: the toolbar shows Find handles only then.
    [[nodiscard]] bool available() const noexcept
    {
        return m_Capabilities.openFiles || m_Capabilities.modules;
    }

    /// Opens the dialog on the next render(), keeping the last query and results.
    void open() noexcept
    {
        m_ShowRequested = true;
    }

    [[nodiscard]] bool isOpen() const noexcept
    {
        return m_ShowRequested || m_Open;
    }

    /// Draws the dialog while it is open. Returns what a click in the results asks for.
    [[nodiscard]] std::optional<FindHandlesAction> render(const ProcessList& processes);

    /// The newest results: the worker's latest, or the search just begun until the worker publishes for it. Null
    /// before the first search.
    [[nodiscard]] std::shared_ptr<const Domain::HandleSearch::SearchPublication> publication() const noexcept;

    /// Whether a search is running on the worker: one has begun, isn't done, and its job is still queued or in
    /// flight (a search that couldn't even publish its failure still ends when its job does, #3335).
    [[nodiscard]] bool searching() const noexcept;

  private:
    /// One search: what the worker runs.
    struct SearchJob
    {
        std::vector<Domain::HandleSearch::SearchProcess> processes;
        std::vector<Domain::HandleSearch::SearchSources> sources; ///< Per process; empty reads both (#3357)
        Domain::HandleSearch::Readers readers;
        bool authorized = false;
        Domain::HandleSearch::SearchPublication start;
        std::stop_source cancel{std::nostopstate}; ///< The search's own stop (runFindHandlesSearch()).
    };
    using SearchWorker = Domain::Worker<SearchJob, Domain::HandleSearch::SearchPublication>;

    /// Whether the worker has a job queued or in flight. The queue first: a job moves from it to in flight under
    /// one lock, so reading in this order never misses one between the two.
    [[nodiscard]] bool jobActive() const noexcept;
    /// Ends the search just begun as one that couldn't start, so the status line says so.
    void failToStart() noexcept;
    /// Starts a search of @p processes on the worker, reading @p sources of each (empty: both); @p start carries the query and
    /// anything found before.
    void startSearch(std::vector<Domain::HandleSearch::SearchProcess> processes,
                     std::vector<Domain::HandleSearch::SearchSources> sources,
                     bool authorized,
                     Domain::HandleSearch::SearchPublication start);
    /// The query row: the box, Find and Stop. True when a search should start.
    [[nodiscard]] bool renderQueryRow();
    [[nodiscard]] std::optional<FindHandlesAction> renderResults(const Domain::HandleSearch::SearchPublication& publication);

    std::unique_ptr<Platform::IProcessOpenFilesReader> m_OpenFiles;
    std::unique_ptr<Platform::IProcessModulesReader> m_Modules;
    Capabilities m_Capabilities;
    /// The last search begun here, shown until the worker publishes one of its generations (or for good, when it
    /// couldn't start).
    std::shared_ptr<const Domain::HandleSearch::SearchPublication> m_Begun;
    /// The last search's own stop, shared with its job: Stop signals it whether the job is queued or running.
    std::stop_source m_SearchCancel{std::nostopstate};
    std::string m_Query;
    std::uint64_t m_OrderVersion = 0; ///< The publication m_Order was built for
    FindHandlesColumn m_SortColumn = FindHandlesColumn::Process;
    bool m_SortAscending = true;
    std::vector<std::size_t> m_Order;
    std::optional<Platform::ProcessTarget> m_Selected;
    bool m_ShowRequested = false;
    bool m_Open = false;
    bool m_FocusQuery = false;
    ImVec2 m_LastWorkSize; // For UI::Widgets::centerNextDialog()
    /// Last: stopped and joined first, before the readers its job uses go. Made with the first search, and again
    /// after one that failed (a failed worker takes no more work).
    std::unique_ptr<SearchWorker> m_Worker;
};

} // namespace App
