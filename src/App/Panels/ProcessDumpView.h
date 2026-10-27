#pragma once

// "Create dump file..." (#807), from Process Details' Actions card and the Processes table's row menu:
// the confirm (the shared ProcessActionConfirm dialog), the dump itself on a worker thread of its own,
// and one dialog that shows its progress -- Cancel stops it -- and then its result, with Open folder on
// success, as Task Manager's does. Never on the UI thread: a full dump of a large process takes
// seconds to minutes.
//
// The confirm names the folder and warns when its drive may be too full for the dump (#2001). Its
// "Save dump to..." button opens the platform's folder dialog on a worker thread of its own (it blocks
// until the user answers), modal to the main window; the folder chosen is used for this dump and
// remembered for the next ones (UserSettings::dumpFolder).
//
// The folder and its drive are read on a worker too (#3155): whether the remembered folder still
// exists, the drive's free space and the process's private bytes, each time the confirm opens or its
// folder changes. A remembered folder on an unplugged drive or an offline share can take seconds to
// answer; until it has, the confirm says it is checking and its buttons wait, Cancel aside. The dump
// result's "Open folder" is opened on a worker as well (BackgroundOpen).
//
// The IProcessActions stays owned by the panel (the composition root's) and is passed in when the
// dump starts. The worker uses it until it finishes, so the panel declares this view after its
// actions (it is destroyed first) and the destructor cancels a dump in flight and waits for it.
//
// Everything but render() is free of ImGui (tests/App/test_ProcessDumpView.cpp).

#include "App/BackgroundOpen.h"
#include "App/Panels/BackgroundTask.h"
#include "Platform/IProcessActions.h"
#include "Platform/ThreadName.h"
#include "UI/IconsFontAwesome6.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

namespace DumpDetail
{

/// The Actions card's button and the row menu's item ("###" keeps the menu item's ID fixed).
inline constexpr const char* CREATE_DUMP_LABEL = ICON_FA_FLOPPY_DISK " Create dump file...";
inline constexpr const char* CREATE_DUMP_TOOLTIP =
    "Write a full memory dump of this process to a file, for a debugger such as WinDbg or gdb";
/// The confirm dialog's button.
inline constexpr const char* CONFIRM_LABEL = "Create dump file";
/// The confirm dialog's folder button (#2001), left of Cancel.
inline constexpr const char* CHOOSE_FOLDER_LABEL = ICON_FA_FOLDER_OPEN " Save dump to...";
inline constexpr const char* CHOOSE_FOLDER_TOOLTIP = "Choose the folder the dump file is written to; it is used for later dumps too";

/// What a full dump is reckoned to need beyond the process's private bytes (#2001): a quarter more,
/// for the images, mapped files and shared memory a full dump holds too, plus a fixed margin for
/// small processes, whose dumps are mostly those images.
inline constexpr std::uint64_t DUMP_SIZE_MARGIN_BYTES = std::uint64_t{128} * 1024U * 1024U;
inline constexpr std::uint64_t DUMP_SIZE_OVERHEAD_DIVISOR = 4; ///< A quarter more than the private bytes

/// Whether to offer "Create dump file..." where the row menu acts on @p batchCount selected processes
/// (0: on the row it was opened on alone; Process Details passes 0): where the platform can write
/// one, and for one process only -- each dump is large and slow, so they are asked for one at a time.
[[nodiscard]] constexpr bool offersCreateDump(const Platform::ProcessActionCapabilities& capabilities, std::size_t batchCount) noexcept
{
    return capabilities.canCreateDump && batchCount == 0;
}

/// The size a full dump of a process with @p processBytes private bytes is reckoned to need: those,
/// a quarter more, and DUMP_SIZE_MARGIN_BYTES (saturating, never wrapping).
[[nodiscard]] constexpr std::uint64_t estimatedDumpBytes(std::uint64_t processBytes) noexcept
{
    constexpr std::uint64_t MAX = ~std::uint64_t{0};
    const std::uint64_t overhead = (processBytes / DUMP_SIZE_OVERHEAD_DIVISOR) + DUMP_SIZE_MARGIN_BYTES;
    return (processBytes > MAX - overhead) ? MAX : processBytes + overhead;
}

/// Whether the dump may not fit: the volume's free bytes are known and fewer than the estimate made
/// from the private bytes. Unknown either way: no warning, rather than a false one.
[[nodiscard]] constexpr bool dumpMayNotFit(const Platform::DumpSpace& space) noexcept
{
    return space.freeBytes.has_value() && space.processBytes.has_value() && *space.freeBytes < estimatedDumpBytes(*space.processBytes);
}

/// The confirm's warning when the dump may not fit in @p directory (dumpMayNotFit()); empty when it
/// should, or when that can't be told. It points to "Save dump to..." only where @p canChooseFolder
/// (ProcessActionCapabilities::canChooseDumpFolder; not on Linux, #807).
[[nodiscard]] std::string
lowSpaceWarning(const Platform::DumpSpace& space, const std::filesystem::path& directory, bool canChooseFolder = true);

/// The folder dumps go to by default: the user's temp folder (%TEMP% on Windows), as Task Manager's
/// do. Empty when there is none; the platform then refuses the dump and says so.
[[nodiscard]] std::filesystem::path defaultDumpDirectory();

/// The folder dumps go to: @p remembered (UserSettings::dumpFolder, UTF-8) while it is an existing
/// folder, else defaultDumpDirectory(). Asks the file system: on the confirm's worker, never the UI thread.
[[nodiscard]] std::filesystem::path dumpDirectory(std::string_view remembered);
/// The folder the user last chose (UserSettings::dumpFolder, UTF-8), from the user's config: read on
/// the UI thread, checked by dumpDirectory() on the confirm's worker (#3155).
[[nodiscard]] std::string rememberedDumpFolder();
/// Keeps @p directory as the folder later dumps go to, in the user's config, and saves it.
void rememberDumpDirectory(const std::filesystem::path& directory);

/// What the confirm says when SDL's folder dialog couldn't be shown, from SDL's @p error: in the user's
/// terms where SDL found no dialog to use at all (no desktop portal and no zenity), else SDL's own.
[[nodiscard]] std::string folderDialogFailure(std::string_view error);

/// The folder dialog used where the platform layer has none (Linux, #807): SDL's
/// (Core::showFolderDialog(), the desktop portal's chooser), modal to the main window, starting in
/// @p initial. @p answer gets the folder chosen, nullopt when cancelled, or why the dialog couldn't be
/// shown -- at once when there is no window, else later, maybe on another thread.
void sdlFolderDialog(const std::filesystem::path& initial, std::function<void(Platform::FolderChoice)> answer);

/// "Create a dump file of notepad.exe (PID 1234)?"
[[nodiscard]] std::string confirmTitle(std::string_view processName, std::int32_t pid);
/// What the dump does and where it goes, for the confirm dialog; then @p warning (lowSpaceWarning(),
/// or why the folder dialog failed) when there is one. @p dumpsFromSnapshot
/// (ProcessActionCapabilities::dumpsFromSnapshot) says how long the process pauses: while a snapshot
/// is taken (Windows), or for the whole write (Linux's gcore, #2241).
[[nodiscard]] std::string confirmQuestion(std::string_view processName,
                                          std::int32_t pid,
                                          const std::filesystem::path& directory,
                                          bool dumpsFromSnapshot,
                                          std::string_view warning = {});
/// "Writing a dump file of notepad.exe (PID 1234)... 12 s", while it runs.
[[nodiscard]] std::string progressText(std::string_view processName, std::int32_t pid, std::chrono::seconds elapsed);

/// What the confirm says, in place of a space warning, while its folder and drive are read (#3155).
inline constexpr std::string_view CHECKING_FOLDER_TEXT = "Checking the folder and the free space on its drive...";

/// What the confirm's worker read for its folder (#3155): the folder (the remembered one, or the
/// default where it is gone), the drive's free space and the process's private bytes.
struct ConfirmFacts
{
    std::filesystem::path directory;
    Platform::DumpSpace space;
};
/// The confirm worker's answer: its facts, or why it couldn't read them (it threw, or had no thread).
using ConfirmCheck = std::expected<ConfirmFacts, std::string>;

/// What the result dialog shows.
struct DumpMessage
{
    bool ok = false;
    std::string title;          ///< The dialog's title: "Dump file created" or "Couldn't create a dump file".
    std::string text;           ///< What happened.
    std::filesystem::path file; ///< The dump written; empty on failure.
    std::string note;           ///< The platform's note on the dump (a 32-bit process's); may be empty.
    std::string method;         ///< How it was written: from a snapshot, or live and why (#2001); empty on failure.
};

/// The result dialog's content for a dump of @p processName (PID @p pid) that ended in @p result.
/// Where the platform never takes a snapshot (@p dumpsFromSnapshot false: Linux), a dump of the running
/// process is the normal case and isn't worded as a fallback (#2241).
[[nodiscard]] DumpMessage
resultMessage(std::string_view processName, std::int32_t pid, const Platform::ProcessDumpResult& result, bool dumpsFromSnapshot);

/// The failure a dump or folder dialog that threw, or couldn't start, comes back as (BackgroundTask).
template<typename Result> [[nodiscard]] Result failedWith(std::string detail)
{
    return std::unexpected(std::move(detail));
}

} // namespace DumpDetail

/// The dump's state between frames, used on the UI thread only: the workers get copies of the
/// target and folder and a stop token (whose shared state is thread-safe), and hand back only their
/// plain results, through futures.
class ProcessDumpView
{
  public:
    /// Called on the UI thread with each folder the user chooses, to remember it.
    using FolderChosenHandler = std::function<void(const std::filesystem::path&)>;
    /// The answer to a folder dialog: called once, maybe on another thread.
    using FolderAnswer = std::function<void(Platform::FolderChoice)>;
    /// A folder dialog that answers later, through its FolderAnswer: the one used where the platform
    /// layer has none of its own (Linux: SDL's, which shows the desktop portal's chooser, #807). Called on
    /// the UI thread with the folder to start in.
    using FolderDialog = std::function<void(const std::filesystem::path& initial, FolderAnswer answer)>;

    /// Remembers a chosen folder in the user's config (DumpDetail::rememberDumpDirectory()), and, where
    /// the platform has no folder dialog, uses SDL's over the main window (DumpDetail::sdlFolderDialog()).
    ProcessDumpView();
    /// Remembers a chosen folder through @p onFolderChosen instead (tests: no config written); empty:
    /// not at all. No fallback folder dialog: tests set one with setFolderDialog().
    explicit ProcessDumpView(FolderChosenHandler onFolderChosen);

    /// The folder dialog used where the platform has none (empty: none, so no "Save dump to...").
    void setFolderDialog(FolderDialog dialog)
    {
        m_FolderDialog = std::move(dialog);
    }

    /// Whether "Save dump to..." is offered with @p actions: the platform's folder dialog, or the fallback.
    [[nodiscard]] bool canChooseFolder(Platform::IProcessActions* actions) const;
    /// Cancels a dump in flight and waits for it, and for a folder dialog still up (shutdown()).
    ~ProcessDumpView();

    ProcessDumpView(const ProcessDumpView&) = delete;
    ProcessDumpView& operator=(const ProcessDumpView&) = delete;
    /// Movable, so the panels that own one stay movable: a dump in flight moves with it (its worker
    /// shares only the future and the stop token's state). Assigning over one shuts it down first.
    ProcessDumpView(ProcessDumpView&&) noexcept = default;
    ProcessDumpView& operator=(ProcessDumpView&& other) noexcept;

    /// "Create dump file..." was chosen for @p target, named @p processName: asks to confirm writing it
    /// into @p directory, warning when its drive may be too full -- read through @p actions'
    /// dumpSpace() on a worker (#3155), taken in by takeConfirmCheck() (null: no check), and saying how
    /// long the process pauses (ProcessActionCapabilities::dumpsFromSnapshot; null: for the whole
    /// write). Ignored while a confirm, a dump or its result is up (active()).
    void request(const Platform::ProcessTarget& target,
                 std::string processName,
                 std::filesystem::path directory,
                 Platform::IProcessActions* actions = nullptr);

    /// request() into the folder the user last chose, @p remembered (UserSettings::dumpFolder, UTF-8;
    /// empty: none): the worker checks that it still exists, and uses the default folder where it
    /// doesn't (DumpDetail::dumpDirectory()), before reading its drive's space. The Actions card's
    /// button and the row menu's item.
    void requestInRememberedFolder(const Platform::ProcessTarget& target,
                                   std::string processName,
                                   std::string_view remembered,
                                   Platform::IProcessActions* actions);

    /// Takes in what the confirm's worker read for its folder (#3155) -- at once, or with @p wait after
    /// waiting for it: the folder, and the space warning in the confirm's question. True on the call
    /// that took one in for the confirm still up. A check for a confirm since closed, or for a folder
    /// since changed, is dropped (and the current folder checked again).
    bool takeConfirmCheck(Platform::IProcessActions* actions, bool wait = false);

    /// "Save dump to..." was pressed in the confirm: opens @p actions' folder dialog on a worker,
    /// starting in the confirm's folder and modal to @p ownerWindow (a native window, or null); where the
    /// platform has none, the fallback FolderDialog, which answers asynchronously. Does nothing unless a
    /// confirm is pending, no folder dialog is up already, its folder isn't being checked
    /// (checkingFolder()), and there is one (canChooseFolder()).
    void chooseFolder(Platform::IProcessActions* actions, void* ownerWindow);

    /// Takes in the folder dialog's answer -- at once, or with @p wait after waiting for it. True on
    /// the call that took one in. A folder chosen becomes the confirm's (its space checked again
    /// through @p actions, on the confirm's worker) and is remembered; a cancelled dialog changes
    /// nothing; a failed one says why in the confirm.
    bool takeFolderChoice(Platform::IProcessActions* actions, bool wait = false);

    /// The confirm was accepted: starts the dump on a worker, through @p actions (null: the result says
    /// actions are unavailable, and nothing starts). @p actions must outlive the dump: see the file
    /// comment. Does nothing unless a confirm is pending, or while the folder dialog is up or its
    /// folder is being checked.
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

    /// Cancels a dump in flight and waits for it to end, for a folder dialog still up to be answered,
    /// and for the confirm's folder check and an "Open folder" still running; for the destructor, and
    /// a panel's detach.
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
        return m_Worker.busy();
    }
    /// Whether the folder dialog is up.
    [[nodiscard]] bool choosingFolder() const noexcept
    {
        return m_FolderPick.busy();
    }
    /// Whether the confirm's folder and drive are being read on its worker, or are still to be (#3155).
    /// A check of a confirm since closed, or of a folder since changed, doesn't count: it is dropped.
    [[nodiscard]] bool checkingFolder() const noexcept
    {
        return m_CheckWanted || (m_ConfirmCheck.busy() && m_RunningCheckGeneration == m_CheckGeneration);
    }
    /// Whether the dump result's "Open folder" is still being opened on its worker (#3155).
    [[nodiscard]] bool openingFolder() const noexcept
    {
        return m_OpenFolder.busy();
    }
    /// Whether any of its dialogs is up: the confirm, the folder dialog, the progress or the result.
    /// F9 waits until none is.
    [[nodiscard]] bool active() const noexcept
    {
        return confirmPending() || choosingFolder() || busy() || m_Result.has_value();
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
    /// The confirm's question as it shows now: the folder, and any warning.
    [[nodiscard]] const std::string& confirmQuestion() const noexcept
    {
        return m_ConfirmQuestion;
    }
    [[nodiscard]] const std::optional<DumpDetail::DumpMessage>& result() const noexcept
    {
        return m_Result;
    }

    /// Draws the confirm while it is requested or open, and the progress-then-result dialog while a
    /// dump runs or its result is up; takes in a finished dump and a folder dialog's answer. Call it
    /// every frame, outside any table. A confirm still up for another process than @p liveTarget is
    /// closed unconfirmed (null: the caller has no selection to compare, as the Processes table's row
    /// menu).
    void render(Platform::IProcessActions* actions, const Platform::ProcessTarget* liveTarget = nullptr);

  private:
    void renderProgressDialog();
    /// Opens the confirm for @p target, in @p directory; the caller asks for its check.
    void openConfirm(const Platform::ProcessTarget& target,
                     std::string processName,
                     std::filesystem::path directory,
                     Platform::IProcessActions* actions);
    /// Takes in @p check, the confirm worker's answer: its folder and space warning, or why it failed.
    void applyCheck(DumpDetail::ConfirmCheck check, Platform::IProcessActions* actions);
    /// Has the confirm's folder read on its worker (#3155): resolved from @p remembered first where
    /// @p resolve, and its drive's space read through @p actions (null: not read). A check already
    /// running is left to finish, dropped, and this one started after it.
    void checkFolder(Platform::IProcessActions* actions, bool resolve, std::string remembered = {});
    /// Starts the check checkFolder() asked for, once none is running.
    void startCheckIfDue();
    /// Rebuilds the confirm's question from its folder, the space warning and the folder dialog's
    /// failure, or says the folder is being checked. No platform call.
    void rebuildQuestion();

    // Member order is free of lifetime rules: the destructor's shutdown() waits for all four workers
    // (dump, folder dialog, folder check, Open folder) before any member goes, and none of their jobs
    // touches a member (each owns copies; the actions are the panel's). The flags are last, together,
    // so they pack (clang-analyzer-optin.performance.Padding).
    FolderChosenHandler m_OnFolderChosen;

    Platform::ProcessTarget m_Target{.pid = -1, .startTimeTicks = 0};
    std::string m_ProcessName;
    std::filesystem::path m_Directory;
    std::string m_ConfirmTitle;
    std::string m_ConfirmQuestion;
    std::string m_SpaceWarning;  // lowSpaceWarning() for m_Directory; empty when none
    std::string m_FolderFailure; // why the folder dialog couldn't be shown; empty when it could

    // The confirm's folder check (#3155): on a worker, one at a time. A check asked for while one runs
    // waits in m_CheckWanted (and its inputs) until that one is taken in, and drops it.
    BackgroundTask<DumpDetail::ConfirmCheck> m_ConfirmCheck{Platform::DUMP_CHECK_THREAD_NAME,
                                                            &DumpDetail::failedWith<DumpDetail::ConfirmCheck>};
    std::string m_CheckRemembered;                       // UserSettings::dumpFolder, for a check that resolves it
    Platform::IProcessActions* m_CheckActions = nullptr; // the actions it reads the space through; may be null
    std::uint64_t m_CheckGeneration = 0;                 // bumped by each checkFolder() and each confirm opened or closed
    std::uint64_t m_RunningCheckGeneration = 0;          // the generation of the check running

    // The folder dialog's answer: its worker's, or adopted from the fallback (answered from SDL's event
    // pump, not a worker).
    BackgroundTask<Platform::FolderChoice> m_FolderPick{Platform::FOLDER_DIALOG_THREAD_NAME,
                                                        &DumpDetail::failedWith<Platform::FolderChoice>};
    FolderDialog m_FolderDialog; // used where the platform has no folder dialog; empty: none

    /// The running dump; a new stop token per dump.
    BackgroundTask<Platform::ProcessDumpResult> m_Worker{Platform::DUMP_THREAD_NAME, &DumpDetail::failedWith<Platform::ProcessDumpResult>};
    std::chrono::steady_clock::time_point m_StartedAt;
    std::optional<DumpDetail::DumpMessage> m_Result;
    // The result's "Open folder", on a worker (#3155): ShellExecuteW or xdg-open.
    BackgroundOpen m_OpenFolder;

    bool m_ShowConfirm = false;
    bool m_DismissConfirm = false;    // the confirm must close unconfirmed at the next render()
    bool m_ConfirmPending = false;    // a confirm was requested and not yet answered
    bool m_DumpsFromSnapshot = false; // ProcessActionCapabilities::dumpsFromSnapshot of the actions given
    bool m_CheckWanted = false;       // a folder check was asked for and not started yet
    bool m_CheckResolve = false;      // it resolves m_CheckRemembered first
    bool m_CancelRequested = false;   // Cancel was pressed while the dump ran
};

} // namespace App
