/// @file test_ProcessDumpView.cpp
/// @brief "Create dump file..." (#807) without an ImGui context: which menus offer it, the confirm, the
/// dump on its worker through a mock IProcessActions, Cancel, and the result dialog's messages. The
/// dialogs themselves are driven headless in test_ProcessActionsBlockRender.cpp.

#include "App/Panels/ProcessDumpView.h"
#include "App/UserConfig.h"
#include "Core/ConfigDirOverride.h"
#include "Core/Utf8Path.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#ifdef __linux__
#include "Platform/Linux/LinuxCoreDump.h"
#include "Platform/Linux/RootDumpFolder.h"
#endif

#include <gtest/gtest.h>

#ifdef __linux__
#include <unistd.h>
#endif

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
#include <string_view>
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
    EXPECT_EQ(DumpDetail::defaultDumpDirectory(false), std::filesystem::temp_directory_path());
    EXPECT_EQ(DumpDetail::dumpDirectory("", false), std::filesystem::temp_directory_path());
}

// #4141: elevated, %TEMP% is the less-privileged side's (redirectable, and readable unelevated): never the default.
// tempDumpDirectory() stands in for the default here, so no test makes the elevated folder (#4188).
TEST(ProcessDumpViewTest, ASessionWithNoDefaultFolderHasNone)
{
    EXPECT_EQ(DumpDetail::tempDumpDirectory(false), std::filesystem::temp_directory_path());
    EXPECT_EQ(DumpDetail::tempDumpDirectory(true), std::filesystem::path{});
#ifdef __linux__
    // Run as root, the default is root's own folder (#4181), not none: only a non-root view forced to no default has none.
    EXPECT_EQ(DumpDetail::defaultDumpDirectory(true),
              ::geteuid() == 0 ? Platform::RootDumpFolder::rootDumpFolder() : std::filesystem::path{});
#endif
    const DumpDetail::DefaultFolderFn none = &DumpDetail::tempDumpDirectory;
    EXPECT_EQ(DumpDetail::dumpDirectory("", true, none), std::filesystem::path{}) << "nothing remembered: none";
    const std::filesystem::path temp = std::filesystem::temp_directory_path();
    EXPECT_EQ(DumpDetail::dumpDirectory(Core::pathToUtf8(temp / "tasksmack-no-such-dump-folder-4141"), true, none), std::filesystem::path{})
        << "gone: none, not the temp folder";
    // A folder that exists is still used: one chosen with Save dump to... in this session (trustedDumpFolder()).
    EXPECT_EQ(DumpDetail::dumpDirectory(Core::pathToUtf8(temp), true, none), temp);
#ifndef _WIN32
#ifdef __linux__
    EXPECT_EQ(DumpDetail::noDefaultDumpFolder(), ::geteuid() == 0) << "on Linux, any root process (#4163)";
#else
    EXPECT_FALSE(DumpDetail::noDefaultDumpFolder()) << "this test isn't elevated";
#endif
#endif
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

// --- Who can read the folder (#4164) ---------------------------------------------------------------

TEST(ProcessDumpViewTest, TheFolderAccessWarningNamesWhatTheCheckFound)
{
    using Platform::DumpFolderAccess;
    EXPECT_TRUE(DumpDetail::folderAccessWarning({}).empty()) << "not checked: an unelevated session's folder";
    EXPECT_TRUE(DumpDetail::folderAccessWarning({.access = DumpFolderAccess::AdministratorsOnly, .problem = {}}).empty());
    EXPECT_EQ(DumpDetail::folderAccessWarning({.access = DumpFolderAccess::ReadableByUser, .problem = {}}),
              DumpDetail::FOLDER_READABLE_TEXT);
    EXPECT_EQ(DumpDetail::folderAccessWarning({.access = DumpFolderAccess::Unknown, .problem = {}}),
              DumpDetail::FOLDER_ACCESS_UNKNOWN_TEXT);
    EXPECT_EQ(DumpDetail::folderAccessWarning({.access = DumpFolderAccess::Refused, .problem = "it is a junction"}), "it is a junction");
    EXPECT_FALSE(DumpDetail::folderAccessWarning({.access = DumpFolderAccess::Refused, .problem = {}}).empty());
}

// The confirm warns when programs running as the user could read the dump, and the admin can still go on, or choose
// another folder, which is checked again.
TEST(ProcessDumpViewTest, TheConfirmWarnsWhenTheFolderIsReadableAndStillConfirms)
{
    TestMocks::MockProcessActions mock;
    Platform::ProcessActionCapabilities caps = dumpCapable(true);
    caps.canChooseDumpFolder = true;
    mock.setCapabilities(caps);
    mock.setDumpSpace({.freeBytes = 512 * MIB, .processBytes = 3 * GIB});
    mock.setDumpFolderCheck({.access = Platform::DumpFolderAccess::ReadableByUser, .problem = {}});
    mock.setFolderChoice(std::optional<std::filesystem::path>(std::filesystem::path("admins") / "dumps"));
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.request(TARGET, "lsass.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_EQ(mock.dumpFolderAccessCount(), 1);
    EXPECT_EQ(mock.lastFolderAccessDirectory(), someFolder());
    const std::string& question = view.confirmQuestion();
    EXPECT_TRUE(question.contains(DumpDetail::FOLDER_READABLE_TEXT)) << question;
    EXPECT_TRUE(question.contains("may not fit")) << "the space warning still follows: " << question;
    EXPECT_LT(question.find(DumpDetail::FOLDER_READABLE_TEXT), question.find("may not fit")) << question;

    // Choose again: an administrators-only folder, checked on the worker, and the warning goes.
    mock.setDumpFolderCheck({.access = Platform::DumpFolderAccess::AdministratorsOnly, .problem = {}});
    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_EQ(mock.dumpFolderAccessCount(), 2);
    EXPECT_EQ(mock.lastFolderAccessDirectory(), std::filesystem::path("admins") / "dumps");
    EXPECT_FALSE(view.confirmQuestion().contains(DumpDetail::FOLDER_READABLE_TEXT)) << view.confirmQuestion();

    // A warning only: back to a readable folder, the dump is still confirmed.
    mock.setDumpFolderCheck({.access = Platform::DumpFolderAccess::ReadableByUser, .problem = {}});
    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_TRUE(view.confirmQuestion().contains(DumpDetail::FOLDER_READABLE_TEXT)) << view.confirmQuestion();
    mock.setDumpResult(Platform::ProcessDumpResult(std::unexpected(std::string("test"))));
    view.confirm(&mock);
    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_EQ(mock.createDumpCount(), 1);
}

// A folder the dump would refuse (a junction, as administrator) disables Create dump file until another is chosen.
TEST(ProcessDumpViewTest, ARefusedFolderCantBeConfirmedUntilAnotherIsChosen)
{
    TestMocks::MockProcessActions mock;
    Platform::ProcessActionCapabilities caps = dumpCapable(true);
    caps.canChooseDumpFolder = true;
    mock.setCapabilities(caps);
    mock.setDumpFolderCheck({.access = Platform::DumpFolderAccess::Refused, .problem = "it is a junction"});
    mock.setFolderChoice(std::optional<std::filesystem::path>(std::filesystem::path("admins") / "dumps"));
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.request(TARGET, "lsass.exe", someFolder(), &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_TRUE(view.folderRefused());
    EXPECT_TRUE(view.confirmQuestion().contains("it is a junction")) << view.confirmQuestion();
    view.confirm(&mock);
    EXPECT_FALSE(view.busy());
    EXPECT_TRUE(view.confirmPending()) << "nothing started";
    EXPECT_EQ(mock.createDumpCount(), 0);

    mock.setDumpFolderCheck({.access = Platform::DumpFolderAccess::AdministratorsOnly, .problem = {}});
    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_FALSE(view.folderRefused());
    mock.setDumpResult(Platform::ProcessDumpResult(std::unexpected(std::string("test"))));
    view.confirm(&mock);
    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_EQ(mock.createDumpCount(), 1);
}

// A confirm with no folder yet (an elevated session's, #4141) asks nothing about one.
TEST(ProcessDumpViewTest, NoFolderIsNoFolderAccessCheck)
{
    TestMocks::MockProcessActions mock;
    mock.setDumpFolderCheck({.access = Platform::DumpFolderAccess::ReadableByUser, .problem = {}});
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.setNoDefaultFolder(true);
    view.requestInRememberedFolder(TARGET, "lsass.exe", "", &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_TRUE(view.needsFolder());
    EXPECT_EQ(mock.dumpFolderAccessCount(), 0);
    EXPECT_FALSE(view.confirmQuestion().contains(DumpDetail::FOLDER_READABLE_TEXT)) << view.confirmQuestion();
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

// #4188: elevated, the default is TaskSmack's own admin-only folder (here a stand-in): the confirm names it, can be
// confirmed at once, and, as it passes the folder check, shows no warning. A folder the admin then picks is checked
// and warned about as before.
TEST(ProcessDumpViewTest, TheElevatedDefaultFolderIsUsedWithNoWarningAndAPickedOneStillWarns)
{
    const std::filesystem::path own = std::filesystem::path("ProgramData") / "TaskSmack" / "Dumps";
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    mock.setDumpFolderCheck({.access = Platform::DumpFolderAccess::AdministratorsOnly, .problem = {}});
    mock.setFolderChoice(std::optional<std::filesystem::path>(otherFolder()));
    std::atomic<int> asked{0};               // how often the default folder was asked for, on the confirm's worker
    std::atomic<bool> askedNoDefault{false}; // the noDefault it was last given
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.setNoDefaultFolder(true);
    view.setDefaultFolder(
        // NOLINTNEXTLINE(bugprone-exception-escape) - only the path's copy can throw; a throw there is a test failure
        [&asked, &askedNoDefault, &own](bool noDefault)
        {
            askedNoDefault = noDefault;
            ++asked;
            return std::filesystem::path(own); // a copy: the view keeps calling it
        });
    view.requestInRememberedFolder(TARGET, "lsass.exe", "", &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_EQ(asked.load(), 1) << "asked once, on the worker, for the elevated default";
    EXPECT_TRUE(askedNoDefault.load());
    EXPECT_EQ(view.directory(), own);
    EXPECT_FALSE(view.needsFolder()) << "no Save dump to... needed (#4162's wait is only the fallback)";
    EXPECT_EQ(mock.dumpFolderAccessCount(), 1);
    EXPECT_EQ(mock.lastFolderAccessDirectory(), own);
    EXPECT_FALSE(view.confirmQuestion().contains(DumpDetail::FOLDER_READABLE_TEXT)) << view.confirmQuestion();
    EXPECT_FALSE(view.confirmQuestion().contains(DumpDetail::FOLDER_ACCESS_UNKNOWN_TEXT)) << view.confirmQuestion();
    EXPECT_FALSE(view.confirmQuestion().contains(DumpDetail::NO_DEFAULT_FOLDER_TEXT)) << view.confirmQuestion();
    EXPECT_TRUE(view.confirmQuestion().contains(own.string())) << view.confirmQuestion();
    EXPECT_TRUE(view.resultFolderOpensOnlyElevated()) << "its result names it, with Copy, instead of Open folder";

    // The admin picks a folder programs running as them can open: it is checked, and the hint shows.
    mock.setDumpFolderCheck({.access = Platform::DumpFolderAccess::ReadableByUser, .problem = {}});
    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_EQ(view.directory(), otherFolder());
    EXPECT_EQ(mock.lastFolderAccessDirectory(), otherFolder());
    EXPECT_TRUE(view.confirmQuestion().contains(DumpDetail::FOLDER_READABLE_TEXT)) << view.confirmQuestion();
    EXPECT_EQ(asked.load(), 1) << "a chosen folder isn't resolved again";
    EXPECT_FALSE(view.resultFolderOpensOnlyElevated()) << "Explorer, as the user, can open this one";

    mock.setDumpResult(
        Platform::ProcessDump{.file = otherFolder() / "lsass.dmp", .sizeBytes = 1, .note = {}, .fromSnapshot = true, .liveReason = {}});
    view.confirm(&mock);
    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_EQ(mock.lastDumpDirectory(), otherFolder()) << "a hint, not a refusal";
}

// #4188: when the elevated default can't be made, the confirm falls back to #4162's: it waits for Save dump to...
TEST(ProcessDumpViewTest, WhenTheElevatedDefaultCantBeMadeTheConfirmWaitsForAFolder)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.setNoDefaultFolder(true);
    view.setDefaultFolder([](bool /*noDefault*/) { return std::filesystem::path{}; });
    view.requestInRememberedFolder(TARGET, "lsass.exe", "", &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_TRUE(view.needsFolder());
    EXPECT_TRUE(view.confirmQuestion().contains(DumpDetail::NO_DEFAULT_FOLDER_TEXT)) << view.confirmQuestion();
    view.confirm(&mock);
    EXPECT_EQ(mock.createDumpCount(), 0);
}

#ifdef _WIN32
// #4188 review: a hand-off session that isn't elevated can't make an admin-only file or folder, so the confirm says that,
// not that TaskSmack couldn't make its folder as administrator.
TEST(ProcessDumpViewTest, AHandOffSessionThatIsntElevatedSaysWhyItHasNoFolder)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.setNoDefaultFolder(true);
    view.setHandoffBlocked(true);
    view.setDefaultFolder([](bool /*noDefault*/) { return std::filesystem::path{}; });
    view.requestInRememberedFolder(TARGET, "lsass.exe", "", &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_TRUE(view.needsFolder());
    EXPECT_TRUE(view.confirmQuestion().contains(DumpDetail::HANDOFF_NO_DEFAULT_FOLDER_TEXT)) << view.confirmQuestion();
    EXPECT_FALSE(view.confirmQuestion().contains(DumpDetail::NO_DEFAULT_FOLDER_TEXT)) << view.confirmQuestion();
    EXPECT_TRUE(DumpDetail::HANDOFF_NO_DEFAULT_FOLDER_TEXT.contains("--handoff"));
}

// #4242: choosing a folder with Save dump to... doesn't help an unelevated hand-off: the text stays and nothing is made.
TEST(ProcessDumpViewTest, AnUnelevatedHandOffKeepsItsTextAndMakesNoDumpAfterAFolderIsChosen)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    mock.setDumpFolderCheck({.access = Platform::DumpFolderAccess::AdministratorsOnly, .problem = {}});
    mock.setFolderChoice(std::optional<std::filesystem::path>(otherFolder()));
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.setNoDefaultFolder(true);
    view.setHandoffBlocked(true);
    view.setDefaultFolder([](bool /*noDefault*/) { return std::filesystem::path{}; });
    view.requestInRememberedFolder(TARGET, "lsass.exe", "", &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_EQ(view.directory(), otherFolder());
    EXPECT_FALSE(view.needsFolder()) << "a folder is chosen";
    EXPECT_TRUE(view.handoffBlocked());
    EXPECT_TRUE(view.confirmQuestion().contains(DumpDetail::HANDOFF_NO_DEFAULT_FOLDER_TEXT)) << view.confirmQuestion();
    view.confirm(&mock);
    EXPECT_EQ(mock.createDumpCount(), 0);
}
#endif

// The warnings say the dump file itself is admin-only, and that the hint is about the folder (#4188).
TEST(ProcessDumpViewTest, TheFolderHintSaysTheFileItselfIsAdministratorsOnly)
{
    for (const std::string_view text : {DumpDetail::FOLDER_READABLE_TEXT, DumpDetail::FOLDER_ACCESS_UNKNOWN_TEXT})
    {
        EXPECT_TRUE(text.contains("The dump file itself is for administrators only")) << text;
        EXPECT_TRUE(text.contains("this folder")) << text;
    }
    EXPECT_TRUE(DumpDetail::FOLDER_READABLE_TEXT.contains("OneDrive"));
    EXPECT_TRUE(DumpDetail::FOLDER_READABLE_TEXT.contains("sync error")) << "OneDrive can't read an admin-only dump";
    EXPECT_FALSE(DumpDetail::FOLDER_READABLE_TEXT.contains("upload"));
    EXPECT_TRUE(DumpDetail::FOLDER_ACCESS_UNKNOWN_TEXT.contains("couldn't read this folder's permissions"));
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

// #4141: with no default folder (elevated), the confirm asks for one, and can't be confirmed until Save dump to...
// chooses it.
TEST(ProcessDumpViewTest, WithNoDefaultFolderTheConfirmWaitsForSaveDumpTo)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    mock.setFolderChoice(std::optional<std::filesystem::path>(otherFolder()));
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.setNoDefaultFolder(true);
    view.requestInRememberedFolder(TARGET, "lsass.exe", "", &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_EQ(view.directory(), std::filesystem::path{}) << "never the temp folder";
    EXPECT_TRUE(view.needsFolder());
    EXPECT_TRUE(view.confirmQuestion().contains(DumpDetail::NO_DEFAULT_FOLDER_TEXT)) << view.confirmQuestion();
    EXPECT_TRUE(view.confirmQuestion().contains("the folder you choose")) << view.confirmQuestion();
    EXPECT_FALSE(view.confirmQuestion().contains("the temporary folder")) << view.confirmQuestion();

    view.confirm(&mock); // its button is disabled: nothing starts
    EXPECT_FALSE(view.busy());
    EXPECT_TRUE(view.confirmPending());
    EXPECT_EQ(mock.createDumpCount(), 0);

    view.chooseFolder(&mock, nullptr);
    ASSERT_TRUE(view.takeFolderChoice(&mock, true));
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_FALSE(view.needsFolder());
    EXPECT_FALSE(view.confirmQuestion().contains(DumpDetail::NO_DEFAULT_FOLDER_TEXT)) << view.confirmQuestion();
    EXPECT_TRUE(view.confirmQuestion().contains(otherFolder().string())) << view.confirmQuestion();

    mock.setDumpResult(
        Platform::ProcessDump{.file = otherFolder() / "lsass.dmp", .sizeBytes = 1, .note = {}, .fromSnapshot = true, .liveReason = {}});
    view.confirm(&mock);
    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_EQ(mock.createDumpCount(), 1);
    EXPECT_EQ(mock.lastDumpDirectory(), otherFolder());
}

// #4141: unelevated, nothing changes: the confirm names the temp folder and can be confirmed at once.
TEST(ProcessDumpViewTest, WithADefaultFolderTheConfirmUsesTheTempFolder)
{
    const std::filesystem::path temp = std::filesystem::temp_directory_path();
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(folderCapable());
    ProcessDumpView view([](const std::filesystem::path&) {});
    view.setNoDefaultFolder(false);
    view.requestInRememberedFolder(TARGET, "notepad.exe", "", &mock);
    ASSERT_TRUE(view.takeConfirmCheck(&mock, true));
    EXPECT_EQ(view.directory(), temp);
    EXPECT_FALSE(view.needsFolder());
    EXPECT_FALSE(view.confirmQuestion().contains(DumpDetail::NO_DEFAULT_FOLDER_TEXT)) << view.confirmQuestion();

    mock.setDumpResult(
        Platform::ProcessDump{.file = temp / "notepad.dmp", .sizeBytes = 1, .note = {}, .fromSnapshot = true, .liveReason = {}});
    view.confirm(&mock);
    ASSERT_TRUE(view.takeFinished(true));
    EXPECT_EQ(mock.lastDumpDirectory(), temp);
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
    EXPECT_EQ(DumpDetail::dumpDirectory(Core::pathToUtf8(folder.path()), false), folder.path());
    // Never chosen, or gone since: the temp folder, as before.
    EXPECT_EQ(DumpDetail::dumpDirectory("", false), DumpDetail::defaultDumpDirectory(false));
    EXPECT_EQ(DumpDetail::dumpDirectory(Core::pathToUtf8(folder.path() / "gone"), false), DumpDetail::defaultDumpDirectory(false));
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
    const std::filesystem::path fallback = DumpDetail::defaultDumpDirectory(false); // a test's view has a default folder
    for (const Case& test : {
             Case{.remembered = Core::pathToUtf8(folder.path()), .expected = folder.path()},
             Case{.remembered = Core::pathToUtf8(folder.path() / "gone"), .expected = fallback},
             Case{.remembered = {}, .expected = fallback},
         })
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
    EXPECT_EQ(view.directory(), DumpDetail::defaultDumpDirectory(false)) << "a test's view has a default folder";
}

TEST(ProcessDumpViewTest, TheRememberedFolderIsTheConfigsAsWritten)
{
    auto& config = UserConfig::get();
    const std::string original = config.settings().dumpFolder;
    config.settings().dumpFolder = "D:\\Dumps";
    EXPECT_EQ(DumpDetail::rememberedDumpFolder(), "D:\\Dumps") << "unchecked: the worker checks it";
    config.settings().dumpFolder = original;
}

// #2728: a hand-off session doesn't dump into a folder its handed-over config names: that folder is the
// less-privileged side's choice, and an elevated dump there writes where it chose. The session's own Save dump to...
// choice is used, else the elevated account's default.
TEST(ProcessDumpViewTest, AHandOffSessionIgnoresTheHandedOverDumpFolder)
{
    EXPECT_EQ(DumpDetail::trustedDumpFolder("D:\\Dumps", false, ""), "D:\\Dumps");
    EXPECT_EQ(DumpDetail::trustedDumpFolder("C:\\Windows\\System32", true, ""), "") << "the default instead";
    EXPECT_EQ(DumpDetail::trustedDumpFolder("C:\\Windows\\System32", true, "E:\\Mine"), "E:\\Mine") << "chosen in this session";

    auto& config = UserConfig::get();
    const std::string original = config.settings().dumpFolder;
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - a copy: active() changes below
    const std::optional<std::filesystem::path> previous = Core::ConfigDirOverride::active();
    config.settings().dumpFolder = "C:\\Windows\\System32";
    Core::ConfigDirOverride::adoptHandedOver(std::filesystem::temp_directory_path());
    EXPECT_NE(DumpDetail::rememberedDumpFolder(), "C:\\Windows\\System32") << "the handed-over config's folder isn't used";
    Core::ConfigDirOverride::setActiveForTesting(previous);
    EXPECT_EQ(DumpDetail::rememberedDumpFolder(), "C:\\Windows\\System32") << "outside a hand-off, as written";
    config.settings().dumpFolder = original;
}
} // namespace
} // namespace App
