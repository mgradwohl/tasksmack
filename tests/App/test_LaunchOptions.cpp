/// @file test_LaunchOptions.cpp
/// @brief TaskSmack's command line (#3093, part 2): --minimized, which the sign-in entry passes, and
/// everything else ignored and listed for the log; the program's own path is never an argument. Started
/// in place of Task Manager (#3072), taskmgr.exe's path and Task Manager's arguments mark the start and are
/// neither read nor listed as TaskSmack's.

#include "App/LaunchOptions.h"
#include "Domain/StartAtLogin.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "Platform/Windows/ElevatedRestart.h"
#include "Platform/Windows/WinString.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

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

// #2728: an elevated restart is started with the Standard TaskSmack's PID and creation time, and the launching
// user's config directory (Platform/IElevatedRestart.h).
TEST(LaunchOptionsTest, AnElevatedRestartReadsTheHandOffAndTheConfigDirectory)
{
    const LaunchOptions options = parseLaunchOptions(std::vector<std::string>{
        "TaskSmack.exe",
        "--handoff",
        "4242",
        "133000000000000000",
        "--config-dir",
        R"(C:\Users\Pat Smith\AppData\Roaming\TaskSmack)",
    });
    EXPECT_EQ(options.handoffPid, std::optional<std::uint32_t>{4242});
    EXPECT_EQ(options.handoffStartTicks, 133'000'000'000'000'000ULL);
    EXPECT_EQ(options.configDir,
              std::optional<std::filesystem::path>{std::filesystem::path(u8R"(C:\Users\Pat Smith\AppData\Roaming\TaskSmack)")});
    EXPECT_TRUE(options.ignored.empty());
    EXPECT_FALSE(options.minimized);
}

TEST(LaunchOptionsTest, AConfigDirectoryWithoutAHandOffIsIgnored)
{
    const LaunchOptions options = parseLaunchOptions(std::vector<std::string>{"TaskSmack.exe", "--config-dir", R"(C:\elsewhere)"});
    EXPECT_FALSE(options.configDir.has_value()) << "only an elevated restart hands one over";
    EXPECT_FALSE(options.handoffPid.has_value());
    EXPECT_EQ(options.ignored, (std::vector<std::string>{"--config-dir"}));
}

TEST(LaunchOptionsTest, AHandOffWithoutAWholePidAndStartIsIgnored)
{
    for (const auto& [pid, ticks] : {
             std::pair{std::string("0"), std::string("5")},
             std::pair{std::string("-1"), std::string("5")},
             std::pair{std::string("12x"), std::string("5")},
             std::pair{std::string("99999999999"), std::string("5")},
             std::pair{std::string("42"), std::string("0")},
             std::pair{std::string("42"), std::string("x")},
             std::pair{std::string("42"), std::string("")},
         })
    {
        const LaunchOptions options =
            parseLaunchOptions(std::vector<std::string>{"TaskSmack.exe", "--handoff", pid, ticks, "--config-dir", R"(C:\dir)"});
        EXPECT_FALSE(options.handoffPid.has_value()) << pid << " " << ticks;
        EXPECT_FALSE(options.configDir.has_value()) << pid << " " << ticks;
    }
    const LaunchOptions noStart = parseLaunchOptions(std::vector<std::string>{"TaskSmack.exe", "--handoff", "42"});
    EXPECT_FALSE(noStart.handoffPid.has_value()) << "a PID without its creation time binds to nothing";
    EXPECT_EQ(noStart.ignored, (std::vector<std::string>{"--handoff", "42"}));
}

#ifdef _WIN32
// #4310: the elevated copy read --config-dir as UTF-8 (wideToUtf8()), then made a path of that, so a folder whose name
// holds a lone surrogate came back as its U+FFFD twin, which the folder check refused, and the session silently used
// the administrator's own settings. The hand-off line ElevatedRestart builds is wide and quoted, losslessly; split as
// the new copy splits it (CommandLineToArgvW()), the folder is taken from the wide arguments, exactly.
TEST(LaunchOptionsTest, AHandedOverConfigDirectoryWithALoneSurrogateRoundTripsExactly)
{
    std::wstring folder = LR"(C:\Users\Pat)";
    folder.push_back(static_cast<wchar_t>(0xD800)); // NTFS takes a lone surrogate in a name; UTF-8 has no form for it
    folder += LR"(\AppData\Roaming\TaskSmack)";
    const auto line = Platform::Windows::ElevatedRestart::parameters(4242, 133'000'000'000'000'000ULL, std::filesystem::path(folder));
    ASSERT_TRUE(line.has_value()) << line.error();

    int count = 0;
    LPWSTR* argv = CommandLineToArgvW((LR"("C:\Program Files\TaskSmack\TaskSmack.exe" )" + *line).c_str(), &count);
    ASSERT_NE(argv, nullptr);
    std::vector<std::string> utf8;
    std::vector<std::filesystem::path> native;
    for (const wchar_t* const argument : std::span<LPWSTR>(argv, static_cast<std::size_t>(count)))
    {
        utf8.push_back(Platform::WinString::wideToUtf8(argument));
        native.emplace_back(std::wstring(argument));
    }
    LocalFree(static_cast<HLOCAL>(argv));

    const LaunchOptions options = parseLaunchOptions(utf8, native);
    EXPECT_EQ(options.handoffPid, std::optional<std::uint32_t>{4242});
    ASSERT_TRUE(options.configDir.has_value());
    EXPECT_EQ(options.configDir.value_or(std::filesystem::path{}).native(), folder) << "the folder handed over, lone surrogate and all";
    EXPECT_TRUE(options.ignored.empty());

    // Its UTF-8 text alone names the U+FFFD twin: what was handed over before.
    const LaunchOptions lossy = parseLaunchOptions(utf8);
    ASSERT_TRUE(lossy.configDir.has_value());
    EXPECT_NE(lossy.configDir.value_or(std::filesystem::path{}).native(), folder);
}
#endif

} // namespace
} // namespace App
