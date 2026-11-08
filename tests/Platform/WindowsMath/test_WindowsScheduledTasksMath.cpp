/// @file test_WindowsScheduledTasksMath.cpp
/// @brief The pure helpers behind the Startup tab's scheduled tasks (#3424): the trigger filter, Windows' own
/// tasks, whose task is the current user's, the command an exec action runs, an indirect Author, the error
/// wording and the entry a task becomes. Built on every platform.

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Windows/WindowsScheduledTasksMath.h"
#include "Platform/Windows/WindowsStartupProbeMath.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace Platform::Windows::ScheduledTaskMath
{
namespace
{

TEST(ScheduledTaskMathTest, OnlyAnEnabledLogonOrBootTriggerCounts)
{
    EXPECT_TRUE(isStartupTrigger(TRIGGER_LOGON));
    EXPECT_TRUE(isStartupTrigger(TRIGGER_BOOT));
    EXPECT_FALSE(isStartupTrigger(1));  // TASK_TRIGGER_TIME
    EXPECT_FALSE(isStartupTrigger(11)); // TASK_TRIGGER_SESSION_STATE_CHANGE (an unlock is not a sign-in)

    const std::vector<TaskTrigger> none;
    EXPECT_FALSE(startsAtSignInOrBoot(none));
    const std::vector<TaskTrigger> daily{{.type = 2, .enabled = true}};
    EXPECT_FALSE(startsAtSignInOrBoot(daily));
    const std::vector<TaskTrigger> disabledLogon{{.type = TRIGGER_LOGON, .enabled = false}};
    EXPECT_FALSE(startsAtSignInOrBoot(disabledLogon));
    const std::vector<TaskTrigger> mixed{{.type = 2, .enabled = true}, {.type = TRIGGER_BOOT, .enabled = true}};
    EXPECT_TRUE(startsAtSignInOrBoot(mixed));
}

TEST(ScheduledTaskMathTest, WindowsOwnTasksAreUnderMicrosoft)
{
    EXPECT_TRUE(isMicrosoftTask("\\Microsoft\\Windows\\UpdateOrchestrator\\Reboot"));
    EXPECT_TRUE(isMicrosoftTask("\\MICROSOFT\\Office\\Telemetry"));
    EXPECT_FALSE(isMicrosoftTask("\\Microsoft"));                   // a task named Microsoft at the root
    EXPECT_FALSE(isMicrosoftTask("\\Microsoft\\"));                 // the folder itself
    EXPECT_FALSE(isMicrosoftTask("\\MicrosoftEdgeUpdateTaskCore")); // Edge's updater, at the root, isn't
    EXPECT_FALSE(isMicrosoftTask("\\Vendor\\Microsoft\\Task"));
}

TEST(ScheduledTaskMathTest, TheCurrentUsersTaskIsMatchedByNameOrSid)
{
    const std::array<std::string, 3> me{"S-1-5-21-1-2-3-1001", "PC\\alice", "alice"};
    EXPECT_TRUE(runsAsUser("alice", me));
    EXPECT_TRUE(runsAsUser("pc\\ALICE", me));
    EXPECT_TRUE(runsAsUser("S-1-5-21-1-2-3-1001", me));
    EXPECT_FALSE(runsAsUser("SYSTEM", me));
    EXPECT_FALSE(runsAsUser("S-1-5-18", me));
    EXPECT_FALSE(runsAsUser("PC\\bob", me));
    EXPECT_FALSE(runsAsUser("", me)) << "a group's task runs for whoever signs in";
    const std::array<std::string, 1> unknown{""};
    EXPECT_FALSE(runsAsUser("alice", unknown));
}

TEST(ScheduledTaskMathTest, TheCommandQuotesAPathWithSpaces)
{
    EXPECT_EQ(taskCommand("C:\\Program Files\\A\\a.exe", "/x"), "\"C:\\Program Files\\A\\a.exe\" /x");
    EXPECT_EQ(taskCommand("\"C:\\Program Files\\A\\a.exe\"", "/x"), "\"C:\\Program Files\\A\\a.exe\" /x");
    EXPECT_EQ(taskCommand("%windir%\\system32\\sc.exe", ""), "%windir%\\system32\\sc.exe");
    EXPECT_EQ(taskCommand("", "/x"), "") << "a COM handler task runs no command line";
}

TEST(ScheduledTaskMathTest, AnIndirectAuthorNamesItsResource)
{
    EXPECT_EQ(indirectSource("$(@%SystemRoot%\\system32\\wininet.dll,-16776)"), "@%SystemRoot%\\system32\\wininet.dll,-16776");
    EXPECT_EQ(indirectSource("Vendor Inc."), "");
    EXPECT_EQ(indirectSource("$(@)"), "");
    EXPECT_EQ(indirectSource("$(@x"), "");
}

TEST(ScheduledTaskMathTest, ErrorsAreWorded)
{
    EXPECT_EQ(taskErrorText(HRESULT_ACCESS_DENIED), "Requires administrator");
    EXPECT_EQ(taskErrorText(HRESULT_FILE_NOT_FOUND), "The scheduled task is no longer registered");
    EXPECT_EQ(taskErrorText(HRESULT_PATH_NOT_FOUND), "The scheduled task is no longer registered");
    EXPECT_EQ(taskErrorText(0x80041326U), "Task Scheduler error 0x80041326");
}

TEST(ScheduledTaskMathTest, ATaskBecomesAnEntry)
{
    const std::array<std::string, 2> me{"S-1-5-21-9", "alice"};
    TaskRecord task;
    task.path = "\\Vendor\\Updater";
    task.enabled = false;
    task.execPath = "C:\\Vendor\\up.exe";
    task.userId = "S-1-5-21-9";
    const StartupEntry entry = taskEntry(task, me);
    EXPECT_EQ(entry.registrationName, "\\Vendor\\Updater");
    EXPECT_EQ(entry.command, "C:\\Vendor\\up.exe");
    EXPECT_EQ(entry.location, StartupLocation::ScheduledTaskUser);
    EXPECT_EQ(entry.scope, StartupScope::User);
    EXPECT_EQ(entry.scope, StartupMath::scopeOf(entry.location));
    EXPECT_FALSE(entry.enabled);
    EXPECT_FALSE(entry.osComponent);
    EXPECT_TRUE(hasSwitchableState(entry));
    EXPECT_FALSE(StartupMath::approvedKeyFor(entry.location).has_value()) << "Task Scheduler keeps its own state";
    EXPECT_EQ(StartupMath::startupEntryName(entry.location, "up.exe", entry.registrationName), "Updater");

    task.path = R"(\Microsoft\Windows\Defrag\ScheduledDefrag)";
    task.userId = "SYSTEM";
    const StartupEntry windows = taskEntry(task, me);
    EXPECT_EQ(windows.location, StartupLocation::ScheduledTaskMachine);
    EXPECT_EQ(windows.scope, StartupScope::Machine);
    EXPECT_EQ(windows.scope, StartupMath::scopeOf(windows.location));
    EXPECT_TRUE(windows.osComponent);
    EXPECT_EQ(windows.notSwitchableReason, MICROSOFT_TASK_REASON);
    EXPECT_FALSE(hasSwitchableState(windows));
    EXPECT_EQ(StartupMath::startupEntryName(windows.location, "defrag.exe", windows.registrationName), "ScheduledDefrag");
    EXPECT_EQ(StartupMath::startupEntryName(windows.location, "defrag.exe", ""), "defrag.exe");
}

} // namespace
} // namespace Platform::Windows::ScheduledTaskMath
