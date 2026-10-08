/// @file test_LinuxProcessEnvironmentReader.cpp
/// @brief Platform::LinuxProcessEnvironmentReader against real processes (#179): a child started with
/// a known environment reads back as exactly that, a PID with no process reads as exited, a start
/// time that does not match the PID's process reads as exited (a reused PID), and another user's
/// process reads as permission denied.

#include "Platform/Factory.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/Linux/LinuxProcessEnvironmentReader.h"
#include "Platform/Linux/ProcParsing.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides kill(), csignal does not
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Platform
{
namespace
{

/// The start time of @p pid as the probe reports it, or 0 if it cannot be read.
[[nodiscard]] std::uint64_t startTicksOf(pid_t pid)
{
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    return ProcParsing::parseStatStartTime(line).value_or(0);
}

/// A child that execs `sleep` with a fixed environment, killed and reaped on scope exit.
class EnvironmentChild
{
  public:
    EnvironmentChild() : m_Pid(spawn())
    {}

    ~EnvironmentChild()
    {
        if (m_Pid > 0)
        {
            ::kill(m_Pid, SIGKILL);
            int status = 0;
            ::waitpid(m_Pid, &status, 0);
        }
    }

    EnvironmentChild(const EnvironmentChild&) = delete;
    EnvironmentChild& operator=(const EnvironmentChild&) = delete;
    EnvironmentChild(EnvironmentChild&&) = delete;
    EnvironmentChild& operator=(EnvironmentChild&&) = delete;

    [[nodiscard]] pid_t pid() const
    {
        return m_Pid;
    }

    /// Waits (up to about five seconds) until the child has exec'd sleep AND its new environment is in
    /// place. The name alone is not enough: execve renames the task before it builds the new stack and
    /// sets the environment's bounds, so a read in between sees an empty environ (#1497).
    [[nodiscard]] bool waitForExec() const
    {
        const std::string proc = "/proc/" + std::to_string(m_Pid);
        for (int attempt = 0; attempt < 500; ++attempt)
        {
            std::ifstream comm(proc + "/comm");
            std::string name;
            std::getline(comm, name);
            if (name == "sleep")
            {
                std::ifstream environ(proc + "/environ", std::ios::binary);
                if (environ.peek() != std::ifstream::traits_type::eof())
                {
                    return true;
                }
            }
            ::usleep(10'000);
        }
        return false;
    }

  private:
    /// Forks a child that execs /bin/sleep with exactly three variables; returns its PID (-1 on failure).
    [[nodiscard]] static pid_t spawn()
    {
        // Built before fork(): only async-signal-safe calls may run between fork and exec.
        std::string arg0 = "sleep";
        std::string arg1 = "60";
        std::string var0 = "ZED=last";
        std::string var1 = "MY_API_TOKEN=supersecret";
        std::string var2 = "FOO=bar=baz";
        std::array<char*, 3> argv{arg0.data(), arg1.data(), nullptr};
        std::array<char*, 4> envp{var0.data(), var1.data(), var2.data(), nullptr};
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            ::execve("/bin/sleep", argv.data(), envp.data());
            ::_exit(127);
        }
        return pid;
    }

    pid_t m_Pid = -1;
};

[[nodiscard]] std::optional<std::string> valueOf(const EnvironmentReadResult& result, std::string_view name)
{
    const auto it = std::ranges::find(result.variables, name, &EnvironmentVariable::name);
    return (it != result.variables.end()) ? std::optional<std::string>{it->value} : std::nullopt;
}

TEST(LinuxProcessEnvironmentReaderTest, ReportsSupport)
{
    const LinuxProcessEnvironmentReader reader;
    EXPECT_TRUE(reader.hasEnvironment());
    EXPECT_TRUE(makeProcessEnvironmentReader()->hasEnvironment());
}

TEST(LinuxProcessEnvironmentReaderTest, ReadsAChildsEnvironment)
{
    const EnvironmentChild child;
    ASSERT_GT(child.pid(), 0);
    ASSERT_TRUE(child.waitForExec());

    LinuxProcessEnvironmentReader reader;
    const EnvironmentReadResult result = reader.readEnvironment({.pid = child.pid(), .startTimeTicks = startTicksOf(child.pid())});
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok);
    ASSERT_EQ(result.variables.size(), 3U);
    EXPECT_EQ(result.variables[0].name, "ZED"); // block order; the view sorts
    EXPECT_EQ(valueOf(result, "MY_API_TOKEN"), "supersecret");
    EXPECT_EQ(valueOf(result, "FOO"), "bar=baz");
}

TEST(LinuxProcessEnvironmentReaderTest, UnknownStartTimeIsRefusedNotRead)
{
    const EnvironmentChild child;
    ASSERT_GT(child.pid(), 0);
    ASSERT_TRUE(child.waitForExec());

    // A live, readable process -- but with its identity unconfirmed, nothing is read (as the process
    // actions refuse an unknown start time).
    LinuxProcessEnvironmentReader reader;
    const EnvironmentReadResult result = reader.readEnvironment({.pid = child.pid(), .startTimeTicks = 0});
    EXPECT_EQ(result.status, EnvironmentReadStatus::IdentityUnknown);
    EXPECT_TRUE(result.variables.empty());
}

TEST(LinuxProcessEnvironmentReaderTest, MismatchedStartTimeIsAnExitedProcess)
{
    const EnvironmentChild child;
    ASSERT_GT(child.pid(), 0);
    ASSERT_TRUE(child.waitForExec());

    // The PID is live but the start time names a different (earlier) process: a reused PID.
    LinuxProcessEnvironmentReader reader;
    const EnvironmentReadResult result = reader.readEnvironment({.pid = child.pid(), .startTimeTicks = startTicksOf(child.pid()) + 12345});
    EXPECT_EQ(result.status, EnvironmentReadStatus::ProcessExited);
    EXPECT_TRUE(result.variables.empty());
}

TEST(LinuxProcessEnvironmentReaderTest, MissingProcessIsAnExitedProcess)
{
    // Fork a child that exits at once, reap it, and read its (now free) PID.
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        ::_exit(0);
    }
    ASSERT_GT(pid, 0);
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);

    LinuxProcessEnvironmentReader reader;
    const EnvironmentReadResult result = reader.readEnvironment({.pid = pid, .startTimeTicks = 1});
    // The PID could in principle have been handed out again already; its start time still won't match.
    EXPECT_EQ(result.status, EnvironmentReadStatus::ProcessExited);

    EXPECT_EQ(reader.readEnvironment({.pid = 0, .startTimeTicks = 0}).status, EnvironmentReadStatus::ProcessExited);
    EXPECT_EQ(reader.readEnvironment({.pid = -5, .startTimeTicks = 0}).status, EnvironmentReadStatus::ProcessExited);
}

TEST(LinuxProcessEnvironmentReaderTest, AnotherUsersProcessIsPermissionDenied)
{
    // PID 1 (init) belongs to root. As root, or with CAP_SYS_PTRACE, it opens: nothing to test. Whether
    // it opens decides it -- an empty environment is a valid one, so its contents are not checked.
    const std::ifstream probe("/proc/1/environ");
    if (probe.is_open())
    {
        GTEST_SKIP() << "/proc/1/environ is readable here (root or CAP_SYS_PTRACE)";
    }

    // With procfs mounted hidepid, /proc/1/stat is unreadable too: no start time, no identity, so the
    // reader rightly answers IdentityUnknown and there is no permission-denied path to exercise.
    const std::uint64_t initStartTicks = startTicksOf(1);
    if (initStartTicks == 0)
    {
        GTEST_SKIP() << "/proc/1/stat is unreadable here (procfs hidepid): PID 1's identity is unknown";
    }

    LinuxProcessEnvironmentReader reader;
    const EnvironmentReadResult result = reader.readEnvironment({.pid = 1, .startTimeTicks = initStartTicks});
    EXPECT_EQ(result.status, EnvironmentReadStatus::PermissionDenied);
    EXPECT_TRUE(result.variables.empty());
}

} // namespace
} // namespace Platform
