#pragma once

// "Create dump file..." (#807), from Process Details' Actions card and the Processes table's row menu:
// the confirm (the shared ProcessActionConfirm dialog), the dump itself on a worker thread of its own,
// and one dialog that shows its progress -- Cancel stops it -- and then its result, with Open folder on
// success, as Task Manager's does. Never on the UI thread: a full dump of a large process takes
// seconds to minutes.
//
// The IProcessActions stays owned by the panel (the composition root's) and is passed in when the
// dump starts. The worker uses it until it finishes, so the panel declares this view after its
// actions (it is destroyed first) and the destructor cancels a dump in flight and waits for it.
//
// Everything but render() is free of ImGui (tests/App/test_ProcessDumpView.cpp).

#include "Platform/IProcessActions.h"
#include "UI/IconsFontAwesome6.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace App
{

namespace DumpDetail
{

/// The Actions card's button and the row menu's item ("###" keeps the menu item's ID fixed).
inline constexpr const char* CREATE_DUMP_LABEL = ICON_FA_FLOPPY_DISK " Create dump file...";
inline constexpr const char* CREATE_DUMP_TOOLTIP = "Write a full memory dump of this process to a file, for a debugger such as WinDbg";
/// The confirm dialog's button.
inline constexpr const char* CONFIRM_LABEL = "Create dump file";

/// Whether to offer "Create dump file..." where the row menu acts on @p batchCount selected processes
/// (0: on the row it was opened on alone; Process Details passes 0): where the platform can write
/// one, and for one process only -- each dump is large and slow, so they are asked for one at a time.
[[nodiscard]] constexpr bool offersCreateDump(const Platform::ProcessActionCapabilities& capabilities, std::size_t batchCount) noexcept
{
    return capabilities.canCreateDump && batchCount == 0;
}

/// The folder dumps go to: the user's temp folder (%TEMP% on Windows), as Task Manager's do. Empty
/// when there is none; the platform then refuses the dump and says so.
[[nodiscard]] std::filesystem::path defaultDumpDirectory();

/// "Create a dump file of notepad.exe (PID 1234)?"
[[nodiscard]] std::string confirmTitle(std::string_view processName, std::int32_t pid);
/// What the dump does and where it goes, for the confirm dialog.
[[nodiscard]] std::string confirmQuestion(std::string_view processName, std::int32_t pid, const std::filesystem::path& directory);
/// "Writing a dump file of notepad.exe (PID 1234)... 12 s", while it runs.
[[nodiscard]] std::string progressText(std::string_view processName, std::int32_t pid, std::chrono::seconds elapsed);

/// What the result dialog shows.
struct DumpMessage
{
    bool ok = false;
    std::string title;          ///< The dialog's title: "Dump file created" or "Couldn't create a dump file".
    std::string text;           ///< What happened.
    std::filesystem::path file; ///< The dump written; empty on failure.
    std::string note;           ///< The platform's note on the dump (a 32-bit process's); may be empty.
};

/// The result dialog's content for a dump of @p processName (PID @p pid) that ended in @p result.
[[nodiscard]] DumpMessage resultMessage(std::string_view processName, std::int32_t pid, const Platform::ProcessDumpResult& result);

} // namespace DumpDetail

/// The dump's state between frames, used on the UI thread only: the worker gets copies of the
/// target and folder and a stop token (whose shared state is thread-safe), and hands back only its
/// plain result, through the future.
class ProcessDumpView
{
  public:
    ProcessDumpView() = default;
    /// Cancels a dump in flight and waits for it (shutdown()).
    ~ProcessDumpView();

    ProcessDumpView(const ProcessDumpView&) = delete;
    ProcessDumpView& operator=(const ProcessDumpView&) = delete;
    /// Movable, so the panels that own one stay movable: a dump in flight moves with it (its worker
    /// shares only the future and the stop token's state). Assigning over one shuts it down first.
    ProcessDumpView(ProcessDumpView&&) noexcept = default;
    ProcessDumpView& operator=(ProcessDumpView&& other) noexcept;

    /// "Create dump file..." was chosen for @p target, named @p processName: asks to confirm writing it
    /// into @p directory. Ignored while a confirm, a dump or its result is up (active()).
    void request(const Platform::ProcessTarget& target, std::string processName, std::filesystem::path directory);

    /// The confirm was accepted: starts the dump on a worker, through @p actions (null: the result says
    /// actions are unavailable, and nothing starts). @p actions must outlive the dump: see the file
    /// comment. Does nothing unless a confirm is pending.
    void confirm(Platform::IProcessActions* actions);

    /// The confirm was cancelled or dismissed: nothing starts.
    void cancelConfirm() noexcept;

    /// Asks the dump in flight to stop: it deletes its partial file and reports itself cancelled.
    void cancel() noexcept;

    /// Takes in the finished dump's result -- at once, or with @p wait after waiting for it. True on
    /// the call that took one in.
    bool takeFinished(bool wait = false);

    /// The result dialog was closed.
    void dismissResult() noexcept
    {
        m_Result.reset();
    }

    /// Cancels a dump in flight and waits for it to end; for the destructor, and a panel's detach.
    void shutdown() noexcept;

    /// A different process was selected: a confirm still up is dropped (the dialog closes at the next
    /// render()). A dump already running is not: it is of the process it was confirmed for.
    void onSelectionChanged() noexcept;

    /// Whether a confirm is requested or open.
    [[nodiscard]] bool confirmPending() const noexcept
    {
        return m_ShowConfirm || m_ConfirmPending;
    }
    /// Whether a dump is running.
    [[nodiscard]] bool busy() const noexcept
    {
        return m_Worker.valid();
    }
    /// Whether any of its dialogs is up: the confirm, the progress or the result. F9 waits until none is.
    [[nodiscard]] bool active() const noexcept
    {
        return confirmPending() || busy() || m_Result.has_value();
    }

    /// The process the pending confirm or the running dump is of (PID -1 when neither).
    [[nodiscard]] const Platform::ProcessTarget& target() const noexcept
    {
        return m_Target;
    }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept
    {
        return m_Directory;
    }
    [[nodiscard]] const std::optional<DumpDetail::DumpMessage>& result() const noexcept
    {
        return m_Result;
    }

    /// Draws the confirm while it is requested or open, and the progress-then-result dialog while a
    /// dump runs or its result is up; takes in a finished dump. Call it every frame, outside any table.
    /// A confirm still up for another process than @p liveTarget is closed unconfirmed (null: the
    /// caller has no selection to compare, as the Processes table's row menu).
    void render(Platform::IProcessActions* actions, const Platform::ProcessTarget* liveTarget = nullptr);

  private:
    void renderProgressDialog();

    bool m_ShowConfirm = false;
    bool m_DismissConfirm = false; // the confirm must close unconfirmed at the next render()
    bool m_ConfirmPending = false; // a confirm was requested and not yet answered
    Platform::ProcessTarget m_Target{.pid = -1, .startTimeTicks = 0};
    std::string m_ProcessName;
    std::filesystem::path m_Directory;
    std::string m_ConfirmTitle;
    std::string m_ConfirmQuestion;

    std::future<Platform::ProcessDumpResult> m_Worker;
    std::stop_source m_Stop; ///< The running dump's; a new one per dump.
    std::chrono::steady_clock::time_point m_StartedAt;
    bool m_CancelRequested = false;
    std::optional<DumpDetail::DumpMessage> m_Result;
};

} // namespace App
