/// @file test_WindowsStartupLiveReaders.cpp
/// @brief The real readers behind the Startup tab's impact column, scheduled tasks and the Boot performance
/// section (#3676). test_WindowsScheduledTasks.cpp, WindowsMath/test_WindowsStartupImpact.cpp and
/// test_WindowsSystemInfoReaders.cpp drive the walk, the parsers and the switch through fakes; these call the
/// Win32 readers themselves, and only read: the user's own StartupInfo files, the Task Scheduler's folders and
/// tasks, the Diagnostics-Performance event log. Each answer must be one the callers handle: a well-formed
/// result, or a failure with a code their reason text covers. Nothing here writes a task, a startup entry or a
/// file: the one switch call names a task that cannot exist, so it can only fail.

#include "Platform/IStartupProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/ComScope.h"
#include "Platform/Windows/WindowsBootPerformance.h"
#include "Platform/Windows/WindowsScheduledTasks.h"
#include "Platform/Windows/WindowsScheduledTasksMath.h"
#include "Platform/Windows/WindowsStartupImpact.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

namespace Math = Windows::ScheduledTaskMath;

// A task and a folder no machine has: the names carry this test's own marker.
constexpr const char* NO_SUCH_TASK = "\\TaskSmackLiveReaderTest-NoSuchTask-7f3a9c";
constexpr const char* NO_SUCH_FOLDER = "\\TaskSmackLiveReaderTest-NoSuchFolder-7f3a9c";

// ========== The sign-in measurement files ==========

TEST(WindowsStartupImpactLiveTest, EachFileReadsOrFailsWithAHandledError)
{
    const WindowsStartupImpact::Functions fns = WindowsStartupImpact::systemFunctions();
    ASSERT_NE(fns.readFile, nullptr);
    for (int index = WindowsStartupImpact::FIRST_FILE; index <= WindowsStartupImpact::LAST_FILE; ++index)
    {
        std::vector<std::uint8_t> bytes;
        std::uint32_t error = 99999;
        if (fns.readFile(index, bytes, error))
        {
            EXPECT_EQ(error, 0U) << "file " << index;
            EXPECT_LE(bytes.size(), WindowsStartupImpact::MAX_FILE_BYTES);
        }
        else
        {
            // Not there (a user who hasn't signed in five times, or a runner with no sign-ins), or not ours to read.
            EXPECT_TRUE(error == WindowsStartupImpact::ERROR_FILE_NOT_FOUND_CODE ||
                        error == WindowsStartupImpact::ERROR_PATH_NOT_FOUND_CODE || error == WindowsStartupImpact::ERROR_ACCESS_DENIED_CODE)
                << "file " << index << " error " << error;
        }
    }
}

TEST(WindowsStartupImpactLiveTest, NewestSignInIsMeasuredOrSaysWhyNot)
{
    const StartupImpactReading reading = WindowsStartupImpact::readNewestSignIn(WindowsStartupImpact::systemFunctions());
    if (reading.processes.empty())
    {
        EXPECT_FALSE(reading.unavailableReason.empty());
        return;
    }
    EXPECT_TRUE(reading.unavailableReason.empty());
    EXPECT_GT(reading.signInUnixSeconds, 0U);
    const bool ordered = std::ranges::is_sorted(reading.processes, {}, &StartupImpactProcess::startTicks);
    EXPECT_TRUE(ordered);
    for (const StartupImpactProcess& process : reading.processes)
    {
        EXPECT_FALSE(process.path.empty());
    }
}

// ========== The Task Scheduler ==========

/// A Task Scheduler API, not yet connected, inside a COM scope.
class WindowsScheduledTasksLiveTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Com = std::make_unique<ComScope>();
        ASSERT_TRUE(m_Com->usable());
        m_Api = Windows::makeTaskSchedulerApi();
        ASSERT_NE(m_Api, nullptr);
    }

    void TearDown() override
    {
        m_Api.reset(); // every COM object released before COM is uninitialised
        m_Com.reset();
    }

    std::unique_ptr<ComScope> m_Com;
    std::unique_ptr<Windows::ScheduledTaskApi> m_Api;
};

TEST_F(WindowsScheduledTasksLiveTest, NothingIsReadBeforeItIsConnected)
{
    EXPECT_TRUE(m_Api->subfolders("\\").empty());
    EXPECT_TRUE(m_Api->tasks("\\").empty());
    const Windows::TaskReading task = m_Api->task("\\Anything");
    ASSERT_FALSE(task.has_value());
    EXPECT_LT(task.error(), 0);
    EXPECT_LT(m_Api->setEnabled("\\Anything", true), 0);
}

/// The same, connected to the local Task Scheduler; skipped when this machine's service can't be reached.
class WindowsScheduledTasksConnectedLiveTest : public WindowsScheduledTasksLiveTest
{
  protected:
    void SetUp() override
    {
        WindowsScheduledTasksLiveTest::SetUp();
        if (IsSkipped() || HasFatalFailure())
        {
            return;
        }
        if (const std::int32_t result = m_Api->connect(); result < 0)
        {
            GTEST_SKIP() << "Task Scheduler not reachable: " << result;
        }
    }
};

TEST_F(WindowsScheduledTasksConnectedLiveTest, TheRootFolderHasSubfolders)
{
    const std::vector<std::string> folders = m_Api->subfolders("\\");
    EXPECT_FALSE(folders.empty()); // \Microsoft at least
    for (const std::string& folder : folders)
    {
        EXPECT_TRUE(folder.starts_with('\\')) << folder;
    }
    EXPECT_NE(std::ranges::find(folders, "\\Microsoft"), folders.end());
}

TEST_F(WindowsScheduledTasksConnectedLiveTest, AFolderThatIsNotThereListsNothing)
{
    EXPECT_TRUE(m_Api->subfolders(NO_SUCH_FOLDER).empty());
    EXPECT_TRUE(m_Api->tasks(NO_SUCH_FOLDER).empty());
}

TEST_F(WindowsScheduledTasksConnectedLiveTest, ATaskThatIsNotThereIsNotFoundAndIsNeverWritten)
{
    const Windows::TaskReading task = m_Api->task(NO_SUCH_TASK);
    ASSERT_FALSE(task.has_value());
    EXPECT_EQ(task.error(), static_cast<std::int32_t>(Math::HRESULT_FILE_NOT_FOUND));
    // The switch finds nothing to write to, so the real task list is untouched.
    EXPECT_LT(m_Api->setEnabled(NO_SUCH_TASK, false), 0);
    EXPECT_LT(m_Api->setEnabled(NO_SUCH_TASK, true), 0);
}

TEST_F(WindowsScheduledTasksConnectedLiveTest, StartupTasksReadAgainThroughTheirOwnPath)
{
    const std::optional<std::vector<Math::TaskRecord>> tasks = Windows::readStartupTasks(*m_Api);
    if (!tasks.has_value())
    {
        FAIL() << "the Task Scheduler connected but readStartupTasks gave nothing";
    }
    // Whatever the machine has: each listed task starts at sign-in or boot and reads back the same by its path.
    std::size_t checked = 0;
    for (const Math::TaskRecord& listed : *tasks)
    {
        EXPECT_FALSE(listed.path.empty());
        EXPECT_TRUE(Math::startsAtSignInOrBoot(listed.triggers)) << listed.path;
        if (checked++ >= 5)
        {
            continue; // a few are enough to prove the round trip
        }
        const Windows::TaskReading again = m_Api->task(listed.path);
        ASSERT_TRUE(again.has_value()) << listed.path;
        EXPECT_EQ(again->path, listed.path);
        EXPECT_EQ(again->enabled, listed.enabled) << listed.path;
        EXPECT_EQ(again->execPath, listed.execPath) << listed.path;
        EXPECT_EQ(again->userId, listed.userId) << listed.path;
    }
}

TEST_F(WindowsScheduledTasksLiveTest, TheCurrentUserIsNamedBySidFirst)
{
    const std::vector<std::string> names = m_Api->currentUserNames();
    ASSERT_FALSE(names.empty());
    EXPECT_TRUE(names.front().starts_with("S-1-")) << names.front();
    for (const std::string& name : names)
    {
        EXPECT_FALSE(name.empty());
    }
}

// ========== The Diagnostics-Performance boot event ==========

TEST(WindowsBootPerformanceLiveTest, ReadsTheBootOrSaysWhyNot)
{
    BootPerformanceInfo info;
    WindowsBootPerformance::readBootPerformance(info, WindowsBootPerformance::systemFunctions());
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    if (info.timingsRead)
    {
        EXPECT_TRUE(info.finished);
        EXPECT_GT(info.totalUs.value_or(0), 0U);
        EXPECT_TRUE(info.unavailableReason.empty());
    }
    else
    {
        // An unelevated TaskSmack is told so; a machine without the log says that; nothing else is blank.
        EXPECT_FALSE(info.unavailableReason.empty());
    }
}

} // namespace
} // namespace Platform
