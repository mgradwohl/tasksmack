/// @file test_ProcessDumpView.cpp
/// @brief "Create dump file..." (#807) without an ImGui context: which menus offer it, the confirm, the
/// dump on its worker through a mock IProcessActions, Cancel, and the result dialog's messages. The
/// dialogs themselves are driven headless in test_ProcessActionsBlockRender.cpp.

#include "App/Panels/ProcessDumpView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>

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
    const std::string question = DumpDetail::confirmQuestion("notepad.exe", 1001, someFolder());
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
    mock.setDumpResult(Platform::ProcessDump{.file = file, .sizeBytes = std::uint64_t{3} * 1024U * 1024U, .note = {}});
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
        "app32.exe", 77, Platform::ProcessDump{.file = "x.dmp", .sizeBytes = 1, .note = "This is a 32-bit process"});
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
        view.result().value_or(DumpDetail::DumpMessage{.ok = true, .title = {}, .text = {}, .file = {}, .note = {}});
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

} // namespace
} // namespace App
