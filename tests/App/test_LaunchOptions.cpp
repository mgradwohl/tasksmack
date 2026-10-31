/// @file test_LaunchOptions.cpp
/// @brief TaskSmack's command line (#3093, part 2): --minimized, which the sign-in entry passes, and
/// everything else ignored and listed for the log; the program's own path is never an argument. Started
/// in place of Task Manager (#3072), taskmgr.exe's path and Task Manager's arguments mark the start and are
/// neither read nor listed as TaskSmack's.

#include "App/LaunchOptions.h"
#include "Domain/StartAtLogin.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace App
{
namespace
{

TEST(LaunchOptionsTest, NoArgumentsStartsNormally)
{
    const LaunchOptions options = parseLaunchOptions(std::vector<std::string>{R"(C:\Program Files\TaskSmack\TaskSmack.exe)"});
    EXPECT_FALSE(options.minimized);
    EXPECT_TRUE(options.ignored.empty());
    EXPECT_FALSE(parseLaunchOptions(std::vector<std::string>{}).minimized) << "no argv[0] either";
}

TEST(LaunchOptionsTest, MinimizedStartsMinimized)
{
    const LaunchOptions options = parseLaunchOptions(std::vector<std::string>{"TaskSmack.exe", "--minimized"});
    EXPECT_TRUE(options.minimized);
    EXPECT_TRUE(options.ignored.empty());
}

TEST(LaunchOptionsTest, TheProgramPathIsNeverTheArgument)
{
    // A copy saved as "--minimized" is still started normally.
    EXPECT_FALSE(parseLaunchOptions(std::vector<std::string>{"--minimized"}).minimized);
}

TEST(LaunchOptionsTest, OtherArgumentsAreIgnoredAndListed)
{
    const LaunchOptions options =
        parseLaunchOptions(std::vector<std::string>{"TaskSmack.exe", "/minimized", "--MINIMIZED", "--minimized", "x"});
    EXPECT_TRUE(options.minimized);
    EXPECT_EQ(options.ignored, (std::vector<std::string>{"/minimized", "--MINIMIZED", "x"}));
}

// #3072: Windows starts "TaskSmack.exe C:\WINDOWS\system32\taskmgr.exe [Task Manager's arguments]". The start
// is marked, and Task Manager's arguments, a --minimized among them, are not TaskSmack's.
TEST(LaunchOptionsTest, AStartInPlaceOfTaskManagerReadsNoneOfItsArguments)
{
    const std::string program = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";
    for (const std::vector<std::string>& arguments : {
             std::vector<std::string>{program, R"(C:\WINDOWS\system32\taskmgr.exe)"},
             std::vector<std::string>{program, R"(C:\Windows\System32\Taskmgr.exe)", "/4"},
             std::vector<std::string>{program, R"("C:\Windows\System32\taskmgr.exe")", "/7", "/startup"},
             std::vector<std::string>{program, "taskmgr", "--minimized"},
         })
    {
        const LaunchOptions options = parseLaunchOptions(arguments);
        EXPECT_TRUE(options.asTaskManager) << arguments[1];
        EXPECT_FALSE(options.minimized) << arguments[1] << ": Task Manager's argument, not TaskSmack's";
        EXPECT_TRUE(options.ignored.empty()) << arguments[1] << ": nothing reported as ignored";
    }
}

TEST(LaunchOptionsTest, TaskManagersPathAnywhereButFirstIsJustIgnored)
{
    const LaunchOptions later = parseLaunchOptions(std::vector<std::string>{"TaskSmack.exe", "--minimized", "taskmgr.exe"});
    EXPECT_FALSE(later.asTaskManager);
    EXPECT_TRUE(later.minimized);
    EXPECT_EQ(later.ignored, (std::vector<std::string>{"taskmgr.exe"}));
    EXPECT_FALSE(parseLaunchOptions(std::vector<std::string>{"taskmgr.exe"}).asTaskManager) << "argv[0] is never an argument";
    EXPECT_FALSE(parseLaunchOptions(std::vector<std::string>{"TaskSmack.exe"}).asTaskManager);
}

// The argument the sign-in entry passes is the one this reads.
TEST(LaunchOptionsTest, TheSignInEntrysCommandStartsMinimized)
{
    const std::string program = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";
    const std::string command = Domain::StartAtLogin::runCommand(program);
    ASSERT_TRUE(command.ends_with(" --minimized"));
    const std::string argument = command.substr(command.rfind(' ') + 1);
    EXPECT_TRUE(parseLaunchOptions(std::vector<std::string>{program, argument}).minimized);
}

} // namespace
} // namespace App
