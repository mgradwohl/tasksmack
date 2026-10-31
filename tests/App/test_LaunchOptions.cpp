/// @file test_LaunchOptions.cpp
/// @brief TaskSmack's command line (#3093, part 2): --minimized, which the sign-in entry passes, and
/// everything else ignored and listed for the log; the program's own path is never an argument.

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
