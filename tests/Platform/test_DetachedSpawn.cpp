/// @file test_DetachedSpawn.cpp
/// @brief Live tests for Platform::DetachedSpawn (#182): a fake "terminal" (a shell script that records
/// its argv, its PID and whether it inherited a descriptor) is started detached, and the tests check
/// what it received, that the call did not wait for it, and that it was never left as our zombie.
/// strace itself is never run.

#include "Platform/Linux/DetachedSpawn.h"
#include "Platform/Linux/SyscallTrace.h"
#include "ScopedTempDir.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <expected>
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
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform::DetachedSpawn
{
namespace
{

using TestSupport::ScopedTempDir;

/// waitpid(@p pid) without blocking: -1 with errno ECHILD when there is no such child of ours.
[[nodiscard]] pid_t waitNoHang(pid_t pid)
{
    int status = 0;
    // NOLINTNEXTLINE(misc-include-cleaner) - WNOHANG is provided by <sys/wait.h>
    return ::waitpid(pid, &status, WNOHANG);
}

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

/// A fake terminal in @p dir: records its argv (one per line) in argv.txt, its PID in pid.txt, and in
/// fds.txt "clean", or "leaked N" for each of @p probeFds open in it, then sleeps @p sleepSeconds.
/// Each file is renamed into place once written, so a reader never sees half of one.
[[nodiscard]] std::filesystem::path writeFakeTerminal(const std::filesystem::path& dir, const std::vector<int>& probeFds, int sleepSeconds)
{
    std::string fdChecks;
    for (const int fd : probeFds)
    {
        fdChecks += std::format("[ -e /proc/self/fd/{0} ] && echo 'leaked {0}'\n", fd);
    }
    const std::filesystem::path script = dir / "fake-terminal";
    writeScript(script,
                std::format("out='{0}'\n"
                            "for a in \"$@\"; do printf '%s\\n' \"$a\"; done > \"$out/argv.tmp\"\n"
                            "echo $$ > \"$out/pid.tmp\"\n"
                            "{{ {1} true; }} > \"$out/fds.tmp\"\n"
                            "[ -s \"$out/fds.tmp\" ] || echo clean > \"$out/fds.tmp\"\n"
                            "mv \"$out/pid.tmp\" \"$out/pid.txt\"\n"
                            "mv \"$out/fds.tmp\" \"$out/fds.txt\"\n"
                            "mv \"$out/argv.tmp\" \"$out/argv.txt\"\n"
                            "sleep {2}\n",
                            dir.string(),
                            fdChecks,
                            sleepSeconds));
    return script;
}

/// Inheritable (no FD_CLOEXEC) descriptors for the leak checks: one at 100, and one at 70000 -- above
/// 65535, where the old fallback stopped -- when RLIMIT_NOFILE allows it.
class LeakableFds
{
  public:
    LeakableFds()
    {
        for (const int minimum : {100, 70000})
        {
            const int fd = ::fcntl(STDERR_FILENO, F_DUPFD, minimum);
            if (fd >= 0)
            {
                m_Fds.push_back(fd);
            }
        }
    }
    ~LeakableFds()
    {
        for (const int fd : m_Fds)
        {
            ::close(fd);
        }
    }
    LeakableFds(const LeakableFds&) = delete;
    LeakableFds& operator=(const LeakableFds&) = delete;
    LeakableFds(LeakableFds&&) = delete;
    LeakableFds& operator=(LeakableFds&&) = delete;

    [[nodiscard]] const std::vector<int>& fds() const noexcept
    {
        return m_Fds;
    }

  private:
    std::vector<int> m_Fds;
};

TEST(DetachedSpawnTest, RunsTheProgramWithExactlyTheArgvGivenAndLeavesNoZombie)
{
    const ScopedTempDir dir("tasksmack_detached_spawn");
    // Descriptors deliberately left inheritable: the detached program must not get them.
    const LeakableFds leakable;
    ASSERT_FALSE(leakable.fds().empty());
    const std::filesystem::path terminal = writeFakeTerminal(dir.path, leakable.fds(), 2);

    // The real argv for a process whose name is shell syntax: it must arrive as one plain argument.
    const SyscallTrace::Terminal fake{.path = terminal.string(), .syntax = SyscallTrace::TerminalSyntax::DashE};
    std::vector<std::string> command = SyscallTrace::tracerCommand("/usr/bin/strace", 4242);
    command.emplace_back("$(touch injected); `id` \"quoted\" 'single' ;|&");
    const std::vector<std::string> argv = SyscallTrace::buildTerminalArgv(fake, command);

    const auto started = std::chrono::steady_clock::now();
    const auto result = spawnDetached(argv);
    const auto elapsed = std::chrono::steady_clock::now() - started;
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
    errno = 0;
    EXPECT_EQ(waitNoHang(programPid), -1);
    EXPECT_EQ(errno, ECHILD);
    // Nor is the intermediate child left behind: there is no child of ours to reap at all.
    errno = 0;
    EXPECT_EQ(waitNoHang(-1), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(DetachedSpawnTest, FallbackWithoutCloseRangeLeaksNoDescriptorEvenAbove65535)
{
    const ScopedTempDir dir("tasksmack_detached_spawn_fallback");
    const LeakableFds leakable;
    ASSERT_FALSE(leakable.fds().empty());
    const std::filesystem::path terminal = writeFakeTerminal(dir.path, leakable.fds(), 0);

    const std::vector<std::string> argv{terminal.string(), "fallback"};
    const auto result = Detail::spawnDetached(argv, Detail::SpawnHooks{.useCloseRange = false, .poll = nullptr});
    ASSERT_TRUE(result.has_value()) << spawnFailureMessage(result.error(), terminal.string());

    ASSERT_TRUE(waitForFile(dir.path / "argv.txt").has_value()) << "the fake terminal never ran";
    EXPECT_EQ(readFile(dir.path / "fds.txt").value_or(""), "clean\n");
    if (leakable.fds().size() < 2)
    {
        GTEST_SKIP() << "RLIMIT_NOFILE does not allow a descriptor at 70000; only the low one was checked";
    }
}

/// Runs @p body with descriptors 0, 1 and 2 closed, as when TaskSmack is started with them closed,
/// and restores them afterwards. @p body must not use gtest assertions (they print to stdout).
template<typename Body> void withStdioClosed(const Body& body)
{
    std::array<int, 3> saved{-1, -1, -1};
    for (int fd = 0; fd <= STDERR_FILENO; ++fd)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX fcntl() is variadic by definition
        saved.at(static_cast<std::size_t>(fd)) = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    }
    for (int fd = 0; fd <= STDERR_FILENO; ++fd)
    {
        ::close(fd);
    }
    body();
    for (int fd = 0; fd <= STDERR_FILENO; ++fd)
    {
        const int copy = saved.at(static_cast<std::size_t>(fd));
        if (copy >= 0)
        {
            ::dup2(copy, fd);
            ::close(copy);
        }
    }
}

TEST(DetachedSpawnTest, ReportsAnExecFailureEvenWithStandardDescriptorsClosed)
{
    // With 0-2 closed, /dev/null and the report pipe would land on them, and the child's dup2() onto
    // 0-2 would overwrite the pipe, turning this failure into a reported success.
    const ScopedTempDir dir("tasksmack_detached_spawn_nostdio");
    const std::vector<std::string> missing{(dir.path / "no-such-terminal").string()};
    std::optional<std::expected<void, SpawnFailure>> result;
    withStdioClosed([&] { result = spawnDetached(missing); });
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().stage, SpawnFailure::Stage::Exec);
    EXPECT_EQ(result->error().error, ENOENT);
}

TEST(DetachedSpawnTest, StartsTheProgramWithStandardDescriptorsClosed)
{
    const ScopedTempDir dir("tasksmack_detached_spawn_nostdio_ok");
    const std::filesystem::path terminal = writeFakeTerminal(dir.path, {}, 0);
    const std::vector<std::string> argv{terminal.string(), "no-stdio"};
    std::optional<std::expected<void, SpawnFailure>> result;
    withStdioClosed([&] { result = spawnDetached(argv); });
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value());
    const std::optional<std::string> recorded = waitForFile(dir.path / "argv.txt");
    ASSERT_TRUE(recorded.has_value()) << "the fake terminal never ran";
    EXPECT_EQ(lines(*recorded), std::vector<std::string>{"no-stdio"});
}

/// A poll() that fails outright, as after ENOMEM; counts its calls.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - a plain function pointer hook can't capture
int g_FailingPollCalls = 0;
int failingPoll(pollfd* /*fds*/, nfds_t /*count*/, int /*timeout*/)
{
    ++g_FailingPollCalls;
    errno = ENOMEM;
    return -1;
}

TEST(DetachedSpawnTest, AFailedPollIsTreatedAsATimeoutAndNeverBlocksOnRead)
{
    // The program does not exist, so its exec fails and a report would be waiting -- but with poll()
    // failing nothing may be read: the result is "assumed started", as for a timeout, never a block.
    const ScopedTempDir dir("tasksmack_detached_spawn_pollfail");
    const std::vector<std::string> missing{(dir.path / "no-such-terminal").string()};
    g_FailingPollCalls = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto result = Detail::spawnDetached(missing, Detail::SpawnHooks{.useCloseRange = true, .poll = &failingPoll});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(g_FailingPollCalls, 1);
    EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
}

TEST(DetachedSpawnTest, ReportsAProgramThatDoesNotExist)
{
    const ScopedTempDir dir("tasksmack_detached_spawn_missing");
    const std::vector<std::string> argv{(dir.path / "no-such-terminal").string(), "-e", "true"};
    const auto result = spawnDetached(argv);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().stage, SpawnFailure::Stage::Exec);
    EXPECT_EQ(result.error().error, ENOENT);
    errno = 0;
    EXPECT_EQ(waitNoHang(-1), -1);
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
