/// @file test_DetachedSpawn.cpp
/// @brief Live tests for Platform::DetachedSpawn (#182): a fake "terminal" (a shell script that records
/// its argv, its PID and whether it inherited a descriptor) is started detached, and the tests check
/// what it received, that the call did not wait for it, and that it was never left as our zombie.
/// strace itself is never run.

#include "Platform/Linux/DetachedSpawn.h"
#include "Platform/Linux/SyscallTrace.h"
#include "ScopedTempDir.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - POSIX headers: include-cleaner lacks mappings for pid_t, wait macros
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform::DetachedSpawn
{
namespace
{

using TestSupport::ScopedTempDir;

// NOLINTNEXTLINE(misc-include-cleaner) - WNOHANG is provided by <sys/wait.h>
constexpr int NO_HANG = WNOHANG;

/// Reads @p path whole, or nullopt if it does not exist (yet).
[[nodiscard]] std::optional<std::string> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Waits up to 10 s for @p path to appear (the script renames it into place once it is complete).
[[nodiscard]] std::optional<std::string> waitForFile(const std::filesystem::path& path)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (std::optional<std::string> contents = readFile(path))
        {
            return contents;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> result;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
    {
        result.push_back(line);
    }
    return result;
}

/// Writes an executable /bin/sh script to @p path.
void writeScript(const std::filesystem::path& path, const std::string& body)
{
    {
        std::ofstream out(path);
        out << "#!/bin/sh\n" << body;
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
}

/// A fake terminal in @p dir: records its argv (one per line) in argv.txt, its PID in pid.txt, and
/// whether descriptor @p probeFd was open in it in fds.txt, then sleeps @p sleepSeconds. Each file is
/// renamed into place once written, so a reader never sees half of one.
[[nodiscard]] std::filesystem::path writeFakeTerminal(const std::filesystem::path& dir, int probeFd, int sleepSeconds)
{
    const std::filesystem::path script = dir / "fake-terminal";
    writeScript(script,
                std::format("out='{0}'\n"
                            "for a in \"$@\"; do printf '%s\\n' \"$a\"; done > \"$out/argv.tmp\"\n"
                            "echo $$ > \"$out/pid.tmp\"\n"
                            "if [ -e /proc/self/fd/{1} ]; then echo leaked; else echo clean; fi > \"$out/fds.tmp\"\n"
                            "mv \"$out/pid.tmp\" \"$out/pid.txt\"\n"
                            "mv \"$out/fds.tmp\" \"$out/fds.txt\"\n"
                            "mv \"$out/argv.tmp\" \"$out/argv.txt\"\n"
                            "sleep {2}\n",
                            dir.string(),
                            probeFd,
                            sleepSeconds));
    return script;
}

TEST(DetachedSpawnTest, RunsTheProgramWithExactlyTheArgvGivenAndLeavesNoZombie)
{
    const ScopedTempDir dir("tasksmack_detached_spawn");
    // A descriptor deliberately left inheritable: the detached program must not get it.
    const int leakable = ::fcntl(STDERR_FILENO, F_DUPFD, 100);
    ASSERT_GE(leakable, 0);
    const std::filesystem::path terminal = writeFakeTerminal(dir.path, leakable, 2);

    // The real argv for a process whose name is shell syntax: it must arrive as one plain argument.
    const SyscallTrace::Terminal fake{.path = terminal.string(), .syntax = SyscallTrace::TerminalSyntax::DashE};
    std::vector<std::string> command = SyscallTrace::tracerCommand("/usr/bin/strace", 4242);
    command.emplace_back("$(touch injected); `id` \"quoted\" 'single' ;|&");
    const std::vector<std::string> argv = SyscallTrace::buildTerminalArgv(fake, command);

    const auto started = std::chrono::steady_clock::now();
    const auto result = spawnDetached(argv);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ::close(leakable);
    ASSERT_TRUE(result.has_value()) << spawnFailureMessage(result.error(), terminal.string());
    // The script sleeps 2 s after recording; the call returns as soon as it has exec'd.
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500));

    const std::optional<std::string> recorded = waitForFile(dir.path / "argv.txt");
    ASSERT_TRUE(recorded.has_value()) << "the fake terminal never ran";
    // argv[0] is the shell's own; $@ is everything after it.
    const std::vector<std::string> expected(argv.begin() + 1, argv.end());
    EXPECT_EQ(lines(*recorded), expected);
    EXPECT_FALSE(std::filesystem::exists(dir.path / "injected"));
    EXPECT_EQ(readFile(dir.path / "fds.txt").value_or(""), "clean\n");

    // The program is not our child, so it can never become our zombie: waitpid() has nothing to wait
    // for, whether it is still sleeping or has exited.
    const std::string pidText = readFile(dir.path / "pid.txt").value_or("");
    pid_t programPid = 0;
    ASSERT_EQ(std::from_chars(pidText.data(), pidText.data() + pidText.size(), programPid).ec, std::errc{});
    int status = 0;
    errno = 0;
    EXPECT_EQ(::waitpid(programPid, &status, NO_HANG), -1);
    EXPECT_EQ(errno, ECHILD);
    // Nor is the intermediate child left behind: there is no child of ours to reap at all.
    errno = 0;
    EXPECT_EQ(::waitpid(-1, &status, NO_HANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(DetachedSpawnTest, ReportsAProgramThatDoesNotExist)
{
    const ScopedTempDir dir("tasksmack_detached_spawn_missing");
    const std::vector<std::string> argv{(dir.path / "no-such-terminal").string(), "-e", "true"};
    const auto result = spawnDetached(argv);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, SpawnFailure::Stage::Exec);
    EXPECT_EQ(result.error().error, ENOENT);
    int status = 0;
    errno = 0;
    EXPECT_EQ(::waitpid(-1, &status, NO_HANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(DetachedSpawnTest, ReportsAProgramThatIsNotExecutable)
{
    const ScopedTempDir dir("tasksmack_detached_spawn_noexec");
    const std::filesystem::path file = dir.path / "not-executable";
    {
        std::ofstream out(file);
        out << "#!/bin/sh\n";
    }
    std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root may exec any file with an execute bit; this one has none, but root's DAC override is not tested here";
    }
    const std::vector<std::string> argv{file.string()};
    const auto result = spawnDetached(argv);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, SpawnFailure::Stage::Exec);
    EXPECT_EQ(result.error().error, EACCES);
}

TEST(DetachedSpawnTest, RefusesAnEmptyOrRelativeArgv)
{
    EXPECT_FALSE(spawnDetached(std::vector<std::string>{}).has_value());
    const auto relative = spawnDetached(std::vector<std::string>{"xterm", "-e", "true"});
    ASSERT_FALSE(relative.has_value());
    EXPECT_EQ(relative.error().stage, SpawnFailure::Stage::Setup);
    EXPECT_EQ(relative.error().error, EINVAL);
}

TEST(DetachedSpawnTest, FailureMessagesNameTheProgramAndTheCause)
{
    EXPECT_EQ(spawnFailureMessage({.stage = SpawnFailure::Stage::Exec, .error = ENOENT}, "/usr/bin/xterm"),
              "Could not start /usr/bin/xterm: it no longer exists");
    EXPECT_EQ(spawnFailureMessage({.stage = SpawnFailure::Stage::Exec, .error = EACCES}, "/usr/bin/xterm"),
              "Could not start /usr/bin/xterm: permission denied");
    EXPECT_EQ(spawnFailureMessage({.stage = SpawnFailure::Stage::Fork, .error = EAGAIN}, "/usr/bin/xterm"),
              "Could not start a process for /usr/bin/xterm: " + std::system_category().message(EAGAIN));
    EXPECT_EQ(spawnFailureMessage({.stage = SpawnFailure::Stage::Setup, .error = EMFILE}, "/usr/bin/xterm"),
              "Could not prepare to start /usr/bin/xterm: " + std::system_category().message(EMFILE));
}

} // namespace
} // namespace Platform::DetachedSpawn
