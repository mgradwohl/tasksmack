// tasksmack-helper's seccomp allow-list (#2752): each check runs in a forked child that installs the
// filter (no_new_privs is all it needs, so these run unprivileged in CI) and makes one call that would
// succeed without it. The helper itself runs under the enforcing filter in every TasksmackHelperTest and
// PkexecBrokerTest (test_BrokerServer.cpp).

#include "Platform/Linux/HelperSandbox.h"
#include "Platform/Linux/HelperSandboxMode.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string>

// NOLINTBEGIN(misc-include-cleaner) - kernel and syscall headers: include-cleaner lacks mappings for
// SYS_*, BPF_*, SECCOMP_*, PR_*, O_*, PROT_*, MAP_*, RLIMIT_*, FIOCLEX and the W* macros
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Platform
{
namespace
{

/// Exit status of a child whose filter didn't go in.
constexpr int NOT_INSTALLED = 100;

/// Runs @p body in a forked child under the filter in @p mode. @return the child's exit status (body's
/// return value), or minus the signal that killed it.
template<typename Body> [[nodiscard]] int underFilter(HelperSandbox::Mode mode, Body body)
{
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || !HelperSandbox::installFilter(mode))
        {
            ::_exit(NOT_INSTALLED);
        }
        ::_exit(body());
    }
    int status = 0;
    if (pid < 0 || ::waitpid(pid, &status, 0) != pid)
    {
        return -1000;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
}

/// errno when @p result is -1, else 0.
[[nodiscard]] int errorOf(long result)
{
    return result == -1 ? errno : 0;
}

/// A file it may create, in the test's temporary directory.
[[nodiscard]] std::string scratchPath()
{
    const char* const dir = std::getenv("TMPDIR"); // NOLINT(concurrency-mt-unsafe) - read before fork, no threads change it
    return std::string(dir != nullptr ? dir : "/tmp") + "/tasksmack-sandbox-test-" + std::to_string(::getpid());
}

class HelperSandboxTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!HelperSandbox::supported())
        {
            GTEST_SKIP() << "No filter for this architecture";
        }
    }
};

TEST_F(HelperSandboxTest, TheProgramChecksTheArchitectureFirstAndEndsInItsModesDefault)
{
    const auto enforce = HelperSandbox::program(HelperSandbox::Mode::Enforce, ::getpid());
    const auto log = HelperSandbox::program(HelperSandbox::Mode::Log, ::getpid());
    ASSERT_GT(enforce.size(), 3U);
    ASSERT_EQ(enforce.size(), log.size());
    EXPECT_EQ(enforce[0].code, BPF_LD | BPF_W | BPF_ABS);
    EXPECT_EQ(enforce[0].k, offsetof(seccomp_data, arch));
    EXPECT_EQ(enforce[2].code, BPF_RET | BPF_K);
    EXPECT_EQ(enforce[2].k, SECCOMP_RET_KILL_PROCESS); // another architecture is killed in either mode
    EXPECT_EQ(log[2].k, SECCOMP_RET_KILL_PROCESS);
    EXPECT_EQ(enforce.back().k, SECCOMP_RET_KILL_PROCESS);
    EXPECT_EQ(log.back().k, SECCOMP_RET_LOG);
}

TEST_F(HelperSandboxTest, ItsOwnJobStillWorks)
{
    const int status = underFilter(HelperSandbox::Mode::Enforce,
                                   []
                                   {
                                       const int fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
                                       if (fd < 0)
                                       {
                                           return 1;
                                       }
                                       std::array<char, 64> buffer{};
                                       if (::read(fd, buffer.data(), buffer.size()) <= 0 || ::close(fd) != 0)
                                       {
                                           return 2;
                                       }
                                       const long pidfd = ::syscall(SYS_pidfd_open, ::getpid(), 0);
                                       if (pidfd < 0)
                                       {
                                           return 3;
                                       }
                                       ::close(static_cast<int>(pidfd));
                                       if (::setpriority(PRIO_PROCESS, 0, 1) != 0)
                                       {
                                           return 4;
                                       }
                                       rlimit limit{};
                                       if (::prlimit(0, RLIMIT_NOFILE, nullptr, &limit) != 0)
                                       {
                                           return 5;
                                       }
                                       const void* const page =
                                           ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                                       return page == MAP_FAILED ? 6 : 0;
                                   });
    EXPECT_EQ(status, 0);
}

TEST_F(HelperSandboxTest, FilesOpenReadOnly)
{
    const std::string path = scratchPath();
    EXPECT_EQ(
        underFilter(HelperSandbox::Mode::Enforce, [&path] { return errorOf(::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600)); }),
        EACCES);
    EXPECT_NE(::access(path.c_str(), F_OK), 0); // nothing was created

    // A file this user may write: each writing open is refused, and it's left as it was.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::write(fd, "kept", 4), 4);
    ::close(fd);
    for (const int flags : {O_RDWR, O_WRONLY | O_TRUNC, O_WRONLY | O_APPEND})
    {
        EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [&path, flags] { return errorOf(::open(path.c_str(), flags | O_CLOEXEC)); }),
                  EACCES)
            << flags;
    }
    struct stat after{};
    EXPECT_EQ(::stat(path.c_str(), &after), 0);
    EXPECT_EQ(after.st_size, 4);
    ::unlink(path.c_str());
    // openat2's flags are out of the filter's reach: it says the call doesn't exist.
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [] { return errorOf(::syscall(SYS_openat2, AT_FDCWD, "/", nullptr, 0)); }), ENOSYS);
}

TEST_F(HelperSandboxTest, NoCodeIsLoadedOrRun)
{
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce,
                          []
                          {
                              const void* const page = ::mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                              return page == MAP_FAILED ? errno : 0;
                          }),
              EPERM);
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce,
                          []
                          {
                              std::string name = "true";
                              std::array<char*, 2> argv{name.data(), nullptr};
                              return errorOf(::execve("/bin/true", argv.data(), nullptr));
                          }),
              EPERM);
}

TEST_F(HelperSandboxTest, ItsCapabilitiesEscapesAreRefused)
{
    // Each would succeed (or fail otherwise) without the filter.
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce,
                          []
                          {
                              int value = 1;
                              int copy = 0;
                              const iovec local{.iov_base = &copy, .iov_len = sizeof(copy)};
                              const iovec remote{.iov_base = &value, .iov_len = sizeof(value)};
                              return errorOf(::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0)); // its own memory
                          }),
              EPERM);
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce,
                          [] { return errorOf(::syscall(SYS_name_to_handle_at, AT_FDCWD, "/", nullptr, nullptr, 0)); }),
              EPERM);
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce,
                          []
                          {
                              rlimit limit{};
                              return errorOf(::prlimit(::getppid(), RLIMIT_NOFILE, nullptr, &limit)); // another process's
                          }),
              EPERM);
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [] { return errorOf(::prctl(PR_SET_DUMPABLE, 1, 0, 0, 0)); }), EPERM);
}

TEST_F(HelperSandboxTest, OnlyTheBrokerSocketCarriesMessages)
{
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [] { return errorOf(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)); }),
              EPERM);
    std::array<int, 2> fds{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds.data()), 0);
    const int other = fds[0];
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [other] { return errorOf(::send(other, "x", 1, 0)); }), EPERM);
    ::close(fds[0]);
    ::close(fds[1]);
    // ioctl: only isatty()'s query.
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [] { return errorOf(::ioctl(STDERR_FILENO, FIOCLEX)); }), ENOTTY);
}

TEST_F(HelperSandboxTest, NoExecutableMemoryByAnotherRoute)
{
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce,
                          []
                          {
                              void* const page = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                              return page == MAP_FAILED ? 100 : errorOf(::mprotect(page, 4096, PROT_READ | PROT_EXEC));
                          }),
              EPERM);
    // READ_IMPLIES_EXEC would add PROT_EXEC to every readable mapping, past the mmap check.
    EXPECT_EQ(
        underFilter(HelperSandbox::Mode::Enforce, [] { return errorOf(::syscall(SYS_personality, 0x0400000 /* READ_IMPLIES_EXEC */)); }),
        EPERM);
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [] { return errorOf(::shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600)); }), EPERM);
}

TEST_F(HelperSandboxTest, ItSignalsOnlyItselfByNumber)
{
    // Another process only through a pidfd; by number only itself (abort(), raise()).
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [] { return errorOf(::kill(::getppid(), 0)); }), EPERM);
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [] { return errorOf(::kill(::getpid(), 0)); }), 0);
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, [] { return errorOf(::syscall(SYS_tgkill, ::getpid(), ::gettid(), 0)); }), 0);
}

#if defined(__x86_64__)
// The x32 ABI shares x86-64's audit architecture: its calls are told apart by a bit, and killed.
TEST_F(HelperSandboxTest, AnX32CallIsKilled)
{
    constexpr long X32_SYSCALL_BIT = 0x40000000;
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Log, [] { return errorOf(::syscall(X32_SYSCALL_BIT | SYS_getpid)); }), -SIGSYS);
}
#endif

TEST_F(HelperSandboxTest, AnUnlistedCallKillsUnderEnforceAndIsAllowedUnderLog)
{
    const auto unlisted = []
    {
        unsigned cpu = 0;
        return errorOf(::syscall(SYS_getcpu, &cpu, nullptr, nullptr));
    };
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Enforce, unlisted), -SIGSYS);
    EXPECT_EQ(underFilter(HelperSandbox::Mode::Log, unlisted), 0);
}

} // namespace
} // namespace Platform

// NOLINTEND(misc-include-cleaner)
