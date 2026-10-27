/// @file test_ProcessDumpView.cpp
/// @brief "Create dump file..." (#807) without an ImGui context: which menus offer it, the confirm, the
/// dump on its worker through a mock IProcessActions, Cancel, and the result dialog's messages. The
/// dialogs themselves are driven headless in test_ProcessActionsBlockRender.cpp.

#include "App/Panels/ProcessDumpView.h"
#include "App/UserConfig.h"
#include "Core/Utf8Path.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#ifdef __linux__
#include "Platform/Linux/LinuxCoreDump.h"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <future>
#include <optional>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr Platform::ProcessTarget TARGET{.pid = 1001, .startTimeTicks = 5000};

[[nodiscard]] Platform::ProcessActionCapabilities dumpCapable(bool canCreateDump)
{
    Platform::ProcessActionCapabilities caps;
    caps.canKill = true;
    caps.canCreateDump = canCreateDump;
    return caps;
}

[[nodiscard]] std::filesystem::path someFolder()
{
    return std::filesystem::path("dumps") / "here";
}

// --- Which menus offer it -------------------------------------------------------------------------

TEST(ProcessDumpViewTest, OfferedForOneProcessWhereThePlatformCanWriteADump)
{
    EXPECT_TRUE(DumpDetail::offersCreateDump(dumpCapable(true), 0));
    // Linux until its half lands, and the synthetic scenario: not offered at all.
    EXPECT_FALSE(DumpDetail::offersCreateDump(dumpCapable(false), 0));
    EXPECT_FALSE(DumpDetail::offersCreateDump(Platform::ProcessActionCapabilities{}, 0));
    // Never for a multi-selection's batch: one dump at a time.
    EXPECT_FALSE(DumpDetail::offersCreateDump(dumpCapable(true), 2));
    EXPECT_FALSE(DumpDetail::offersCreateDump(dumpCapable(true), 5));
}

TEST(ProcessDumpViewTest, TheDefaultInterfaceRefusesADumpAsUnsupported)
{
    // An IProcessActions that does not override createDump() (Linux for now) says so, and does not
    // claim the capability.
    struct Plain : TestMocks::MockProcessActions
    {
        [[nodiscard]] Platform::ProcessDumpResult
        createDump(const Platform::ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken) override
        {
            return Platform::IProcessActions::createDump(target, directory, stopToken); // NOLINT(bugprone-parent-virtual-call)
        }
    };
    Plain plain;
    const Platform::ProcessDumpResult result = plain.createDump(TARGET, someFolder(), std::stop_token{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Platform::DUMP_UNSUPPORTED_MESSAGE);
    EXPECT_FALSE(Platform::ProcessActionCapabilities{}.canCreateDump);
}

TEST(ProcessDumpViewTest, TheDefaultFolderIsTheTempFolder)
{
    EXPECT_EQ(DumpDetail::defaultDumpDirectory(), std::filesystem::temp_directory_path());
}

// --- Confirm --------------------------------------------------------------------------------------

TEST(ProcessDumpViewTest, ARequestAsksToConfirmAndNamesTheProcessAndFolder)
{
    ProcessDumpView view;
    view.request(TARGET, "notepad.exe", someFolder());
    EXPECT_TRUE(view.confirmPending());
    EXPECT_TRUE(view.active());
    EXPECT_FALSE(view.busy());
    EXPECT_EQ(view.target(), TARGET);
    EXPECT_EQ(view.directory(), someFolder());

    EXPECT_EQ(DumpDetail::confirmTitle("notepad.exe", 1001), "Create a dump file of notepad.exe (PID 1001)?");
    const std::string question = DumpDetail::confirmQuestion("notepad.exe", 1001, someFolder(), true);
    EXPECT_TRUE(question.contains("notepad.exe (PID 1001)")) << question;
    EXPECT_TRUE(question.contains("full memory dump")) << question;
    EXPECT_TRUE(question.contains(someFolder().string())) << question;
}

TEST(ProcessDumpViewTest, CancellingTheConfirmWritesNothing)
{
    TestMocks::MockProcessActions mock;
    ProcessDumpView view;
    view.request(TARGET, "notepad.exe", someFolder());
    view.cancelConfirm();
    EXPECT_FALSE(view.active());
    view.confirm(&mock); // nothing pending any more
    EXPECT_FALSE(view.busy());
    EXPECT_EQ(mock.createDumpCount(), 0);
}

TEST(ProcessDumpViewTest, ASelectionChangeDropsTheConfirm)
{
    TestMocks::MockProcessActions mock;
    ProcessDumpView view;
    view.request(TARGET, "notepad.exe", someFolder());
    view.onSelectionChanged();
    EXPECT_FALSE(view.confirmPending());
    view.confirm(&mock);
    EXPECT_EQ(mock.createDumpCount(), 0);
}

TEST(ProcessDumpViewTest, NoProcessNoRequest)
{
    ProcessDumpView view;
    view.request({.pid = 0, .startTimeTicks = 0}, "", someFolder());
    EXPECT_FALSE(view.active());
}

// --- The dump -------------------------------------------------------------------------------------

TEST(ProcessDumpViewTest, AConfirmedDumpRunsOnTheWorkerAndSaysWhereItsFileIs)
{
    TestMocks::MockProcessActions mock;
    const std::filesystem::path file = someFolder() / "notepad_1001_2026-10-13_14-05-09.dmp";
    mock.setDumpResult(Platform::ProcessDump{
        .file = file, .sizeBytes = std::uint64_t{3} * 1024U * 1024U, .note = {}, .fromSnapshot = true, .liveReason = {}});
    ProcessDumpView view;
    view.request(TARGET, "notepad.exe", someFolder());
    view.confirm(&mock);
    EXPECT_FALSE(view.confirmPending());
    EXPECT_TRUE(view.busy());
    EXPECT_TRUE(view.active()); // F9 waits

    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_FALSE(view.busy());
    EXPECT_EQ(mock.createDumpCount(), 1);
    EXPECT_EQ(mock.lastTarget(), TARGET); // the process confirmed, start time and all
    EXPECT_EQ(mock.lastDumpDirectory(), someFolder());

    ASSERT_TRUE(view.result().has_value());
    const DumpDetail::DumpMessage message = view.result().value_or(DumpDetail::DumpMessage{});
    EXPECT_TRUE(message.ok);
    EXPECT_EQ(message.title, "Dump file created");
    EXPECT_TRUE(message.text.contains("notepad.exe (PID 1001)")) << message.text;
    EXPECT_TRUE(message.text.contains("MiB")) << message.text;
    EXPECT_EQ(message.file, file);
    EXPECT_TRUE(view.active()); // until the result is closed
    view.dismissResult();
    EXPECT_FALSE(view.active());
}

TEST(ProcessDumpViewTest, AFailedDumpSaysWhy)
{
    TestMocks::MockProcessActions mock;
    mock.setDumpResult(std::unexpected(std::string("Access denied: can't read the memory of process 1001.")));
    ProcessDumpView view;
    view.request(TARGET, "lsass.exe", someFolder());
    view.confirm(&mock);
    ASSERT_TRUE(view.takeFinished(true));
    ASSERT_TRUE(view.result().has_value());
    const DumpDetail::DumpMessage message = view.result().value_or(DumpDetail::DumpMessage{});
    EXPECT_FALSE(message.ok);
    EXPECT_EQ(message.title, "Couldn't create a dump file");
    EXPECT_EQ(message.text, "Could not create a dump file of lsass.exe (PID 1001): Access denied: can't read the memory of process 1001.");
    EXPECT_TRUE(message.file.empty());
}

TEST(ProcessDumpViewTest, ThePlatformsNoteIsKept)
{
    const DumpDetail::DumpMessage message = DumpDetail::resultMessage(
        "app32.exe",
        77,
        Platform::ProcessDump{.file = "x.dmp", .sizeBytes = 1, .note = "This is a 32-bit process", .fromSnapshot = true, .liveReason = {}},
        true);
    EXPECT_TRUE(message.ok);
    EXPECT_EQ(message.note, "This is a 32-bit process");
}

TEST(ProcessDumpViewTest, WithoutActionsTheResultSaysTheyAreUnavailable)
{
    ProcessDumpView view;
    view.request(TARGET, "notepad.exe", someFolder());
    view.confirm(nullptr);
    EXPECT_FALSE(view.busy());
    ASSERT_TRUE(view.result().has_value());
    const DumpDetail::DumpMessage message =
        view.result().value_or(DumpDetail::DumpMessage{.ok = true, .title = {}, .text = {}, .file = {}, .note = {}, .method = {}});
    EXPECT_FALSE(message.ok);
    EXPECT_TRUE(message.text.contains("Process actions unavailable")) << message.text;
}

TEST(ProcessDumpViewTest, CancelStopsTheDumpAndLeavesNothingToAcknowledge)
{
    TestMocks::MockProcessActions mock;
    mock.setDumpWaitsForCancel(true);
    ProcessDumpView view;
    view.request(TARGET, "big.exe", someFolder());
    view.confirm(&mock);
    ASSERT_TRUE(view.busy());
    EXPECT_FALSE(view.takeFinished()); // still writing
    view.cancel();
    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_FALSE(view.busy());
    EXPECT_FALSE(view.result().has_value());
    EXPECT_FALSE(view.active());
}

TEST(ProcessDumpViewTest, AnotherRequestWhileOneIsUpIsIgnored)
{
    TestMocks::MockProcessActions mock;
    mock.setDumpWaitsForCancel(true);
    ProcessDumpView view;
    view.request(TARGET, "big.exe", someFolder());
    view.confirm(&mock);
    view.request({.pid = 2002, .startTimeTicks = 6000}, "other.exe", someFolder());
    EXPECT_FALSE(view.confirmPending());
    EXPECT_EQ(view.target(), TARGET);
    // Destroying the view cancels the dump and waits for it.
}

TEST(ProcessDumpViewTest, ShutdownCancelsAndWaits)
{
    TestMocks::MockProcessActions mock;
    mock.setDumpWaitsForCancel(true);
    ProcessDumpView view;
    view.request(TARGET, "big.exe", someFolder());
    view.confirm(&mock);
    view.shutdown();
    EXPECT_EQ(mock.createDumpCount(), 1);
    EXPECT_TRUE(view.takeFinished());
    EXPECT_FALSE(view.busy());
}

TEST(ProcessDumpViewTest, ProgressCountsTheSeconds)
{
    EXPECT_EQ(DumpDetail::progressText("big.exe", 1001, std::chrono::seconds(12)), "Writing a dump file of big.exe (PID 1001)... 12 s");
}

// --- Snapshot or live (#2001) ---------------------------------------------------------------------

TEST(ProcessDumpViewTest, ASnapshotDumpSaysSo)
{
    const DumpDetail::DumpMessage message = DumpDetail::resultMessage(
        "notepad.exe",
        1001,
        Platform::ProcessDump{.file = "x.dmp", .sizeBytes = 1, .note = {}, .fromSnapshot = true, .liveReason = {}},
        true);
    EXPECT_TRUE(message.ok);
    EXPECT_TRUE(message.method.contains("from a snapshot")) << message.method;
    EXPECT_TRUE(message.method.contains("paused only while the snapshot was taken")) << message.method;
}

TEST(ProcessDumpViewTest, ALiveDumpSaysWhyThereWasNoSnapshot)
{
    const DumpDetail::DumpMessage message =
        DumpDetail::resultMessage("notepad.exe",
                                  1001,
                                  Platform::ProcessDump{.file = "x.dmp",
                                                        .sizeBytes = 1,
                                                        .note = {},
                                                        .fromSnapshot = false,
                                                        .liveReason = "Windows refused the access a snapshot of it needs"},
                                  true);
    EXPECT_TRUE(message.ok);
    EXPECT_TRUE(message.method.contains("from the running process")) << message.method;
    EXPECT_TRUE(message.method.contains("no snapshot could be taken (Windows refused the access a snapshot of it needs)"))
        << message.method;

    // A failure has no method line.
    const DumpDetail::DumpMessage failed =
        DumpDetail::resultMessage("notepad.exe", 1001, Platform::ProcessDumpResult(std::unexpected(std::string("Access denied"))), true);
    EXPECT_TRUE(failed.method.empty());
}

// --- A platform without snapshots (Linux's gcore, #2241) ------------------------------------------

constexpr const char* SNAPSHOT_PAUSE = "The process pauses while a snapshot of it is taken; where none can be, it may stop responding "
                                       "while the dump is written.";
constexpr const char* WHOLE_WRITE_STOP = "The process is stopped while the dump is written, and doesn't respond until it is done.";

TEST(ProcessDumpViewTest, WithSnapshotsTheConfirmPromisesOnlyASnapshotPause)
{
    // Windows' wording (#2001), kept as it was.
    const std::string question = DumpDetail::confirmQuestion("notepad.exe", 1001, someFolder(), true);
    EXPECT_TRUE(question.ends_with(SNAPSHOT_PAUSE)) << question;
    EXPECT_FALSE(question.contains(WHOLE_WRITE_STOP)) << question;
}

TEST(ProcessDumpViewTest, WithoutSnapshotsTheConfirmSaysTheProcessIsStoppedForTheWholeWrite)
{
    const std::string question = DumpDetail::confirmQuestion("bash", 1001, someFolder(), false, "low space");
    EXPECT_TRUE(question.contains(WHOLE_WRITE_STOP)) << question;
    EXPECT_FALSE(question.contains("snapshot")) << question;
    EXPECT_TRUE(question.contains("bash (PID 1001)")) << question;
    EXPECT_TRUE(question.ends_with("\n\nlow space")) << question;
}

TEST(ProcessDumpViewTest, WithoutSnapshotsALiveDumpIsNotAFallback)
{
    const DumpDetail::DumpMessage message = DumpDetail::resultMessage(
        "bash",
        1001,
        Platform::ProcessDump{.file = "x.core", .sizeBytes = 1, .note = {}, .fromSnapshot = false, .liveReason = "gcore stops it"},
        false);
    EXPECT_TRUE(message.ok);
    EXPECT_EQ(message.method, "Written from the running process, which was stopped while it was written (gcore stops it).");

    const DumpDetail::DumpMessage noReason = DumpDetail::resultMessage(
        "bash", 1001, Platform::ProcessDump{.file = "x.core", .sizeBytes = 1, .note = {}, .fromSnapshot = false, .liveReason = {}}, false);
    EXPECT_EQ(noReason.method, "Written from the running process, which was stopped while it was written.");
}

TEST(ProcessDumpViewTest, TheViewWordsTheConfirmAndResultByThePlatformsCapability)
{
    for (const bool snapshots : {true, false})
    {
        TestMocks::MockProcessActions mock;
        Platform::ProcessActionCapabilities caps = dumpCapable(true);
        caps.dumpsFromSnapshot = snapshots;
        mock.setCapabilities(caps);
        mock.setDumpResult(
            Platform::ProcessDump{.file = "x.dmp", .sizeBytes = 1, .note = {}, .fromSnapshot = false, .liveReason = "the reason"});
        ProcessDumpView view([](const std::filesystem::path&) {});
        view.request(TARGET, "app", someFolder(), &mock);
        ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
        EXPECT_TRUE(view.confirmQuestion().contains(snapshots ? SNAPSHOT_PAUSE : WHOLE_WRITE_STOP)) << view.confirmQuestion();
        view.confirm(&mock);
        ASSERT_TRUE(view.takeFinished(true));
        const DumpDetail::DumpMessage message = view.result().value_or(DumpDetail::DumpMessage{});
        EXPECT_EQ(message.method.contains("no snapshot could be taken (the reason)"), snapshots) << message.method;
        EXPECT_EQ(message.method.contains("was stopped while it was written (the reason)"), !snapshots) << message.method;
    }
}

#ifdef __linux__
TEST(ProcessDumpViewTest, LinuxsGcoreDumpIsWordedAsTheNormalCase)
{
    // What LinuxProcessActions::createDump() returns, as the result dialog shows it.
    const DumpDetail::DumpMessage message = DumpDetail::resultMessage(
        "bash",
        1001,
        Platform::ProcessDump{
            .file = "x.core", .sizeBytes = 1, .note = {}, .fromSnapshot = false, .liveReason = Platform::LinuxCoreDump::LIVE_REASON},
        false);
    EXPECT_FALSE(message.method.contains("snapshot")) << message.method;
    EXPECT_TRUE(message.method.contains("gcore")) << message.method;
}
#endif

// --- The free-space estimate and warning (#2001) --------------------------------------------------

constexpr std::uint64_t MIB = std::uint64_t{1024} * 1024U;
constexpr std::uint64_t GIB = MIB * 1024U;

TEST(ProcessDumpViewTest, TheEstimateIsThePrivateBytesAQuarterMoreAndAMargin)
{
    EXPECT_EQ(DumpDetail::estimatedDumpBytes(0), DumpDetail::DUMP_SIZE_MARGIN_BYTES);
    EXPECT_EQ(DumpDetail::estimatedDumpBytes(4 * GIB), (4 * GIB) + GIB + (128 * MIB));
    // Never wraps past the top of the range.
    constexpr std::uint64_t MAX = ~std::uint64_t{0};
    EXPECT_EQ(DumpDetail::estimatedDumpBytes(MAX), MAX);
    EXPECT_EQ(DumpDetail::estimatedDumpBytes(MAX - 1), MAX);
}

TEST(ProcessDumpViewTest, ItWarnsOnlyWhenTheFreeSpaceIsKnownAndShort)
{
    const std::uint64_t need = DumpDetail::estimatedDumpBytes(2 * GIB);
    EXPECT_TRUE(DumpDetail::dumpMayNotFit({.freeBytes = need - 1, .processBytes = 2 * GIB}));
    EXPECT_FALSE(DumpDetail::dumpMayNotFit({.freeBytes = need, .processBytes = 2 * GIB}));
    EXPECT_FALSE(DumpDetail::dumpMayNotFit({.freeBytes = 100 * GIB, .processBytes = 2 * GIB}));
    // Either reading unknown: no warning, rather than a false one.
    EXPECT_FALSE(DumpDetail::dumpMayNotFit({.freeBytes = std::nullopt, .processBytes = 2 * GIB}));
    EXPECT_FALSE(DumpDetail::dumpMayNotFit({.freeBytes = 0, .processBytes = std::nullopt}));
    EXPECT_FALSE(DumpDetail::dumpMayNotFit({}));
}

TEST(ProcessDumpViewTest, TheWarningNamesTheFolderTheSizesAndWhatToDo)
{
    const std::string warning = DumpDetail::lowSpaceWarning({.freeBytes = GIB, .processBytes = 4 * GIB}, someFolder());
    EXPECT_TRUE(warning.contains("Only 1.0 GiB is free")) << warning;
    EXPECT_TRUE(warning.contains(someFolder().string())) << warning;
    EXPECT_TRUE(warning.contains("about 5.1 GiB")) << warning; // 4 GiB + 1 GiB + 128 MiB
    EXPECT_TRUE(warning.contains("its 4.0 GiB of private memory")) << warning;
    EXPECT_TRUE(warning.contains("may not fit")) << warning;
    EXPECT_TRUE(warning.contains("Save dump to...")) << warning;
    // Where there is no folder picker (Linux, #807) it doesn't point to one.
    const std::string noPicker = DumpDetail::lowSpaceWarning({.freeBytes = GIB, .processBytes = 4 * GIB}, someFolder(), false);
    EXPECT_FALSE(noPicker.contains("Save dump to...")) << noPicker;
    EXPECT_TRUE(noPicker.contains("free some space first")) << noPicker;
    EXPECT_TRUE(DumpDetail::lowSpaceWarning({.freeBytes = 100 * GIB, .processBytes = 4 * GIB}, someFolder()).empty());
    EXPECT_TRUE(DumpDetail::lowSpaceWarning({}, someFolder()).empty());

    const std::string question = DumpDetail::confirmQuestion("big.exe", 1001, someFolder(), true, warning);
    EXPECT_TRUE(question.ends_with("\n\n" + warning)) << question;
    EXPECT_FALSE(DumpDetail::confirmQuestion("big.exe", 1001, someFolder(), true).contains("may not fit"));
}

TEST(ProcessDumpViewTest, TheConfirmWarnsEarlyWhenTheDriveIsTooFull)
{
    TestMocks::MockProcessActions mock;
    mock.setDumpSpace({.freeBytes = 512 * MIB, .processBytes = 3 * GIB});
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    ASSERT_TRUE(view.confirmPending());
    EXPECT_EQ(mock.dumpSpaceCount(), 1); // read once, when the confirm opens
    EXPECT_EQ(mock.lastSpaceDirectory(), someFolder());
    EXPECT_EQ(mock.lastTarget(), TARGET);
    EXPECT_TRUE(view.confirmQuestion().contains("may not fit")) << view.confirmQuestion();
    EXPECT_TRUE(view.confirmQuestion().contains("Only 512.0 MiB is free")) << view.confirmQuestion();

    // Still the user's call: the dump can be confirmed anyway.
    mock.setDumpResult(Platform::ProcessDumpResult(std::unexpected(std::string("Not enough disk space"))));
    view.confirm(&mock);
    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_EQ(mock.createDumpCount(), 1);
}

TEST(ProcessDumpViewTest, TheConfirmSaysNothingOfSpaceWhenTheDumpFits)
{
    TestMocks::MockProcessActions mock;
    mock.setDumpSpace({.freeBytes = 500 * GIB, .processBytes = 3 * GIB});
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    EXPECT_FALSE(view.confirmQuestion().contains("may not fit")) << view.confirmQuestion();
    EXPECT_EQ(view.confirmQuestion(), DumpDetail::confirmQuestion("big.exe", 1001, someFolder(), false));
}

// --- Save dump to... (#2001) ----------------------------------------------------------------------

[[nodiscard]] Platform::ProcessActionCapabilities folderCapable()
{
    Platform::ProcessActionCapabilities caps = dumpCapable(true);
    caps.canChooseDumpFolder = true;
    return caps;
}

[[nodiscard]] std::filesystem::path otherFolder()
{
    return std::filesystem::path("elsewhere") / "dumps";
}

TEST(ProcessDumpViewTest, AChosenFolderBecomesTheConfirmsAndIsRemembered)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    mock.setDumpSpace({.freeBytes = 512 * MIB, .processBytes = 3 * GIB}); // the first folder is too full
    mock.setFolderChoice(std::optional<std::filesystem::path>(otherFolder()));
    std::vector<std::filesystem::path> remembered;
    ProcessDumpView view([&remembered](const std::filesystem::path& folder) { remembered.push_back(folder); });
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    ASSERT_TRUE(view.confirmQuestion().contains("may not fit"));

    view.chooseFolder(&mock, nullptr);
    EXPECT_TRUE(view.choosingFolder());
    EXPECT_TRUE(view.active());
    EXPECT_TRUE(view.confirmPending()); // the confirm stays up behind the folder dialog
    view.confirm(&mock);                // its button is disabled meanwhile: nothing starts
    EXPECT_FALSE(view.busy());
    view.chooseFolder(&mock, nullptr); // and a second dialog is never opened over the first

    mock.setDumpSpace({.freeBytes = 500 * GIB, .processBytes = 3 * GIB}); // the new folder has room
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    EXPECT_FALSE(view.choosingFolder());
    // The new folder's drive is read on the worker (#3155): the confirm says so, and waits for it.
    EXPECT_TRUE(view.checkingFolder());
    EXPECT_TRUE(view.confirmQuestion().contains(DumpDetail::CHECKING_FOLDER_TEXT)) << view.confirmQuestion();
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_FALSE(view.checkingFolder());
    EXPECT_EQ(mock.chooseFolderCount(), 1);
    EXPECT_EQ(mock.lastFolderInitial(), someFolder()); // it opened where the dump was going
    EXPECT_EQ(view.directory(), otherFolder());
    ASSERT_EQ(remembered.size(), 1U);
    EXPECT_EQ(remembered.front(), otherFolder());
    // The space is checked again for the new folder, and the warning is gone.
    EXPECT_EQ(mock.dumpSpaceCount(), 2);
    EXPECT_EQ(mock.lastSpaceDirectory(), otherFolder());
    EXPECT_FALSE(view.confirmQuestion().contains("may not fit")) << view.confirmQuestion();
    EXPECT_TRUE(view.confirmQuestion().contains(otherFolder().string())) << view.confirmQuestion();

    mock.setDumpResult(
        Platform::ProcessDump{.file = otherFolder() / "big.dmp", .sizeBytes = 1, .note = {}, .fromSnapshot = true, .liveReason = {}});
    view.confirm(&mock);
    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_EQ(mock.lastDumpDirectory(), otherFolder());
}

TEST(ProcessDumpViewTest, ACancelledFolderDialogChangesNothing)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    mock.setFolderChoice(std::optional<std::filesystem::path>{});
    int remembered = 0;
    ProcessDumpView view([&remembered](const std::filesystem::path&) { ++remembered; });
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    const std::string question = view.confirmQuestion();
    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    EXPECT_EQ(view.directory(), someFolder());
    EXPECT_EQ(remembered, 0);
    EXPECT_EQ(view.confirmQuestion(), question);
    EXPECT_TRUE(view.confirmPending());
}

TEST(ProcessDumpViewTest, AFolderDialogThatFailsSaysWhyInTheConfirm)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    mock.setFolderChoice(std::unexpected(std::string("Couldn't show the folder dialog: Show failed (0x80004005)")));
    int remembered = 0;
    ProcessDumpView view([&remembered](const std::filesystem::path&) { ++remembered; });
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    EXPECT_EQ(view.directory(), someFolder());
    EXPECT_EQ(remembered, 0);
    EXPECT_TRUE(view.confirmQuestion().contains("Show failed (0x80004005)")) << view.confirmQuestion();
}

TEST(ProcessDumpViewTest, NoFolderDialogWithoutThePlatformsOrAConfirm)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(dumpCapable(true)); // canChooseDumpFolder false
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.chooseFolder(&mock, nullptr); // no confirm up
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    view.chooseFolder(&mock, nullptr);               // no folder dialog on this platform
    view.chooseFolder(nullptr, nullptr);
    EXPECT_FALSE(view.choosingFolder());
    EXPECT_EQ(mock.chooseFolderCount(), 0);

    // The default interface has none, and knows no space.
    struct Plain : TestMocks::MockProcessActions
    {
        [[nodiscard]] Platform::FolderChoice chooseDumpFolder(const std::filesystem::path& initial, void* ownerWindow) override
        {
            return Platform::IProcessActions::chooseDumpFolder(initial, ownerWindow); // NOLINT(bugprone-parent-virtual-call)
        }
        [[nodiscard]] Platform::DumpSpace dumpSpace(const Platform::ProcessTarget& target, const std::filesystem::path& directory) override
        {
            return Platform::IProcessActions::dumpSpace(target, directory); // NOLINT(bugprone-parent-virtual-call)
        }
    };
    Plain plain;
    const Platform::FolderChoice choice = plain.chooseDumpFolder(someFolder(), nullptr);
    ASSERT_FALSE(choice.has_value());
    EXPECT_EQ(choice.error(), Platform::FOLDER_CHOICE_UNSUPPORTED_MESSAGE);
    const Platform::DumpSpace space = plain.dumpSpace(TARGET, someFolder());
    EXPECT_FALSE(space.freeBytes.has_value());
    EXPECT_FALSE(space.processBytes.has_value());
    EXPECT_FALSE(Platform::ProcessActionCapabilities{}.canChooseDumpFolder);
}

TEST(ProcessDumpViewTest, AFolderChosenAfterTheConfirmClosedIsStillRemembered)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    mock.setFolderChoice(std::optional<std::filesystem::path>(otherFolder()));
    std::vector<std::filesystem::path> remembered;
    ProcessDumpView view([&remembered](const std::filesystem::path& folder) { remembered.push_back(folder); });
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    view.chooseFolder(&mock, nullptr);
    view.onSelectionChanged(); // the confirm closes; the folder dialog is the user's to answer
    EXPECT_FALSE(view.confirmPending());
    EXPECT_TRUE(view.active()); // until it is answered
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    ASSERT_EQ(remembered.size(), 1U);
    EXPECT_EQ(remembered.front(), otherFolder());
    EXPECT_FALSE(view.active());
    EXPECT_EQ(mock.createDumpCount(), 0);
}

TEST(ProcessDumpViewTest, ShutdownWaitsForTheFolderDialog)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    view.chooseFolder(&mock, nullptr);
    view.shutdown();
    EXPECT_EQ(mock.chooseFolderCount(), 1);
    EXPECT_TRUE(view.takeFolderChoice(&mock));
}

// --- The fallback folder dialog (#807: Linux, through SDL) ----------------------------------------

/// A fallback folder dialog that keeps what it was asked and lets the test answer when it likes.
struct FakeFolderDialog
{
    int shown = 0;
    std::filesystem::path initial;
    ProcessDumpView::FolderAnswer answer;

    [[nodiscard]] ProcessDumpView::FolderDialog dialog()
    {
        return [this](const std::filesystem::path& from, ProcessDumpView::FolderAnswer reply)
        {
            ++shown;
            initial = from;
            answer = std::move(reply);
        };
    }
};

TEST(ProcessDumpViewTest, WithoutThePlatformsDialogTheFallbackChoosesTheFolder)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(dumpCapable(true)); // no folder dialog in the platform layer, as on Linux
    std::vector<std::filesystem::path> remembered;
    ProcessDumpView view([&remembered](const std::filesystem::path& folder) { remembered.push_back(folder); });
    EXPECT_FALSE(view.canChooseFolder(&mock)) << "neither the platform's nor a fallback";
    FakeFolderDialog fake;
    view.setFolderDialog(fake.dialog());
    EXPECT_TRUE(view.canChooseFolder(&mock));
    EXPECT_TRUE(view.canChooseFolder(nullptr)) << "the fallback needs no actions";

    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    view.chooseFolder(&mock, nullptr);
    EXPECT_EQ(fake.shown, 1);
    EXPECT_EQ(fake.initial, someFolder()); // it opens where the dump was going
    EXPECT_TRUE(view.choosingFolder());
    EXPECT_FALSE(view.takeFolderChoice(&mock)) << "not answered yet";
    view.chooseFolder(&mock, nullptr); // and a second dialog is never opened over the first
    EXPECT_EQ(fake.shown, 1);

    // Answered later, from another thread, as SDL may.
    std::thread([&fake] { fake.answer(std::optional<std::filesystem::path>(otherFolder())); }).join();
    ASSERT_TRUE(view.takeFolderChoice(&mock));
    EXPECT_FALSE(view.choosingFolder());
    EXPECT_EQ(view.directory(), otherFolder());
    ASSERT_EQ(remembered.size(), 1U);
    EXPECT_EQ(remembered.front(), otherFolder());
    EXPECT_TRUE(view.confirmQuestion().contains(otherFolder().string())) << view.confirmQuestion();
    EXPECT_EQ(mock.chooseFolderCount(), 0) << "the platform has no dialog to ask";
}

TEST(ProcessDumpViewTest, TheFallbacksCancelChangesNothingAndItsFailureSaysWhy)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(dumpCapable(true));
    ProcessDumpView view([](const std::filesystem::path&) {});
    FakeFolderDialog fake;
    view.setFolderDialog(fake.dialog());
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    const std::string question = view.confirmQuestion();

    view.chooseFolder(&mock, nullptr);
    fake.answer(std::optional<std::filesystem::path>{});
    ASSERT_TRUE(view.takeFolderChoice(&mock));
    EXPECT_EQ(view.directory(), someFolder());
    EXPECT_EQ(view.confirmQuestion(), question);

    view.chooseFolder(&mock, nullptr);
    fake.answer(std::unexpected(std::string("Couldn't show the folder dialog: no portal")));
    fake.answer(std::optional<std::filesystem::path>(otherFolder())); // a second answer is ignored
    ASSERT_TRUE(view.takeFolderChoice(&mock));
    EXPECT_EQ(view.directory(), someFolder());
    EXPECT_TRUE(view.confirmQuestion().contains("no portal")) << view.confirmQuestion();
}

TEST(ProcessDumpViewTest, ThePlatformsFolderDialogIsPreferredToTheFallback)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    mock.setFolderChoice(std::optional<std::filesystem::path>(otherFolder()));
    ProcessDumpView view([](const std::filesystem::path&) {});
    FakeFolderDialog fake;
    view.setFolderDialog(fake.dialog());
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    EXPECT_EQ(mock.chooseFolderCount(), 1);
    EXPECT_EQ(fake.shown, 0);
    EXPECT_EQ(view.directory(), otherFolder());
}

TEST(ProcessDumpViewTest, ShutdownDoesNotWaitForTheFallbacksDialog)
{
    // SDL's dialog is answered from the event pump on the UI thread, so shutdown() (on that thread)
    // can't wait for it: the answer is dropped, and a late one lands harmlessly.
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(dumpCapable(true));
    ProcessDumpView view([](const std::filesystem::path&) {});
    FakeFolderDialog fake;
    view.setFolderDialog(fake.dialog());
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    view.chooseFolder(&mock, nullptr);
    view.shutdown(); // returns at once
    EXPECT_FALSE(view.choosingFolder());
    fake.answer(std::optional<std::filesystem::path>(otherFolder()));
    EXPECT_FALSE(view.takeFolderChoice(&mock));
    EXPECT_EQ(view.directory(), someFolder());
}

TEST(ProcessDumpViewTest, NoFolderChooserAtAllIsSaidInTheUsersTerms)
{
    // What SDL says on a system with neither the desktop portal nor zenity (seen under WSLg).
    const std::string none = DumpDetail::folderDialogFailure(
        "File dialog driver unsupported (supported values for SDL_HINT_FILE_DIALOG_DRIVER are 'zenity' and 'portal')");
    EXPECT_FALSE(none.contains("SDL_HINT")) << none;
    EXPECT_TRUE(none.contains("xdg-desktop-portal or zenity")) << none;
    // Anything else is SDL's own words.
    EXPECT_EQ(DumpDetail::folderDialogFailure("portal timed out"), "Couldn't show the folder dialog: portal timed out");
}

TEST(ProcessDumpViewTest, SdlsFolderDialogWithoutAWindowSaysSoAtOnce)
{
    int answers = 0;
    std::string error;
    DumpDetail::sdlFolderDialog(someFolder(),
                                [&answers, &error](const Platform::FolderChoice& choice)
                                {
                                    ++answers;
                                    error = choice.has_value() ? std::string{} : choice.error();
                                });
    EXPECT_EQ(answers, 1) << "answered at once, before returning";
    EXPECT_TRUE(error.contains("no window")) << error;
}

// --- The remembered folder (#2001) ----------------------------------------------------------------

/// A folder of this test's own under the temp folder, removed on scope exit.
/// createDump() and chooseDumpFolder() throwing, as a platform's might: a standard exception, or anything else.
class ThrowingActions : public TestMocks::MockProcessActions
{
  public:
    explicit ThrowingActions(bool standard) : m_Standard(standard)
    {
        setCapabilities(folderCapable());
    }

    [[nodiscard]] Platform::ProcessDumpResult createDump(const Platform::ProcessTarget& /*target*/,
                                                         const std::filesystem::path& /*directory*/,
                                                         const std::stop_token& /*stopToken*/) override
    {
        throwIt("the dump writer broke");
        return std::unexpected(std::string("unreachable"));
    }

    [[nodiscard]] Platform::FolderChoice chooseDumpFolder(const std::filesystem::path& /*initial*/, void* /*ownerWindow*/) override
    {
        throwIt("the folder dialog broke");
        return std::unexpected(std::string("unreachable"));
    }

  private:
    void throwIt(const char* what) const
    {
        if (m_Standard)
        {
            throw std::runtime_error(what);
        }
        throw 42; // NOLINT(hicpp-exception-baseclass) - not a std::exception, on purpose
    }

    bool m_Standard;
};

TEST(ProcessDumpViewTest, ADumpThatThrowsIsAFailureThatSaysWhy)
{
    for (const bool standard : {true, false})
    {
        SCOPED_TRACE(standard ? "std::exception" : "anything else");
        ThrowingActions actions(standard);
        ProcessDumpView view([](const std::filesystem::path&) {});
        view.request(TARGET, "big.exe", someFolder(), &actions);
        ASSERT_TRUE(view.takeConfirmCheck(&actions, true)); // the folder is checked on the worker (#3155)
        view.confirm(&actions);
        ASSERT_TRUE(view.takeFinished(true));
        ASSERT_TRUE(view.result().has_value());
        const DumpDetail::DumpMessage message = view.result().value_or(DumpDetail::DumpMessage{});
        EXPECT_FALSE(message.ok);
        EXPECT_TRUE(message.text.ends_with(standard ? "the dump writer broke" : "Unknown error")) << message.text;
    }
}

TEST(ProcessDumpViewTest, AFolderDialogThatThrowsSaysWhyInTheConfirm)
{
    for (const bool standard : {true, false})
    {
        SCOPED_TRACE(standard ? "std::exception" : "anything else");
        ThrowingActions actions(standard);
        int remembered = 0;
        ProcessDumpView view([&remembered](const std::filesystem::path&) { ++remembered; });
        view.request(TARGET, "big.exe", someFolder(), &actions);
        ASSERT_TRUE(view.takeConfirmCheck(&actions, true)); // the folder is checked on the worker (#3155)
        view.chooseFolder(&actions, nullptr);
        ASSERT_TRUE(view.takeFolderChoice(&actions, true));
        EXPECT_EQ(view.directory(), someFolder());
        EXPECT_EQ(remembered, 0);
        EXPECT_TRUE(view.confirmQuestion().contains(standard ? "the folder dialog broke" : "Unknown error")) << view.confirmQuestion();
    }
}

TEST(ProcessDumpViewTest, AFolderFailureFollowsTheSpaceWarning)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    mock.setDumpSpace({.freeBytes = 512 * MIB, .processBytes = 3 * GIB});
    mock.setFolderChoice(std::unexpected(std::string("Show failed (0x80004005)")));
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    const std::string& question = view.confirmQuestion();
    const auto space = question.find("may not fit");
    const auto failure = question.find("\n\nShow failed (0x80004005)");
    ASSERT_NE(space, std::string::npos) << question;
    ASSERT_NE(failure, std::string::npos) << question;
    EXPECT_LT(space, failure);
}

TEST(ProcessDumpViewTest, AMovedViewKeepsItsConfirm)
{
    TestMocks::MockProcessActions mock;
    mock.setDumpSpace({.freeBytes = 512 * MIB, .processBytes = 3 * GIB});
    ProcessDumpView from([](const std::filesystem::path&) {});
    from.request(TARGET, "big.exe", someFolder(), &mock);
    ASSERT_TRUE(from.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
    const std::string question = from.confirmQuestion();

    ProcessDumpView to([](const std::filesystem::path&) {});
    to = std::move(from);
    EXPECT_TRUE(to.confirmPending());
    EXPECT_EQ(to.directory(), someFolder());
    EXPECT_EQ(to.confirmQuestion(), question);

    // The moved-to view runs the dump.
    mock.setDumpResult(std::unexpected(std::string("Not enough disk space")));
    to.confirm(&mock);
    ASSERT_TRUE(to.takeFinished(true));
    EXPECT_EQ(mock.createDumpCount(), 1);
}

/// A folder of this test's own in the temp folder, removed afterwards. Its name has a random suffix and
/// is taken only when create_directory() makes it, so the suite running at the same time from another
/// build tree or worktree never shares it, or deletes it under this one (#2167).
class TempFolder
{
  public:
    TempFolder()
    {
        const std::filesystem::path base = std::filesystem::temp_directory_path() / "tasksmack-dumpview-test-folder-";
        std::random_device random;
        constexpr int MAX_ATTEMPTS = 100;
        for (int attempt = 0; attempt < MAX_ATTEMPTS && m_Path.empty(); ++attempt)
        {
            std::filesystem::path candidate = base;
            candidate += std::to_string(random());
            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec) && !ec)
            {
                m_Path = std::move(candidate);
            }
        }
        if (m_Path.empty())
        {
            throw std::runtime_error("TempFolder: could not create a folder of its own in the temp folder");
        }
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    TempFolder(TempFolder&&) = delete;
    TempFolder& operator=(TempFolder&&) = delete;
    ~TempFolder()
    {
        try
        {
            std::error_code ignored;
            std::filesystem::remove_all(m_Path, ignored);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a destructor; a folder left behind in the temp folder is harmless
        {}
    }
    [[nodiscard]] const std::filesystem::path& path() const
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

TEST(ProcessDumpViewTest, EachTempFolderIsItsOwn)
{
    const TempFolder first;
    const TempFolder second;
    EXPECT_NE(first.path(), second.path());
    EXPECT_TRUE(std::filesystem::is_directory(first.path()));
    EXPECT_TRUE(std::filesystem::is_directory(second.path()));
}

TEST(ProcessDumpViewTest, TheRememberedFolderIsUsedWhileItExists)
{
    const TempFolder folder;
    EXPECT_EQ(DumpDetail::dumpDirectory(Core::pathToUtf8(folder.path())), folder.path());
    // Never chosen, or gone since: the temp folder, as before.
    EXPECT_EQ(DumpDetail::dumpDirectory(""), DumpDetail::defaultDumpDirectory());
    EXPECT_EQ(DumpDetail::dumpDirectory(Core::pathToUtf8(folder.path() / "gone")), DumpDetail::defaultDumpDirectory());
}

// #1891: the default view remembers a chosen folder in the user's config, saving it once; choosing the
// same folder again leaves the config alone.
TEST(ProcessDumpViewTest, TheDefaultViewSavesAChosenFolderToTheConfig)
{
    const TempFolder folder;
    auto& config = UserConfig::get();
    const std::filesystem::path originalPath = config.configPath();
    const UserSettings originalSettings = config.settings();
    const std::filesystem::path configFile = folder.path() / "config.toml";
    config.resetConfigPathForTesting(configFile);
    config.settings().dumpFolder.clear();

    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    const std::filesystem::path chosen = folder.path() / "dumps";
    std::filesystem::create_directories(chosen);
    mock.setFolderChoice(std::optional<std::filesystem::path>(chosen));
    {
        ProcessDumpView view; // the default: remembers in UserConfig
        view.request(TARGET, "big.exe", someFolder(), &mock);
        ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
        view.chooseFolder(&mock, nullptr);
        ASSERT_TRUE(view.takeFolderChoice(&mock, true));
        EXPECT_EQ(view.directory(), chosen);
    }
    EXPECT_EQ(config.settings().dumpFolder, Core::pathToUtf8(chosen));
    config.flushPendingSave(); // saved on the settings worker, not the UI thread (#2916)
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(configFile, ec)) << "saved to the redirected config";
    const auto savedAt = std::filesystem::last_write_time(configFile, ec);

    {
        // The same folder again: already remembered, so nothing is written.
        ProcessDumpView view;
        view.request(TARGET, "big.exe", chosen, &mock);
        ASSERT_TRUE(view.takeConfirmCheck(&mock, true)); // the folder is checked on the worker (#3155)
        view.chooseFolder(&mock, nullptr);
        ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    }
    EXPECT_EQ(std::filesystem::last_write_time(configFile, ec), savedAt);

    config.resetConfigPathForTesting(originalPath);
    config.settings() = originalSettings;
}

TEST(ProcessDumpViewTest, TheConfigKeepsTheDumpFolder)
{
    UserSettings settings;
    EXPECT_TRUE(settings.dumpFolder.empty());
    ASSERT_TRUE(UserConfig::parseSettings("[dumps]\nfolder = 'D:\\Dumps'\n", settings));
    EXPECT_EQ(settings.dumpFolder, "D:\\Dumps");
}

// --- The confirm's folder check, on a worker (#3155) ----------------------------------------------

/// dumpSpace() held until the test releases it, as a remembered folder on an offline share holds
/// GetDiskFreeSpaceExW; or throwing, as a platform's might. Counts its calls (on the check's worker).
class HeldSpaceActions : public TestMocks::MockProcessActions
{
  public:
    enum class Mode : std::uint8_t
    {
        Hold,
        ThrowStandard,
        ThrowOther,
    };

    explicit HeldSpaceActions(Mode mode = Mode::Hold) : m_Mode(mode), m_Released(m_Release.get_future().share())
    {
        setCapabilities(folderCapable());
    }

    ~HeldSpaceActions() override
    {
        release(); // a test that failed before releasing it still tears down
    }

    HeldSpaceActions(const HeldSpaceActions&) = delete;
    HeldSpaceActions& operator=(const HeldSpaceActions&) = delete;
    HeldSpaceActions(HeldSpaceActions&&) = delete;
    HeldSpaceActions& operator=(HeldSpaceActions&&) = delete;

    [[nodiscard]] Platform::DumpSpace dumpSpace(const Platform::ProcessTarget& target, const std::filesystem::path& directory) override
    {
        m_Calls.fetch_add(1);
        if (m_Mode == Mode::ThrowStandard)
        {
            throw std::runtime_error("the drive didn't answer");
        }
        if (m_Mode == Mode::ThrowOther)
        {
            throw 42; // NOLINT(hicpp-exception-baseclass) - not a std::exception, on purpose
        }
        static_cast<void>(m_Released.wait_for(std::chrono::seconds(30))); // bounded: a failed test still tears down
        return MockProcessActions::dumpSpace(target, directory);
    }

    /// Lets every held dumpSpace() return, and every later one at once.
    void release()
    {
        if (!std::exchange(m_IsReleased, true))
        {
            m_Release.set_value();
        }
    }

    [[nodiscard]] int calls() const
    {
        return m_Calls.load();
    }

  private:
    Mode m_Mode;
    std::promise<void> m_Release;
    std::shared_future<void> m_Released;
    bool m_IsReleased = false; // the test thread's
    std::atomic<int> m_Calls{0};
};

TEST(ProcessDumpViewTest, TheConfirmsFolderIsCheckedOnAWorkerWhileItsButtonsWait)
{
    HeldSpaceActions actions;
    actions.setDumpSpace({.freeBytes = 512 * MIB, .processBytes = 3 * GIB});
    ProcessDumpView view([](const std::filesystem::path&) {});

    // Pending: request() returns while the drive is still being read; the confirm is up and says so.
    view.request(TARGET, "big.exe", someFolder(), &actions);
    ASSERT_TRUE(view.confirmPending());
    EXPECT_TRUE(view.checkingFolder());
    EXPECT_TRUE(view.confirmQuestion().contains(DumpDetail::CHECKING_FOLDER_TEXT)) << view.confirmQuestion();
    EXPECT_FALSE(view.confirmQuestion().contains("may not fit")) << "not known yet";
    EXPECT_FALSE(view.takeConfirmCheck(&actions)) << "never waited for unless asked";

    // Its buttons wait for it: neither the dump nor the folder dialog starts.
    view.confirm(&actions);
    view.chooseFolder(&actions, nullptr);
    EXPECT_FALSE(view.busy());
    EXPECT_FALSE(view.choosingFolder());
    EXPECT_EQ(actions.chooseFolderCount(), 0);
    // A second request while it is up is ignored, as before.
    view.request({.pid = 2002, .startTimeTicks = 6000}, "other.exe", otherFolder(), &actions);
    EXPECT_EQ(view.target(), TARGET);

    // Completion: the space warning replaces "Checking...", and the dump can be confirmed.
    actions.release();
    ASSERT_TRUE(view.takeConfirmCheck(&actions, true));
    EXPECT_FALSE(view.checkingFolder());
    EXPECT_EQ(actions.calls(), 1);
    EXPECT_EQ(actions.lastSpaceDirectory(), someFolder());
    EXPECT_FALSE(view.confirmQuestion().contains(DumpDetail::CHECKING_FOLDER_TEXT)) << view.confirmQuestion();
    EXPECT_TRUE(view.confirmQuestion().contains("Only 512.0 MiB is free")) << view.confirmQuestion();
    EXPECT_FALSE(view.takeConfirmCheck(&actions, true)) << "taken in once";
    actions.setDumpResult(std::unexpected(std::string("Not enough disk space")));
    view.confirm(&actions);
    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_EQ(actions.createDumpCount(), 1);
}

TEST(ProcessDumpViewTest, ACheckThatThrowsSaysSoAndStillOffersTheDump)
{
    for (const auto mode : {HeldSpaceActions::Mode::ThrowStandard, HeldSpaceActions::Mode::ThrowOther})
    {
        const bool standard = mode == HeldSpaceActions::Mode::ThrowStandard;
        SCOPED_TRACE(standard ? "std::exception" : "anything else");
        HeldSpaceActions actions(mode);
        ProcessDumpView view([](const std::filesystem::path&) {});
        view.request(TARGET, "big.exe", someFolder(), &actions);
        ASSERT_TRUE(view.takeConfirmCheck(&actions, true));
        EXPECT_FALSE(view.checkingFolder());
        EXPECT_EQ(view.directory(), someFolder()) << "the folder it was checking";
        EXPECT_TRUE(view.confirmQuestion().contains("Couldn't check the free space")) << view.confirmQuestion();
        EXPECT_TRUE(view.confirmQuestion().contains(standard ? "the drive didn't answer" : "Unknown error")) << view.confirmQuestion();
        // The warning was only a forecast: the dump is still the user's call.
        actions.setDumpResult(std::unexpected(std::string("Not enough disk space")));
        view.confirm(&actions);
        ASSERT_TRUE(view.takeFinished(true));
        EXPECT_EQ(actions.createDumpCount(), 1);
    }
}

TEST(ProcessDumpViewTest, ACheckOfAClosedConfirmIsDroppedAndTheNextOnesFollowsIt)
{
    HeldSpaceActions actions;
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.request(TARGET, "big.exe", someFolder(), &actions);
    view.cancelConfirm(); // closed while its check still runs
    EXPECT_FALSE(view.checkingFolder()) << "the check left running is no longer the confirm's";

    // The next confirm's check waits for the one still running, and is not mixed up with it.
    constexpr Platform::ProcessTarget OTHER{.pid = 2002, .startTimeTicks = 6000};
    view.request(OTHER, "other.exe", otherFolder(), &actions);
    ASSERT_TRUE(view.confirmPending());
    EXPECT_TRUE(view.checkingFolder());
    actions.release();
    EXPECT_FALSE(view.takeConfirmCheck(&actions, true)) << "the closed confirm's: dropped, and the next one started";
    EXPECT_TRUE(view.checkingFolder());
    ASSERT_TRUE(view.takeConfirmCheck(&actions, true));
    EXPECT_FALSE(view.checkingFolder());
    EXPECT_EQ(actions.calls(), 2);
    EXPECT_EQ(actions.lastSpaceDirectory(), otherFolder());
    EXPECT_EQ(actions.lastTarget(), OTHER);
    EXPECT_EQ(view.directory(), otherFolder());
    EXPECT_TRUE(view.confirmQuestion().contains("other.exe")) << view.confirmQuestion();
}

TEST(ProcessDumpViewTest, WithoutActionsNothingIsCheckedAndTheConfirmIsReadyAtOnce)
{
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.request(TARGET, "notepad.exe", someFolder());
    EXPECT_FALSE(view.checkingFolder());
    EXPECT_EQ(view.confirmQuestion(), DumpDetail::confirmQuestion("notepad.exe", 1001, someFolder(), false));
    EXPECT_FALSE(view.takeConfirmCheck(nullptr, true)) << "none was started";
}

// The Actions card's button and the row menu's item: the folder the user last chose is checked on the
// worker, never on the UI thread, and the default folder used where it is gone (#3155).
TEST(ProcessDumpViewTest, TheRememberedFolderIsCheckedOnTheWorker)
{
    const TempFolder folder;
    struct Case
    {
        std::string remembered;
        std::filesystem::path expected;
    };
    for (const Case& test : {Case{.remembered = Core::pathToUtf8(folder.path()), .expected = folder.path()},
                             Case{.remembered = Core::pathToUtf8(folder.path() / "gone"), .expected = DumpDetail::defaultDumpDirectory()},
                             Case{.remembered = {}, .expected = DumpDetail::defaultDumpDirectory()}})
    {
        SCOPED_TRACE(test.remembered);
        TestMocks::MockProcessActions mock;
        mock.setCapabilities(folderCapable());
        ProcessDumpView view([](const std::filesystem::path&) {});
        view.requestInRememberedFolder(TARGET, "big.exe", test.remembered, &mock);
        ASSERT_TRUE(view.confirmPending());
        EXPECT_TRUE(view.checkingFolder());
        ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
        EXPECT_EQ(view.directory(), test.expected);
        EXPECT_EQ(mock.dumpSpaceCount(), 1);
        EXPECT_EQ(mock.lastSpaceDirectory(), test.expected) << "the space of the folder the dump goes to";
    }

    // Without actions the folder is still resolved, on the worker; no space is read.
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.requestInRememberedFolder(TARGET, "big.exe", Core::pathToUtf8(folder.path() / "gone"), nullptr);
    EXPECT_TRUE(view.checkingFolder());
    ASSERT_TRUE(view.takeConfirmCheck(nullptr, true));
    EXPECT_EQ(view.directory(), DumpDetail::defaultDumpDirectory());
}

TEST(ProcessDumpViewTest, TheRememberedFolderIsTheConfigsAsWritten)
{
    auto& config = UserConfig::get();
    const std::string original = config.settings().dumpFolder;
    config.settings().dumpFolder = "D:\\Dumps";
    EXPECT_EQ(DumpDetail::rememberedDumpFolder(), "D:\\Dumps") << "unchecked: the worker checks it";
    config.settings().dumpFolder = original;
}

} // namespace
} // namespace App
