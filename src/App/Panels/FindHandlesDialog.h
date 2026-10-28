#pragma once

// Find handles (#3067): "which process is holding this file?". A dialog from the Processes toolbar (or
// Ctrl+Shift+F there) that searches every process's open files and loaded modules for a name, on its own
// worker (Domain/HandleSearch.h), and lists each process and what it holds. Processes this run may not
// read (another user's) can be searched again as an administrator, after one password prompt.

#include "Domain/HandleSearch.h"
#include "Domain/PublicationSlot.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
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
[[nodiscard]] std::string findHandlesStatus(const Domain::HandleSearch::SearchPublication& publication);

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

    /// The newest results (for tests).
    [[nodiscard]] std::shared_ptr<const Domain::HandleSearch::SearchPublication> publication() const noexcept
    {
        return m_Slot->load();
    }

    /// Whether a search is running on the worker.
    [[nodiscard]] bool searching() const noexcept;

  private:
    /// Starts a search of @p processes on the worker; @p start carries the query and anything found before.
    void
    startSearch(std::vector<Domain::HandleSearch::SearchProcess> processes, bool authorized, Domain::HandleSearch::SearchPublication start);
    /// The query row: the box, Find and Stop. True when a search should start.
    [[nodiscard]] bool renderQueryRow();
    [[nodiscard]] std::optional<FindHandlesAction> renderResults(const Domain::HandleSearch::SearchPublication& publication);

    std::unique_ptr<Platform::IProcessOpenFilesReader> m_OpenFiles;
    std::unique_ptr<Platform::IProcessModulesReader> m_Modules;
    Capabilities m_Capabilities;
    std::shared_ptr<Domain::PublicationSlot<Domain::HandleSearch::SearchPublication>> m_Slot =
        std::make_shared<Domain::PublicationSlot<Domain::HandleSearch::SearchPublication>>();
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
    std::jthread m_Worker; // Last: stopped and joined first, before the readers it uses go
};

} // namespace App
