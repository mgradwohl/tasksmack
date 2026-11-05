#include "LinuxProcessControl.h"

#include "CgroupFreezerPath.h"
#include "Domain/PriorityConfig.h"
#include "IoPriority.h"
#include "LinuxCpuDetails.h"
#include "LinuxEfficiencyMode.h"
#include "LinuxProcessActionsDetail.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/SchedPolicy.h"
#include "PosixGuards.h"
#include "PriorityErrorMessage.h"
#include "ProcParsing.h"
#include "ProcPrivileges.h"
#include "ThreadPriority.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sched.h>

// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides SIGKILL, SIGSTOP and SIGCONT; csignal is ISO C's
#include <signal.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

/// What the actions need of whatever process holds `pid` right now: its start time and kernel flags.
struct StatIdentity
{
    std::uint64_t startTime = 0;
    std::uint32_t flags = 0;
};

/// StatIdentity of whatever process holds `pid` right now, or nullopt if there is none.
[[nodiscard]] std::optional<StatIdentity> readStatIdentity(std::int32_t pid)
{
    const std::string statPath = "/proc/" + std::to_string(pid) + "/stat";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
    const FdGuard fd(::open(statPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        return std::nullopt;
    }

    // 1 KiB is ample: comm is kernel-capped at 15 chars and starttime is the 22nd field.
    std::array<char, 1024> buf{};
    const auto len = ::read(fd.get(), buf.data(), buf.size());
    if (len <= 0)
    {
        return std::nullopt;
    }
    const std::optional<ProcParsing::StatFields> fields =
        ProcParsing::parseStatFields(std::string_view(buf.data(), static_cast<std::size_t>(len)));
    if (!fields.has_value())
    {
        return std::nullopt;
    }
    return StatIdentity{.startTime = fields->startTime, .flags = fields->flags};
}

/// Start time of whatever process holds `pid` right now, or nullopt if there is none.
[[nodiscard]] std::optional<std::uint64_t> readStartTicks(std::int32_t pid)
{
    const std::optional<StatIdentity> stat = readStatIdentity(pid);
    return stat.has_value() ? std::optional<std::uint64_t>(stat->startTime) : std::nullopt;
}

/// Whether /proc has no entry for @p pid (ENOENT): the process has exited, where a failed read of its stat alone
/// could be any error (#3330).
[[nodiscard]] bool procEntryGone(std::int32_t pid)
{
    struct stat entry{};
    const std::string path = "/proc/" + std::to_string(pid);
    return ::stat(path.c_str(), &entry) != 0 && errno == ENOENT;
}

/// A process TaskSmack and the root helper don't act on, whatever the action (#2447, #2641, #2774).
enum class ProtectedProcess : std::uint8_t
{
    Init,         ///< PID 1
    KernelThread, ///< PF_KTHREAD in /proc/<pid>/stat
};

/// Whether `pid` is one TaskSmack doesn't act on, and why. The one rule both guards below word their
/// own messages for, so signals and scheduling changes always refuse the same processes.
[[nodiscard]] std::optional<ProtectedProcess> protectedProcess(std::int32_t pid)
{
    if (pid == 1)
    {
        return ProtectedProcess::Init;
    }
    const std::optional<StatIdentity> stat = readStatIdentity(pid);
    if (stat.has_value() && (stat->flags & ProcParsing::PF_KTHREAD_FLAG) != 0)
    {
        return ProtectedProcess::KernelThread;
    }
    return std::nullopt;
}

/// Why a signal to `pid` would do nothing, or nullopt. As root (the helper, #2308) the kernel accepts a
/// signal to a kernel thread or to init and drops it, so TaskSmack would report an action that never
/// happened (#2447): kernel threads ignore signals, and init drops SIGKILL and SIGSTOP and anything it
/// has no handler for (systemd re-executes itself on SIGTERM). An error, not a refusal: no password helps.
[[nodiscard]] std::optional<std::string> signalWouldDoNothing(std::int32_t pid)
{
    const std::optional<ProtectedProcess> reason = protectedProcess(pid);
    if (!reason.has_value())
    {
        return std::nullopt;
    }
    return *reason == ProtectedProcess::Init ? std::string("PID 1 is init, which TaskSmack doesn't end, suspend or resume")
                                             : std::format("PID {} is a kernel thread, which can't be ended, suspended or resumed", pid);
}

/// Why a priority, I/O priority, affinity or scheduling policy change is refused before it's tried (#2641, #3086), as
/// a signal is (signalWouldDoNothing()): PID 1, whose priority, affinity and policy every service systemd starts inherits,
/// and kernel threads (kswapd, jbd2/*, kworker/u*), where a low priority or the Idle I/O class can stall
/// reclaim or the journal for the whole machine. Through the helper they would otherwise succeed.
[[nodiscard]] std::optional<std::string> schedulingChangeRefused(std::int32_t pid)
{
    const std::optional<ProtectedProcess> reason = protectedProcess(pid);
    if (!reason.has_value())
    {
        return std::nullopt;
    }
    return *reason == ProtectedProcess::Init
             ? std::string("PID 1 is init, whose priority, CPU affinity and scheduling policy TaskSmack doesn't change")
             : std::format("PID {} is a kernel thread, whose priority, CPU affinity and scheduling policy TaskSmack doesn't change", pid);
}

/// Error text shared by every action refused because pidfds are unavailable.
[[nodiscard]] std::string noPidfdMessage(std::int32_t pid)
{
    return std::format("Cannot act on process {} safely: this system does not provide pidfds (Linux 5.3 or later, "
                       "not blocked by a sandbox), so a reused PID could not be ruled out; action not sent",
                       pid);
}

/// Whether @p err is the OS refusing for want of permission: an administrator may succeed (#2308).
[[nodiscard]] constexpr bool isPermissionError(int err) noexcept
{
    return err == EPERM || err == EACCES;
}

[[nodiscard]] std::string signalErrorMessage(int err)
{
    switch (err)
    {
    case EPERM:
        return "Permission denied - process belongs to another user";
    case ESRCH:
        return "Process not found - may have already exited";
    case EINVAL:
        return "Invalid signal";
    default:
        return std::system_category().message(err);
    }
}

/// Whether the process @p pidfd refers to has exited: 1 if it has, 0 if it is still running, -1 with
/// errno set if poll() fails. A pidfd polls readable once its process exits. Unlike signal 0, this
/// needs no permission over the process, so another user's live process (EPERM to a signal) is never
/// taken for an exited one (#803 review).
[[nodiscard]] int pidfdExited(const LinuxProcessSyscalls& syscalls, const FdGuard& pidfd)
{
    // A signal can interrupt even a zero-timeout poll(); that says nothing about the process, so it
    // is retried rather than reported as an unconfirmed identity (#803 review).
    int exited = syscalls.pollPidfd(pidfd.get());
    while (exited < 0 && errno == EINTR)
    {
        exited = syscalls.pollPidfd(pidfd.get());
    }
    if (exited < 0)
    {
        return -1;
    }
    return exited > 0 ? 1 : 0;
}

} // namespace

namespace LinuxProcessDetail
{

[[nodiscard]] PidfdOpen openPidfd(const LinuxProcessSyscalls& syscalls, std::int32_t pid)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - pid_t from <sys/types.h>, which clang-tidy 23 doesn't map here
    const int fd = syscalls.pidfdOpen(static_cast<pid_t>(pid));
    if (fd >= 0)
    {
        return {.fd = fd, .refusal = {}};
    }
    const int err = errno;
    // ENOSYS: a kernel before 5.3. EPERM/EACCES: pidfd_open never returns these itself, but a
    // seccomp filter does for syscalls it does not list (some container runtimes' default
    // profiles), so "process belongs to another user" would be the wrong message for them.
    if (err == ENOSYS || err == EPERM || err == EACCES)
    {
        return {.fd = -1, .refusal = noPidfdMessage(pid)};
    }
    return {.fd = -1, .refusal = signalErrorMessage(err)};
}

[[nodiscard]] ProcessActionResult verifyIdentity(const ProcessTarget& target)
{
    if (target.startTimeTicks == 0)
    {
        return checkProcessIdentity(target, 0);
    }
    const std::optional<std::uint64_t> actual = readStartTicks(target.pid);
    if (!actual.has_value())
    {
        return ProcessActionResult::error("Process not found - may have already exited");
    }
    return checkProcessIdentity(target, *actual);
}

[[nodiscard]] std::optional<std::string> readSmallFile(const std::string& path)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
    const FdGuard fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        return std::nullopt;
    }
    std::array<char, 4096> buf{};
    const auto len = ::read(fd.get(), buf.data(), buf.size());
    if (len < 0)
    {
        return std::nullopt;
    }
    return std::string(buf.data(), static_cast<std::size_t>(len));
}

} // namespace LinuxProcessDetail

namespace
{

using LinuxProcessDetail::openPidfd;
using LinuxProcessDetail::PidfdOpen;
using LinuxProcessDetail::readSmallFile;
using LinuxProcessDetail::sendThroughPidfd;
using LinuxProcessDetail::verifyIdentity;

/// Whether TaskSmack itself may set the Realtime I/O class: CAP_SYS_NICE or CAP_SYS_ADMIN in its own
/// effective set (ProcPrivileges::canSetRealtimeIoPriority()). Read once, by the constructor (#1540).
[[nodiscard]] bool ownProcessCanSetRealtimeIo()
{
    const std::optional<std::string> status = readSmallFile("/proc/self/status");
    return ProcPrivileges::canSetRealtimeIoPriority(status ? ProcPrivileges::parseCapEff(*status) : std::nullopt);
}

/// Whether TaskSmack itself may set a real-time scheduling policy at any priority: CAP_SYS_NICE in its own
/// effective set (ProcPrivileges::canSetRealtimeSchedPolicy()). Read once, by the constructor (#3219).
[[nodiscard]] bool ownProcessCanSetRealtimeSched()
{
    const std::optional<std::string> status = readSmallFile("/proc/self/status");
    return ProcPrivileges::canSetRealtimeSchedPolicy(status ? ProcPrivileges::parseCapEff(*status) : std::nullopt);
}

/// The process `id` (a PID, or a thread's TID) belongs to, as /proc/<id>/status names it (#3219); `id` itself
/// when that can't be read (it has exited, and the identity check will say so).
[[nodiscard]] std::int32_t threadGroupOf(std::int32_t id)
{
    const std::optional<std::string> status = readSmallFile(std::format("/proc/{}/status", id));
    return (status ? ProcPrivileges::parseTgid(*status) : std::nullopt).value_or(id);
}

/// Whether thread `tid` is still in `pid`'s thread group. A TID belongs to one task at a time, so if
/// it is still listed under /proc/<pid>/task after setpriority(2), the call reached a thread of the
/// target: had the worker exited and its TID been reused by another process, it would be listed
/// under that process instead (#1228 review).
[[nodiscard]] bool realIsThreadOf(pid_t pid, id_t tid)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path("/proc") / std::to_string(pid) / "task" / std::to_string(tid), ec);
}

/// The thread IDs in /proc/<pid>/task, or why they couldn't be listed. There is no fallback to
/// the PID alone: renicing just the main thread and reporting success is the bug this replaces.
[[nodiscard]] std::expected<std::vector<id_t>, std::error_code> realListThreads(pid_t pid)
{
    std::vector<id_t> tids;
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path("/proc") / std::to_string(pid) / "task", ec);
    for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        id_t tid = 0;
        const auto [ptr, parseError] = std::from_chars(name.data(), name.data() + name.size(), tid);
        if (parseError == std::errc{} && ptr == name.data() + name.size() && tid > 0)
        {
            tids.push_back(tid);
        }
    }
    if (ec)
    {
        return std::unexpected(ec);
    }
    if (tids.empty())
    {
        return std::unexpected(std::make_error_code(std::errc::no_such_process));
    }
    return tids;
}

// The real system calls (realProcessSyscalls()). pidfd_open, pidfd_send_signal and the ioprio calls have
// no glibc wrappers, so they are raw syscalls; a kernel or libc without them says ENOSYS.
int realPidfdOpen(pid_t pid)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U));
#else
    static_cast<void>(pid);
    errno = ENOSYS;
    return -1;
#endif
}

int realPidfdSendSignal(int pidfd, int signal)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    return static_cast<int>(::syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0U));
#else
    static_cast<void>(pidfd);
    static_cast<void>(signal);
    errno = ENOSYS;
    return -1;
#endif
}

int realPollPidfd(int pidfd)
{
    pollfd entry{.fd = pidfd, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&entry, 1, 0);
    if (ready < 0)
    {
        return -1;
    }
    return (ready > 0 && (entry.revents & (POLLIN | POLLHUP)) != 0) ? 1 : 0;
}

int realSetPriority(id_t tid, int nice)
{
    return ::setpriority(PRIO_PROCESS, tid, nice);
}

int realIoprioSet(id_t tid, int ioprio)
{
#ifdef SYS_ioprio_set
    return static_cast<int>(::syscall(SYS_ioprio_set, IoPrio::WHO_PROCESS, static_cast<pid_t>(tid), ioprio));
#else
    static_cast<void>(tid);
    static_cast<void>(ioprio);
    errno = ENOSYS;
    return -1;
#endif
}

int realSetAffinity(id_t tid, std::size_t setSize, const cpu_set_t* set)
{
    return ::sched_setaffinity(static_cast<pid_t>(tid), setSize, set);
}

int realSetScheduler(id_t tid, int policy, int priority)
{
    // sched_priority is 0 for SCHED_OTHER, SCHED_BATCH and SCHED_IDLE (#3086), 1 to 99 for SCHED_FIFO and SCHED_RR (#3219).
    const sched_param param{.sched_priority = priority}; // NOLINT(misc-include-cleaner) - sched_param is provided by <sched.h>
    return ::sched_setscheduler(static_cast<pid_t>(tid), policy, &param);
}

int realGetScheduler(id_t tid)
{
    return ::sched_getscheduler(static_cast<pid_t>(tid));
}

pid_t realWaitPid(pid_t pid, int* status, int options)
{
    return ::waitpid(pid, status, options);
}

int realIoprioGet(pid_t pid)
{
#ifdef SYS_ioprio_get
    return static_cast<int>(::syscall(SYS_ioprio_get, IoPrio::WHO_PROCESS, pid));
#else
    static_cast<void>(pid);
    errno = ENOSYS;
    return -1;
#endif
}
int realPidfdGetfd(int pidfd, int targetFd)
{
#if defined(SYS_pidfd_getfd)
    return static_cast<int>(::syscall(SYS_pidfd_getfd, pidfd, targetFd, 0U));
#else
    static_cast<void>(pidfd);
    static_cast<void>(targetFd);
    errno = ENOSYS;
    return -1;
#endif
}

int realShutdownSocket(int fd)
{
    return ::shutdown(fd, SHUT_RDWR);
}

std::expected<int, int> realSocketDescriptor(std::int32_t pid, std::uint64_t inode)
{
    const std::string wanted = std::format("socket:[{}]", inode);
    const std::filesystem::path fdDir = std::filesystem::path("/proc") / std::to_string(pid) / "fd";
    std::error_code ec;
    std::filesystem::directory_iterator it(fdDir, ec);
    if (ec)
    {
        return std::unexpected(ec.value());
    }
    // Incremented with an error code: a range-for would throw if the listing failed part-way.
    for (; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
    {
        std::error_code linkError;
        const std::filesystem::path target = std::filesystem::read_symlink(it->path(), linkError);
        if (linkError || target.native() != wanted)
        {
            continue;
        }
        int fd = -1;
        const std::string name = it->path().filename().string();
        const auto [ptr, parseError] = std::from_chars(name.data(), name.data() + name.size(), fd);
        if (parseError == std::errc{} && ptr == name.data() + name.size())
        {
            return fd;
        }
    }
    return std::unexpected(ec ? ec.value() : ENOENT);
}

std::optional<std::int32_t> realCurrentNice(std::int32_t pid)
{
    errno = 0;
    const int nice = ::getpriority(PRIO_PROCESS, static_cast<id_t>(pid));
    if (nice == -1 && errno != 0)
    {
        return std::nullopt;
    }
    return nice;
}

std::optional<std::string> realProcCgroup(std::int32_t pid)
{
    return readSmallFile(std::format("/proc/{}/cgroup", pid));
}

/// <cgroupRoot>/<cgroup>/<file>, refused for a cgroup path that would leave the root (CodeQL cpp/path-injection).
[[nodiscard]] std::optional<std::filesystem::path> cgroupFilePath(const std::string& cgroup, std::string_view file)
{
    return CgroupPath::buildContainedCgroupPath("/sys/fs/cgroup", cgroup, file);
}

std::optional<std::string> realReadCgroupFile(const std::string& cgroup, std::string_view file)
{
    const std::optional<std::filesystem::path> path = cgroupFilePath(cgroup, file);
    return path.has_value() ? readSmallFile(path->string()) : std::nullopt;
}

std::expected<std::uint64_t, int> realCgroupIdentity(const std::string& cgroup)
{
    const std::optional<std::filesystem::path> procs = cgroupFilePath(cgroup, LinuxEfficiency::CGROUP_PROCS_FILE);
    if (!procs.has_value())
    {
        return std::unexpected(EINVAL);
    }
    struct stat directory{};
    if (::stat(procs->parent_path().c_str(), &directory) != 0)
    {
        return std::unexpected(errno);
    }
    return static_cast<std::uint64_t>(directory.st_ino);
}

int realWriteCgroupFile(const std::string& cgroup, std::string_view file, const std::string& value)
{
    const std::optional<std::filesystem::path> path = cgroupFilePath(cgroup, file);
    if (!path.has_value())
    {
        return EINVAL;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const FdGuard fd(::open(path->c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.get() < 0)
    {
        return errno;
    }
    const ssize_t written = ::write(fd.get(), value.data(), value.size()); // NOLINT(misc-include-cleaner) - ssize_t is from <sys/types.h>
    if (written < 0)
    {
        return errno;
    }
    return static_cast<std::size_t>(written) == value.size() ? 0 : EIO;
}

int realWriteCgroupFileOf(const std::string& cgroup, std::uint64_t identity, std::string_view file, const std::string& value)
{
    const std::optional<std::filesystem::path> procs = cgroupFilePath(cgroup, LinuxEfficiency::CGROUP_PROCS_FILE);
    if (!procs.has_value() || file.contains('/'))
    {
        return EINVAL;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    // O_PATH: search permission on the directory is all this needs, as a write by path needed; fstat() and openat()
    // both work on such a descriptor.
    const FdGuard directory(::open(procs->parent_path().c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0)
    {
        return errno;
    }
    struct stat opened{};
    if (::fstat(directory.get(), &opened) != 0)
    {
        return errno;
    }
    if (static_cast<std::uint64_t>(opened.st_ino) != identity)
    {
        // Another cgroup at the path now.
        return ESTALE; // NOLINT(misc-include-cleaner) - <cerrno> has it; include-cleaner maps it to a kernel header
    }
    const std::string name(file);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const FdGuard fd(::openat(directory.get(), name.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.get() < 0)
    {
        return errno;
    }
    const ssize_t written = ::write(fd.get(), value.data(), value.size()); // NOLINT(misc-include-cleaner) - ssize_t is from <sys/types.h>
    if (written < 0)
    {
        return errno;
    }
    return static_cast<std::size_t>(written) == value.size() ? 0 : EIO;
}

} // namespace

LinuxProcessSyscalls realProcessSyscalls() noexcept
{
    return {
        .pidfdOpen = &realPidfdOpen,
        .pidfdSendSignal = &realPidfdSendSignal,
        .pollPidfd = &realPollPidfd,
        .setPriority = &realSetPriority,
        .ioprioSet = &realIoprioSet,
        .ioprioGet = &realIoprioGet,
        .setAffinity = &realSetAffinity,
        .setScheduler = &realSetScheduler,
        .getScheduler = &realGetScheduler,
        .listThreads = &realListThreads,
        .isThreadOf = &realIsThreadOf,
        .waitPid = &realWaitPid,
        .pidfdGetfd = &realPidfdGetfd,
        .shutdownSocket = &realShutdownSocket,
        .socketDescriptor = &realSocketDescriptor,
        .currentNice = &realCurrentNice,
        .procCgroup = &realProcCgroup,
        .readCgroupFile = &realReadCgroupFile,
        .writeCgroupFile = &realWriteCgroupFile,
        .cgroupIdentity = &realCgroupIdentity,
        .writeCgroupFileOf = &realWriteCgroupFileOf,
    };
}

struct LinuxProcessControl::EfficiencyMemory
{
    /// What on found before it changed a process: its nice, and its cgroup's weight when it lowered it.
    struct Before
    {
        std::int32_t nice = 0;
        std::optional<int> cpuWeight;
        /// The cgroup whose weight it lowered (#3331): off puts the weight back there, even if the process has
        /// moved since, and never writes it into the cgroup the process is in now.
        std::optional<std::string> cgroup;
        /// That cgroup's identity (its directory's inode) when on lowered it: off puts nothing into another cgroup
        /// made at the same path since (#3331).
        std::optional<std::uint64_t> cgroupId;
        /// Off ran, put the nice back and left only the weight to put back (#3332): the next on records the nice it
        /// finds, keeping the weight still owed.
        bool turnedOff = false;
    };
    using Key = std::pair<std::int32_t, std::uint64_t>; ///< PID and start time
    std::mutex mutex;                                   ///< Held only to read or change the two below, never over a call
    std::map<Key, Before> byProcess;
    /// The processes a setEfficiencyMode() is changing now (#3330): an on and an off from two objects sharing this
    /// memory would otherwise interleave their read, change and record, and lose what on found. A second one for the
    /// same process meanwhile is refused rather than waited for.
    std::set<Key> busy;
};

namespace
{

/// setEfficiencyMode()'s claim on one process in a memory's busy set, given back when it returns (#3330).
class EfficiencyClaim
{
  public:
    EfficiencyClaim(LinuxProcessControl::EfficiencyMemory& memory, LinuxProcessControl::EfficiencyMemory::Key key)
        : m_Memory(memory), m_Key(key)
    {
        const std::scoped_lock lock(m_Memory.mutex);
        m_Held = m_Memory.busy.insert(m_Key).second;
    }
    ~EfficiencyClaim()
    {
        if (m_Held)
        {
            const std::scoped_lock lock(m_Memory.mutex);
            m_Memory.busy.erase(m_Key);
        }
    }
    EfficiencyClaim(const EfficiencyClaim&) = delete;
    EfficiencyClaim& operator=(const EfficiencyClaim&) = delete;
    EfficiencyClaim(EfficiencyClaim&&) = delete;
    EfficiencyClaim& operator=(EfficiencyClaim&&) = delete;

    /// False when another on or off for the process holds it.
    [[nodiscard]] bool held() const noexcept
    {
        return m_Held;
    }

  private:
    LinuxProcessControl::EfficiencyMemory& m_Memory;
    LinuxProcessControl::EfficiencyMemory::Key m_Key;
    bool m_Held = false;
};

} // namespace

LinuxProcessControl::~LinuxProcessControl() = default;
LinuxProcessControl::LinuxProcessControl(LinuxProcessControl&&) noexcept = default;
LinuxProcessControl& LinuxProcessControl::operator=(LinuxProcessControl&&) noexcept = default;

std::shared_ptr<LinuxProcessControl::EfficiencyMemory> LinuxProcessControl::sharedEfficiencyMemory()
{
    static const std::shared_ptr<EfficiencyMemory> shared = makeEfficiencyMemory();
    return shared;
}

std::shared_ptr<LinuxProcessControl::EfficiencyMemory> LinuxProcessControl::makeEfficiencyMemory()
{
    return std::make_shared<EfficiencyMemory>();
}

LinuxProcessControl::LinuxProcessControl() : LinuxProcessControl(realProcessSyscalls(), makeEfficiencyMemory())
{}

LinuxProcessControl::LinuxProcessControl(const LinuxProcessSyscalls& syscalls) : LinuxProcessControl(syscalls, makeEfficiencyMemory())
{}

LinuxProcessControl::LinuxProcessControl(const LinuxProcessSyscalls& syscalls, std::shared_ptr<EfficiencyMemory> efficiency)
    : m_Syscalls(syscalls),
      m_CanSetRealtimeIoPriority(ownProcessCanSetRealtimeIo()),
      m_CanSetRealtimeSchedPolicy(ownProcessCanSetRealtimeSched()),
      m_NoRealtimePids{static_cast<std::int32_t>(::getpid())},
      m_Efficiency(std::move(efficiency))
{
    if (m_Syscalls.currentNice == nullptr)
    {
        m_Syscalls.currentNice = &realCurrentNice;
    }
    if (m_Syscalls.procCgroup == nullptr)
    {
        m_Syscalls.procCgroup = &realProcCgroup;
    }
    if (m_Syscalls.readCgroupFile == nullptr)
    {
        m_Syscalls.readCgroupFile = &realReadCgroupFile;
    }
    if (m_Syscalls.writeCgroupFile == nullptr)
    {
        m_Syscalls.writeCgroupFile = &realWriteCgroupFile;
    }
    if (m_Syscalls.cgroupIdentity == nullptr)
    {
        m_Syscalls.cgroupIdentity = &realCgroupIdentity;
    }
    if (m_Syscalls.writeCgroupFileOf == nullptr)
    {
        m_Syscalls.writeCgroupFileOf = &realWriteCgroupFileOf;
    }
    // A table that fakes only the kernel calls keeps the real thread listing (see LinuxProcessSyscalls).
    if (m_Syscalls.listThreads == nullptr)
    {
        m_Syscalls.listThreads = &realListThreads;
    }
    if (m_Syscalls.isThreadOf == nullptr)
    {
        m_Syscalls.isThreadOf = &realIsThreadOf;
    }
    if (m_Syscalls.pidfdGetfd == nullptr)
    {
        m_Syscalls.pidfdGetfd = &realPidfdGetfd;
    }
    if (m_Syscalls.shutdownSocket == nullptr)
    {
        m_Syscalls.shutdownSocket = &realShutdownSocket;
    }
    if (m_Syscalls.socketDescriptor == nullptr)
    {
        m_Syscalls.socketDescriptor = &realSocketDescriptor;
    }
}

ProcessActionCapabilities LinuxProcessControl::actionCapabilities() const
{
    // No system call trace or dump files here: those start other programs, which LinuxProcessActions
    // adds (#2640).
    return {
        .canTerminate = true,
        .canKill = true,
        .canStop = true,
        .canContinue = true,
        .canSendSignals = true, // HUP, INT, QUIT, USR1, USR2 (#3063)
        .canSetPriority = true,
        .canSetIoPriority = true, // ioprio_set(2) (#803)
        .canSetRealtimeIoPriority = m_CanSetRealtimeIoPriority,
        .canSetAffinity = m_Syscalls.setAffinity != nullptr,                                            // sched_setaffinity(2) (#1887)
        .canSetSchedPolicy = m_Syscalls.setScheduler != nullptr,                                        // sched_setscheduler(2) (#3086)
        .canSetRealtimeSchedPolicy = m_Syscalls.setScheduler != nullptr && m_CanSetRealtimeSchedPolicy, // CAP_SYS_NICE (#3219)
        .syscallTrace = SyscallTraceAvailability::Unsupported,
        .canCreateDump = false,
        .canSetEfficiencyMode = true, // nice 19 and a low cgroup cpu.weight (#3202)
        .canCloseConnections = true,  // pidfd_getfd() and shutdown() (#3066)
    };
}

ProcessActionResult LinuxProcessControl::terminate(const ProcessTarget& target)
{
    return sendSignalNumber(target, SIGTERM, "SIGTERM");
}

ProcessActionResult LinuxProcessControl::kill(const ProcessTarget& target)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGKILL is provided by <signal.h>
    return sendSignalNumber(target, SIGKILL, "SIGKILL");
}

ProcessActionResult LinuxProcessControl::stop(const ProcessTarget& target)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGSTOP is provided by <signal.h>
    return sendSignalNumber(target, SIGSTOP, "SIGSTOP");
}

ProcessActionResult LinuxProcessControl::resume(const ProcessTarget& target)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGCONT is provided by <signal.h>
    return sendSignalNumber(target, SIGCONT, "SIGCONT");
}

ProcessActionResult LinuxProcessControl::sendSignal(const ProcessTarget& target, ProcessSignal signal)
{
    // NOLINTBEGIN(misc-include-cleaner) - the SIG* constants are provided by <signal.h>
    switch (signal)
    {
    case ProcessSignal::Hangup:
        return sendSignalNumber(target, SIGHUP, "SIGHUP");
    case ProcessSignal::Interrupt:
        return sendSignalNumber(target, SIGINT, "SIGINT");
    case ProcessSignal::Quit:
        return sendSignalNumber(target, SIGQUIT, "SIGQUIT");
    case ProcessSignal::User1:
        return sendSignalNumber(target, SIGUSR1, "SIGUSR1");
    case ProcessSignal::User2:
        return sendSignalNumber(target, SIGUSR2, "SIGUSR2");
    }
    // NOLINTEND(misc-include-cleaner)
    return ProcessActionResult::error("Unknown signal");
}

namespace
{

/// reniceThreads() against /proc and setpriority(2), through the syscall table.
[[nodiscard]] std::expected<PriorityChange, std::error_code>
setPriorityOfEveryThread(const LinuxProcessSyscalls& syscalls, int32_t pid, int32_t nice)
{
    return reniceThreads(
        pid,
        syscalls.listThreads,
        [&syscalls, nice](id_t tid) -> int
        {
            // setpriority() returns 0 on success and -1 on error (per POSIX).
            return syscalls.setPriority(tid, nice) == 0 ? 0 : errno;
        },
        syscalls.isThreadOf);
}

/// reniceThreads() against /proc and ioprio_set(2), through the syscall table. I/O priority is per thread on Linux, as
/// nice is: ioprio_set(IOPRIO_WHO_PROCESS, pid) alone changes only the main thread (#803, as #1104).
[[nodiscard]] std::expected<PriorityChange, std::error_code>
setIoPriorityOfEveryThread(const LinuxProcessSyscalls& syscalls, int32_t pid, int ioprio)
{
    return reniceThreads(
        pid,
        syscalls.listThreads,
        [&syscalls, ioprio](id_t tid) -> int { return syscalls.ioprioSet(tid, ioprio) == 0 ? 0 : errno; },
        syscalls.isThreadOf);
}

/// How the shared reporting below names the change: "priority" or "I/O priority".
struct ChangeWording
{
    std::string_view lower;   ///< Mid-sentence, e.g. "its priority was being changed".
    std::string_view leading; ///< At the start of a sentence, e.g. "Priority changed for".
};

constexpr ChangeWording NICE_WORDING{.lower = "priority", .leading = "Priority"};
constexpr ChangeWording IO_WORDING{.lower = "I/O priority", .leading = "I/O priority"};
constexpr ChangeWording AFFINITY_WORDING{.lower = "CPU affinity", .leading = "CPU affinity"};
constexpr ChangeWording SCHED_POLICY_WORDING{.lower = "scheduling policy", .leading = "Scheduling policy"};

/// The error for a failed first listing of the target's threads, for either change.
[[nodiscard]] ProcessActionResult threadListError(const ProcessTarget& target, std::error_code listError, ChangeWording wording)
{
    std::string errorMsg = (listError == std::errc::no_such_file_or_directory || listError == std::errc::no_such_process)
                             ? std::string("Process not found - may have already exited")
                             : std::format("Can't list the threads of process {}: {}", target.pid, listError.message());
    spdlog::warn("Failed to set {} for PID {}: {}", wording.lower, target.pid, errorMsg);
    return ProcessActionResult::error(std::move(errorMsg));
}

/// The outcome of a per-thread change (nice or I/O priority) to @p target, made through
/// reniceThreads() after the identity check; @p pidfd is the one opened before that check, and
/// @p firstErrorMessage the platform message for change.firstError, shown if any thread failed.
/// @p setting names the value set, for the success log: "nice=5", or "class=idle, level=0".
///
/// Once any thread was changed, it first confirms the target survived the change: if it exited
/// meanwhile, the change may have reached another process, which matters more than which threads
/// failed (#1228 review). Then exited workers, an incomplete relisting and threads that kept starting
/// are each reported, and only a change that reached every thread is a success.
[[nodiscard]] ProcessActionResult reportThreadChange(const LinuxProcessSyscalls& syscalls,
                                                     const PriorityChange& change,
                                                     const FdGuard& pidfd,
                                                     const ProcessTarget& target,
                                                     ChangeWording wording,
                                                     std::string_view setting,
                                                     std::string firstErrorMessage)
{
    if (change.changed > 0 || change.unconfirmed > 0)
    {
        // A pidfd polls readable once its process exits. Unlike signal 0, which needs kill rights
        // (same UID or CAP_KILL), this needs no permission over the process, so a change made to
        // another user's process with only CAP_SYS_NICE is not reported as unconfirmed (#1483).
        // A failing poll() leaves the change unconfirmed: it gives no evidence the target still
        // held the PID.
        const int exited = pidfdExited(syscalls, pidfd);
        if (exited != 0)
        {
            const int probeErr = errno;
            std::string errorMsg =
                (exited > 0)
                    ? std::format("Process {} exited while its {} was being changed; the change may have reached a different process",
                                  target.pid,
                                  wording.lower)
                    : std::format("{} was set, but it could not be confirmed that process {} still held its PID: {}",
                                  wording.leading,
                                  target.pid,
                                  std::system_category().message(probeErr));
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.unconfirmed > 0)
        {
            std::string errorMsg =
                std::format("{} changed for {} threads, but {} thread(s) of process {} exited during the change; it may have reached "
                            "another process",
                            wording.leading,
                            change.changed,
                            change.unconfirmed,
                            target.pid);
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.relistError)
        {
            std::string errorMsg =
                std::format("{} changed for {} threads, but the threads of process {} couldn't be listed again ({}); any started since may "
                            "still have the old {}",
                            wording.leading,
                            change.changed,
                            target.pid,
                            change.relistError.message(),
                            wording.lower);
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.threadsKeptStarting)
        {
            std::string errorMsg =
                std::format("{} changed for {} threads, but process {} kept starting new ones; some may still have the old {}",
                            wording.leading,
                            change.changed,
                            target.pid,
                            wording.lower);
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.failed == 0)
        {
            spdlog::info("Successfully set {} ({}) for PID {} ({} threads)", wording.lower, setting, target.pid, change.changed);
            return ProcessActionResult::ok();
        }
    }

    std::string errorMsg = std::move(firstErrorMessage);
    if (change.changed > 0 || change.unconfirmed > 0)
    {
        errorMsg = std::format(
            "{} changed for only {} of {} threads. {}", wording.leading, change.changed, change.changed + change.failed, errorMsg);
    }

    spdlog::warn("Failed to set {} for PID {}: {}", wording.lower, target.pid, errorMsg);
    // A thread refused for want of permission (another user's process, or a raise without
    // CAP_SYS_NICE): the privileged helper may make the whole change (#2308).
    return isPermissionError(change.firstError) ? ProcessActionResult::denied(std::move(errorMsg))
                                                : ProcessActionResult::error(std::move(errorMsg));
}

} // namespace

ProcessActionResult LinuxProcessControl::setPriority(const ProcessTarget& target, int32_t nice)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (const std::optional<std::string> refused = schedulingChangeRefused(target.pid); refused.has_value())
    {
        return ProcessActionResult::error(*refused);
    }

    // Clamp nice value to valid range
    const int32_t clampedNice = Domain::Priority::clampNice(nice);

    spdlog::debug("Setting priority (nice={}) for PID {}", clampedNice, target.pid);

    // setpriority(2) has no pidfd form, so it can only be aimed at the PID. A pidfd still closes
    // the gap, after the fact: opened before the identity check as in sendSignalNumber(), it lets us
    // ask afterwards whether the target is still there. A PID is freed only when its process is
    // reaped, so if the target is still there after the call -- running or a zombie -- the PID
    // never changed hands and the call reached it.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Failed to set priority for PID {}: {}", target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not setting priority for PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    // Nice is a per-thread attribute on Linux (setpriority(2), NOTES): PRIO_PROCESS with the PID
    // changes only the main thread, so a multithreaded compiler or browser kept nearly all of its
    // work at the old priority while the UI reported success (#1104). Renice every thread, as
    // Windows' SetPriorityClass changes the whole process.
    const auto result = setPriorityOfEveryThread(m_Syscalls, target.pid, clampedNice);
    if (!result.has_value())
    {
        return threadListError(target, result.error(), NICE_WORDING);
    }
    return reportThreadChange(m_Syscalls,
                              *result,
                              pidfd,
                              target,
                              NICE_WORDING,
                              std::format("nice={}", clampedNice),
                              priorityErrorMessage(result->firstError, clampedNice, target.pid));
}

ProcessActionResult LinuxProcessControl::setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (static_cast<int>(ioClass) > IoPrio::MAX_CLASS)
    {
        return ProcessActionResult::error("Invalid I/O priority class");
    }
    if (const std::optional<std::string> refused = schedulingChangeRefused(target.pid); refused.has_value())
    {
        return ProcessActionResult::error(*refused);
    }

    // encode() holds the level to 0-7, and to 0 for the classes without levels.
    const IoPriority priority{.ioClass = ioClass, .level = IoPrio::classHasLevels(ioClass) ? Domain::Priority::clampIoLevel(level) : 0};
    const int ioprio = IoPrio::encode(priority);
    const std::string setting = std::format("class={}, level={}", IoPrio::className(ioClass), priority.level);
    spdlog::debug("Setting I/O priority ({}) for PID {}", setting, target.pid);

    // As setPriority(): ioprio_set(2) has no pidfd form either, so the pidfd opened before the
    // identity check is asked afterwards whether the target survived the call.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Failed to set I/O priority for PID {}: {}", target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not setting I/O priority for PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    // I/O priority is per thread, as nice is, so every thread is set (#803, as #1104).
    const auto result = setIoPriorityOfEveryThread(m_Syscalls, target.pid, ioprio);
    if (!result.has_value())
    {
        return threadListError(target, result.error(), IO_WORDING);
    }
    return reportThreadChange(
        m_Syscalls, *result, pidfd, target, IO_WORDING, setting, ioPriorityErrorMessage(result->firstError, ioClass, target.pid));
}

namespace
{

/// A CPU_ALLOC()ed cpu_set_t, freed with CPU_FREE().
struct CpuSetDeleter
{
    void operator()(cpu_set_t* set) const noexcept
    {
        CPU_FREE(set);
    }
};

} // namespace

ProcessActionResult LinuxProcessControl::setAffinity(const ProcessTarget& target, const CpuAffinity& cpus)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (cpus.empty())
    {
        return ProcessActionResult::error("Choose at least one CPU");
    }
    if (const std::optional<std::string> refused = schedulingChangeRefused(target.pid); refused.has_value())
    {
        return ProcessActionResult::error(*refused);
    }
    if (m_Syscalls.setAffinity == nullptr)
    {
        return ProcessActionResult::error("Setting CPU affinity isn't available on this system");
    }

    // A dynamically sized set (CPU_ALLOC), so a CPU at 1024 or above -- past the fixed cpu_set_t -- is
    // kept: the affinity read from Cpus_allowed_list has no width limit either (#1247).
    const std::size_t cpuCount = cpus.words().size() * CpuAffinity::BITS_PER_WORD;
    const std::unique_ptr<cpu_set_t, CpuSetDeleter> set(CPU_ALLOC(cpuCount));
    if (!set)
    {
        return ProcessActionResult::error("Out of memory");
    }
    const std::size_t setSize = CPU_ALLOC_SIZE(cpuCount);
    CPU_ZERO_S(setSize, set.get());
    for (std::size_t cpu = 0; cpu < cpuCount; ++cpu)
    {
        if (cpus.test(cpu))
        {
            CPU_SET_S(cpu, setSize, set.get());
        }
    }
    const std::string setting = std::format("{} CPUs", cpus.count());
    spdlog::debug("Setting CPU affinity ({}) for PID {}", setting, target.pid);

    // As setPriority(): sched_setaffinity(2) has no pidfd form, so the pidfd opened before the
    // identity check is asked afterwards whether the target survived the call.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Failed to set CPU affinity for PID {}: {}", target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not setting CPU affinity for PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    // Affinity is per thread, as nice is: sched_setaffinity(2) with the PID changes only the main
    // thread, so every thread is set, threads started meanwhile included (#1887, as #1104).
    const cpu_set_t* const cpuSet = set.get();
    const auto result = reniceThreads(
        target.pid,
        m_Syscalls.listThreads,
        [this, setSize, cpuSet](id_t tid) -> int { return m_Syscalls.setAffinity(tid, setSize, cpuSet) == 0 ? 0 : errno; },
        m_Syscalls.isThreadOf);
    if (!result.has_value())
    {
        return threadListError(target, result.error(), AFFINITY_WORDING);
    }
    return reportThreadChange(
        m_Syscalls, *result, pidfd, target, AFFINITY_WORDING, setting, affinityErrorMessage(result->firstError, target.pid));
}

ProcessActionResult LinuxProcessControl::setSchedPolicy(const ProcessTarget& target,
                                                        SchedPolicy policy,
                                                        std::int32_t rtPriority,
                                                        RealtimeThreads realtimeThreads)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    // Normal, Batch and Idle at priority 0 (#3086), FIFO and round robin at 1 to 99 (#3219): Deadline and any other
    // pair stop here, before sched_setscheduler(2), as the broker protocol's decoder stops them for the root helper.
    const int linuxPolicy = settableSchedPolicyToLinux(policy);
    if (linuxPolicy < 0)
    {
        return ProcessActionResult::error("Only the Normal, Batch, Idle, FIFO and round robin scheduling policies can be set");
    }
    if (!isValidSchedPolicySetting(policy, rtPriority))
    {
        return ProcessActionResult::error(
            isRealtimeSchedPolicy(policy)
                ? std::format("A real-time priority runs from {} to {}, not {}",
                              MIN_REALTIME_SCHED_PRIORITY,
                              MAX_REALTIME_SCHED_PRIORITY,
                              rtPriority)
                : std::format("The {} policy takes no real-time priority (asked for {})", schedPolicyText(policy, 0).view(), rtPriority));
    }
    if (const std::optional<std::string> refused = schedulingChangeRefused(target.pid); refused.has_value())
    {
        return ProcessActionResult::error(*refused);
    }
    // Never real time for this process or the helper's client, TaskSmack (#3219): its UI thread, or the root helper,
    // spinning at a real-time priority could starve the desktop the user needs to undo it. Compared by thread group,
    // since one of the process's thread IDs names the same process and every one of its threads would be changed.
    if (isRealtimeSchedPolicy(policy))
    {
        const std::int32_t process = threadGroupOf(target.pid);
        if (std::ranges::find(m_NoRealtimePids, process) != m_NoRealtimePids.end())
        {
            const std::string self =
                process == target.pid
                    ? std::format("PID {} is TaskSmack (or its helper), which never runs under a real-time policy", target.pid)
                    : std::format("PID {} is a thread of PID {}, TaskSmack (or its helper), which never runs under a real-time policy",
                                  target.pid,
                                  process);
            spdlog::info("Not setting scheduling policy: {}", self);
            return ProcessActionResult::error(self);
        }
    }
    if (m_Syscalls.setScheduler == nullptr)
    {
        return ProcessActionResult::error("Setting the scheduling policy isn't available on this system");
    }

    const std::string setting = std::format("policy={}", schedPolicyText(policy, static_cast<std::uint32_t>(rtPriority)).view());
    spdlog::debug("Setting scheduling policy ({}) for PID {}", setting, target.pid);

    // As setPriority(): sched_setscheduler(2) has no pidfd form, so the pidfd opened before the
    // identity check is asked afterwards whether the target survived the call.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Failed to set scheduling policy for PID {}: {}", target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not setting scheduling policy for PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    // Normal, Batch or Idle changes real-time threads only when the dialog warned that the process runs real time (it
    // showed the main thread's policy, from the snapshot): RealtimeThreads::Change. Otherwise every real-time thread
    // keeps its policy, the main thread included: an audio worker under a Normal main thread, or a process that turned
    // real time after the dialog opened, is never taken off real time without that warning. A request for real time
    // (FIFO, RR, #3219) is the user's explicit, confirmed choice of real time for the process, so every thread gets
    // it, the workers included.
    const bool keepRealtimeWorkers = realtimeThreads != RealtimeThreads::Change && !isRealtimeSchedPolicy(policy);
    const auto isRealtime = [](int linux) noexcept
    {
        const int base = linux & ~SCHED_RESET_ON_FORK;
        return base == SCHED_FIFO || base == SCHED_RR || base == SCHED_DEADLINE;
    };

    // The policy is per thread, as nice is: sched_setscheduler(2) with the PID changes only the main
    // thread, so every thread is set, threads started meanwhile included (#3086, as #1104).
    std::size_t keptRealtime = 0;
    const auto result = reniceThreads(
        target.pid,
        m_Syscalls.listThreads,
        [this, linuxPolicy, rtPriority, keepRealtimeWorkers, &isRealtime, &keptRealtime](id_t tid) -> int
        {
            // A thread's SCHED_RESET_ON_FORK is kept (rtkit sets it on the real-time threads it grants): clearing it
            // would change what the thread's children start with, and the kernel refuses an unprivileged caller
            // that tries, with EPERM. A policy that can't be read isn't guessed at: that thread fails with why.
            int current = 0;
            if (m_Syscalls.getScheduler != nullptr)
            {
                current = m_Syscalls.getScheduler(tid);
                if (current < 0)
                {
                    return errno;
                }
            }
            if (keepRealtimeWorkers && isRealtime(current))
            {
                ++keptRealtime;
                return 0; // left as it is: see above
            }
            return m_Syscalls.setScheduler(tid, linuxPolicy | (current & SCHED_RESET_ON_FORK), rtPriority) == 0 ? 0 : errno;
        },
        m_Syscalls.isThreadOf);
    if (!result.has_value())
    {
        return threadListError(target, result.error(), SCHED_POLICY_WORDING);
    }
    ProcessActionResult outcome = reportThreadChange(m_Syscalls,
                                                     *result,
                                                     pidfd,
                                                     target,
                                                     SCHED_POLICY_WORDING,
                                                     setting,
                                                     schedPolicyErrorMessage(result->firstError, policy, rtPriority, target.pid));
    if (outcome.success && keptRealtime > 0)
    {
        outcome.successNote = std::format(
            "{} real-time thread{} kept {} policy", keptRealtime, keptRealtime == 1 ? "" : "s", keptRealtime == 1 ? "its" : "their");
    }
    return outcome;
}

CpuAffinityLayout LinuxProcessControl::affinityLayout() const
{
    // No processor groups on Linux. The online CPUs and, on a hybrid CPU, each one's class, read
    // when the Set affinity dialog opens (#1881), as the system probe reads them (#809).
    const std::filesystem::path cpuSysfsRoot("/sys/devices/system/cpu");
    CpuAffinityLayout layout;
    if (const auto online = LinuxCpuDetails::Detail::readFile(cpuSysfsRoot / "online"); online.has_value())
    {
        layout.online = CpuAffinity::fromCpuList(*online).value_or(CpuAffinity{});
    }
    layout.efficiencyClassByCpu = LinuxCpuDetails::readEfficiencyClasses(cpuSysfsRoot, std::nullopt);
    return layout;
}

IoPriorityReadResult LinuxProcessControl::getIoPriority(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return std::unexpected(std::string("Invalid PID"));
    }

    // Checked as an action is, so a reused PID's value is never shown for the target: the pidfd is
    // opened before the identity check and asked after the read whether the target still holds the
    // PID; if it does, the value read was the target's.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        return std::unexpected(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        return std::unexpected(std::move(identity.errorMessage));
    }

    // The main thread's value: the one ionice(1) shows, and the one setIoPriority() sets with the rest.
    const int raw = m_Syscalls.ioprioGet(static_cast<pid_t>(target.pid));
    if (raw < 0)
    {
        const int err = errno;
        if (err == ESRCH)
        {
            return std::unexpected(std::string("Process not found - may have already exited"));
        }
        return std::unexpected(
            std::format("Can't read the I/O priority of process {}: {}", target.pid, std::system_category().message(err)));
    }
    // Reading needs no privilege over the process, so neither may this check: another user's process
    // refuses signal 0 with EPERM while it is very much alive.
    const int exited = pidfdExited(m_Syscalls, pidfd);
    if (exited < 0)
    {
        return std::unexpected(std::format("Can't confirm that process {} still held its PID while its I/O priority was read: {}",
                                           target.pid,
                                           std::system_category().message(errno)));
    }
    if (exited > 0)
    {
        return std::unexpected(std::format("Process {} exited while its I/O priority was being read", target.pid));
    }

    const std::optional<IoPriority> decoded = IoPrio::decode(raw);
    if (!decoded.has_value())
    {
        return std::unexpected(std::format("Process {} has an I/O priority TaskSmack does not recognise ({})", target.pid, raw));
    }
    return *decoded;
}

ProcessActionResult LinuxProcessControl::closeConnection(const ProcessTarget& target, std::uint64_t socketInode)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (socketInode == 0)
    {
        return ProcessActionResult::error("No process holds this socket, so there is nothing to close");
    }
    // As every other action (#3295): init's and kernel threads' sockets are left alone, through the helper too.
    if (const std::optional<ProtectedProcess> reason = protectedProcess(target.pid); reason.has_value())
    {
        return ProcessActionResult::error(
            *reason == ProtectedProcess::Init
                ? std::string("PID 1 is init, whose connections TaskSmack doesn't close")
                : std::format("PID {} is a kernel thread, whose connections TaskSmack doesn't close", target.pid));
    }
    // As sendSignalNumber(): the pidfd first, then the identity check, so the descriptor copied below can only
    // come from the target, never from a process that took its PID since.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);
    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        return identity;
    }
    const std::expected<int, int> descriptor = m_Syscalls.socketDescriptor(target.pid, socketInode);
    if (!descriptor.has_value())
    {
        if (descriptor.error() == ENOENT)
        {
            return ProcessActionResult::error(std::format("PID {} no longer holds this socket", target.pid));
        }
        const std::string message = std::system_category().message(descriptor.error());
        return isPermissionError(descriptor.error()) ? ProcessActionResult::denied(message) : ProcessActionResult::error(message);
    }
    // A copy of the process's descriptor, in this process: shutting it down acts on the one socket both refer to,
    // and the process's own descriptor stays open, reporting an error on its next use.
    const int copy = m_Syscalls.pidfdGetfd(pidfd.get(), *descriptor);
    if (copy < 0)
    {
        const int err = errno;
        if (err == ENOSYS)
        {
            return ProcessActionResult::error("Closing a connection needs Linux 5.6 or later (pidfd_getfd)");
        }
        const std::string message = std::system_category().message(err);
        // EPERM: no ptrace access to another user's process, which the helper has (CAP_SYS_PTRACE).
        return isPermissionError(err) ? ProcessActionResult::denied(message) : ProcessActionResult::error(message);
    }
    const FdGuard copyGuard(copy);
    // The copy is whatever the process held at that number when it was taken: if it closed the socket since and
    // a new one took the number, that one isn't shut down in its place (#3295).
    struct stat copied{};
    if (::fstat(copy, &copied) != 0 || !S_ISSOCK(copied.st_mode) || copied.st_ino != socketInode)
    {
        return ProcessActionResult::error(std::format("PID {} no longer holds this socket", target.pid));
    }
    // A listening socket has no connection to close, and shutting one down stops it accepting (a TCP listener is
    // disconnected, not refused with ENOTCONN), so it's refused whatever the view asked; and so is one whose
    // listening can't be read, rather than shut down unchecked.
    int listening = 0;
    socklen_t listeningSize = sizeof(listening);
    // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET and SO_ACCEPTCONN are provided by <sys/socket.h>
    if (::getsockopt(copy, SOL_SOCKET, SO_ACCEPTCONN, &listening, &listeningSize) != 0)
    {
        return ProcessActionResult::error(
            std::format("Couldn't tell whether this socket is listening: {}", std::system_category().message(errno)));
    }
    if (listening != 0)
    {
        return ProcessActionResult::error("This socket is listening for connections, so there is no connection to close");
    }
    if (m_Syscalls.shutdownSocket(copy) != 0)
    {
        const int err = errno;
        // ENOTCONN: an unconnected socket, which has no connection to shut down.
        return ProcessActionResult::error(err == ENOTCONN ? std::string("This socket isn't connected, so there is no connection to close")
                                                          : std::system_category().message(err));
    }
    spdlog::info("Closed socket {} of PID {}", socketInode, target.pid);
    return ProcessActionResult::ok();
}

ProcessActionResult LinuxProcessControl::sendSignalNumber(const ProcessTarget& target, int signal, std::string_view signalName) const
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }

    spdlog::debug("Sending {} to PID {}", signalName, target.pid);

    // The pidfd is opened *before* the start time is read. Whatever process it refers to held the
    // PID at that moment; if the start time read afterwards is the target's, that process is the
    // target, because the target is older than any process that could take the PID after it. From
    // then on the pidfd keeps referring to it, so a reuse of the PID cannot redirect the signal.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Failed to send {} to PID {}: {}", signalName, target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not sending {} to PID {}: {}", signalName, target.pid, identity.errorMessage);
        return identity;
    }
    if (const std::optional<std::string> nothing = signalWouldDoNothing(target.pid); nothing.has_value())
    {
        spdlog::info("Not sending {}: {}", signalName, *nothing);
        return ProcessActionResult::error(*nothing);
    }
    // Suspending itself would leave this process stopped with nothing to resume it: TaskSmack frozen, or
    // the root helper stopped for good, which the user can neither resume nor end (#2641).
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGSTOP is provided by <signal.h>
    if (signal == SIGSTOP && target.pid == ::getpid())
    {
        const std::string self = std::format("PID {} is this process, which would have nothing to resume it once suspended", target.pid);
        spdlog::info("Not sending {}: {}", signalName, self);
        return ProcessActionResult::error(self);
    }

    const int sendResult = sendThroughPidfd(m_Syscalls, pidfd, signal);

    if (sendResult == 0)
    {
        spdlog::info("Successfully sent {} to PID {}", signalName, target.pid);
        return ProcessActionResult::ok();
    }

    const int err = errno;
    const std::string errorMsg = signalErrorMessage(err);
    spdlog::warn("Failed to send {} to PID {}: {}", signalName, target.pid, errorMsg);
    // EPERM: another user's process, which the privileged helper may signal (#2308).
    return isPermissionError(err) ? ProcessActionResult::denied(errorMsg) : ProcessActionResult::error(errorMsg);
}

namespace
{

/// Why Efficiency mode left a process's cgroup weight as it was, for the success line; empty when it lowered it.
[[nodiscard]] std::string lowerCgroupWeight(const LinuxProcessSyscalls& syscalls,
                                            std::int32_t pid,
                                            const std::optional<std::string>& cgroup,
                                            std::optional<std::uint64_t> identity,
                                            std::optional<int>& weightBefore)
{
    namespace Efficiency = LinuxEfficiency;
    if (!cgroup.has_value() || cgroup->empty())
    {
        return "only its priority was lowered: it isn't in a cgroup of its own";
    }
    const std::optional<std::string> procs = syscalls.readCgroupFile(*cgroup, Efficiency::CGROUP_PROCS_FILE);
    if (!procs.has_value() || !Efficiency::onlyProcessIn(*procs, pid))
    {
        return "only its priority was lowered: it shares its cgroup with other processes, which a lower CPU weight would slow too";
    }
    const std::optional<std::string> weightText = syscalls.readCgroupFile(*cgroup, Efficiency::CPU_WEIGHT_FILE);
    const std::optional<int> weight = weightText.has_value() ? Efficiency::parseCpuWeight(*weightText) : std::nullopt;
    if (!weight.has_value())
    {
        return "only its priority was lowered: its cgroup has no CPU controller";
    }
    if (*weight == Efficiency::MIN_CPU_WEIGHT)
    {
        return {};
    }
    const std::string minimum = std::to_string(Efficiency::MIN_CPU_WEIGHT);
    // Into the cgroup whose identity on read, when it read one: one replaced since gets nothing (#3331).
    const int written = identity.has_value() ? syscalls.writeCgroupFileOf(*cgroup, *identity, Efficiency::CPU_WEIGHT_FILE, minimum)
                                             : syscalls.writeCgroupFile(*cgroup, Efficiency::CPU_WEIGHT_FILE, minimum);
    if (written == ESTALE)
    {
        return "only its priority was lowered: its cgroup was replaced";
    }
    if (const int error = written; error != 0)
    {
        return std::format("only its priority was lowered: its cgroup's CPU weight can't be changed ({})",
                           std::system_category().message(error));
    }
    weightBefore = *weight;
    return {};
}

/// What restoreCgroupWeight() did: why it couldn't put the weight back, for the success line (empty when it did, or
/// had nothing to do), and whether a weight on lowered is still to be put back (#3332), so off keeps its memory.
struct WeightRestore
{
    std::string note;
    bool unfinished = false;
};

/// Puts back the cgroup weight Efficiency mode lowered: @p rememberedWeight when it lowered it, else the default when
/// the process is alone in a cgroup at the minimum. A weight on lowered that can't be found, read or written is said,
/// and left unfinished (#3332).
[[nodiscard]] WeightRestore restoreCgroupWeight(const LinuxProcessSyscalls& syscalls,
                                                std::int32_t pid,
                                                const std::optional<std::string>& cgroup,
                                                bool rememberedProcess,
                                                std::optional<int> rememberedWeight,
                                                std::optional<std::uint64_t> identity)
{
    namespace Efficiency = LinuxEfficiency;
    const bool lowered = rememberedProcess && rememberedWeight.has_value(); // on lowered it, so it is ours to put back
    if (rememberedProcess && !rememberedWeight.has_value())
    {
        return {};
    }
    if (!cgroup.has_value() || cgroup->empty())
    {
        if (lowered)
        {
            return {.note = "its cgroup's CPU weight wasn't put back: its cgroup can't be read", .unfinished = true};
        }
        return {};
    }
    if (!rememberedProcess)
    {
        const std::optional<std::string> procs = syscalls.readCgroupFile(*cgroup, Efficiency::CGROUP_PROCS_FILE);
        if (!procs.has_value() || !Efficiency::onlyProcessIn(*procs, pid))
        {
            return {}; // a shared cgroup's weight isn't this process's to put back
        }
    }
    const std::optional<std::string> weightText = syscalls.readCgroupFile(*cgroup, Efficiency::CPU_WEIGHT_FILE);
    const std::optional<int> current = weightText.has_value() ? Efficiency::parseCpuWeight(*weightText) : std::nullopt;
    if (!current.has_value() && lowered)
    {
        return {.note = "its cgroup's CPU weight wasn't put back: it can't be read", .unfinished = true};
    }
    const std::optional<int> restore = Efficiency::weightToRestore(current, rememberedWeight);
    if (!restore.has_value())
    {
        return {}; // not the minimum on set: someone changed it since, and it is theirs
    }
    const std::string value = std::to_string(*restore);
    // Into the cgroup on lowered, checked on the directory written through: one replaced since gets nothing (#3331).
    const int written = identity.has_value() ? syscalls.writeCgroupFileOf(*cgroup, *identity, Efficiency::CPU_WEIGHT_FILE, value)
                                             : syscalls.writeCgroupFile(*cgroup, Efficiency::CPU_WEIGHT_FILE, value);
    if (written == ESTALE)
    {
        return {.note = "its old cgroup's CPU weight wasn't put back: that cgroup is gone", .unfinished = false};
    }
    if (const int error = written; error != 0)
    {
        return {
            .note = std::format("its cgroup's CPU weight is still the minimum: it can't be changed ({})",
                                std::system_category().message(error)),
            .unfinished = lowered,
        };
    }
    return {};
}

} // namespace

ProcessActionResult LinuxProcessControl::setEfficiencyMode(const ProcessTarget& target, bool on)
{
    namespace Efficiency = LinuxEfficiency;
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    const std::pair<std::int32_t, std::uint64_t> key{target.pid, target.startTimeTicks};
    const EfficiencyClaim claim(*m_Efficiency, key); // one on or off at a time per process over the shared memory (#3330)
    if (!claim.held())
    {
        return ProcessActionResult::error("Efficiency mode is already being changed for this process");
    }
    std::optional<EfficiencyMemory::Before> remembered;
    {
        const std::scoped_lock lock(m_Efficiency->mutex);
        if (const auto found = m_Efficiency->byProcess.find(key); found != m_Efficiency->byProcess.end())
        {
            remembered = found->second;
        }
    }
    const std::optional<std::int32_t> niceBefore = m_Syscalls.currentNice(target.pid);

    // The nice: nice 19 on, the remembered one (or 0) off unless someone changed it since. setPriority() makes
    // every check a priority change makes (PID 1 and kernel threads refused, the start time confirmed, every
    // thread), and a permission refusal from it is one the helper can finish (#2308).
    std::optional<std::int32_t> nice = Efficiency::EFFICIENCY_NICE;
    if (!on)
    {
        if (remembered.has_value() && remembered->turnedOff)
        {
            // An earlier off already put the nice back (#3332): only the weight is still owed, so the priority is left
            // as it is (a process that was at nice 19 before on is still at its own nice) and only the identity checked.
            nice = std::nullopt;
        }
        else
        {
            const std::optional<std::int32_t> rememberedNice = remembered.has_value() ? std::optional(remembered->nice) : std::nullopt;
            nice = niceBefore.has_value() ? Efficiency::niceToRestore(*niceBefore, rememberedNice) : rememberedNice.value_or(0);
        }
    }
    ProcessActionResult changed = nice.has_value() ? setPriority(target, *nice) : verifyIdentity(target);
    if (!changed.success)
    {
        return changed;
    }

    const std::optional<std::string> cgroupText = m_Syscalls.procCgroup(target.pid);
    const std::optional<std::string> cgroup = cgroupText.has_value() ? Efficiency::unifiedCgroupPath(*cgroupText) : std::nullopt;
    if (on)
    {
        std::optional<int> weightBefore;
        // A weight an earlier off still owes another cgroup (#3331): this on lowers no second one, which would leave
        // one of the two unrecorded.
        const bool owedElsewhere =
            remembered.has_value() && remembered->cpuWeight.has_value() && remembered->cgroup.has_value() && remembered->cgroup != cgroup;
        // Which cgroup the path names now, read before anything is lowered (#3331): a weight is only ever lowered
        // with its cgroup's identity recorded, and a record for a cgroup since replaced at the same path is stale.
        std::optional<std::uint64_t> cgroupId;
        bool identityUnknown = false;
        if (!owedElsewhere && cgroup.has_value() && !cgroup->empty())
        {
            const std::expected<std::uint64_t, int> id = m_Syscalls.cgroupIdentity(*cgroup);
            cgroupId = id.has_value() ? std::optional(*id) : std::nullopt;
            identityUnknown = !id.has_value();
        }
        const bool staleRecord = remembered.has_value() && remembered->cgroup == cgroup && remembered->cgroupId.has_value() &&
                                 cgroupId.has_value() && remembered->cgroupId != cgroupId;
        std::string note;
        if (owedElsewhere)
        {
            note = "only its priority was lowered: its earlier cgroup's CPU weight is still to be put back";
        }
        else if (identityUnknown)
        {
            note = "only its priority was lowered: its cgroup can't be checked";
        }
        else
        {
            note = lowerCgroupWeight(m_Syscalls, target.pid, cgroup, cgroupId, weightBefore);
        }
        {
            // Turned on twice, the first time's "before" stays what off puts back. Processes that have exited since
            // their on are forgotten here, so the shared memory doesn't grow for TaskSmack's lifetime (#3330): one
            // whose PID now holds another process (its start time differs) counts as exited. Read outside the lock.
            std::vector<EfficiencyMemory::Key> entries;
            {
                const std::scoped_lock lock(m_Efficiency->mutex);
                for (const auto& entry : m_Efficiency->byProcess)
                {
                    entries.push_back(entry.first);
                }
            }
            // Kept: this process, and any whose exit isn't certain (a /proc read that failed for another reason).
            std::erase_if(entries,
                          [&key](const EfficiencyMemory::Key& entry)
                          {
                              if (entry == key)
                              {
                                  return true;
                              }
                              const std::optional<std::uint64_t> startNow = readStartTicks(entry.first);
                              const bool gone = !startNow.has_value() && procEntryGone(entry.first);
                              return !Efficiency::rememberedProcessExited(startNow, gone, entry.second);
                          });
            const std::scoped_lock lock(m_Efficiency->mutex);
            for (const EfficiencyMemory::Key& exited : entries)
            {
                m_Efficiency->byProcess.erase(exited);
            }

            const auto [entry, added] = m_Efficiency->byProcess.try_emplace(key,
                                                                            EfficiencyMemory::Before{
                                                                                .nice = niceBefore.value_or(0),
                                                                                .cpuWeight = weightBefore,
                                                                                .cgroup = weightBefore.has_value() ? cgroup : std::nullopt,
                                                                                .cgroupId = cgroupId,
                                                                            });
            if (!added && staleRecord && !entry->second.turnedOff)
            {
                // Its cgroup was replaced at the same path (#3331): that cgroup's weight is nothing to put back, the
                // replacement's (if this on lowered it) is; the nice from before the first on is still owed.
                entry->second.cpuWeight = weightBefore;
                entry->second.cgroup = weightBefore.has_value() ? cgroup : std::nullopt;
                entry->second.cgroupId = weightBefore.has_value() ? cgroupId : std::nullopt;
            }
            else if (!added && entry->second.turnedOff)
            {
                // After an off that couldn't put the weight back: the nice is the one found now, and the weight
                // still owed (and its cgroup) is kept, unless this on lowered one itself.
                entry->second.nice = niceBefore.value_or(0);
                if (weightBefore.has_value() || staleRecord)
                {
                    entry->second.cpuWeight = weightBefore;
                    entry->second.cgroup = cgroup;
                    entry->second.cgroupId = cgroupId;
                }
                entry->second.turnedOff = false;
            }
            else if (!added && weightBefore.has_value() && !entry->second.cpuWeight.has_value())
            {
                // An earlier on lowered no weight (its cgroup shared then, or its identity unread) and this one did:
                // recorded now, or off would leave it at the minimum (#3331).
                entry->second.cpuWeight = weightBefore;
                entry->second.cgroup = cgroup;
                entry->second.cgroupId = cgroupId;
            }
        }
        spdlog::info("Efficiency mode on for PID {}{}", target.pid, note.empty() ? "" : ": " + note);
        return ProcessActionResult::ok(std::move(note));
    }
    // The cgroup on lowered, where it lowered one (#3331): a process that has moved since has its old cgroup's weight
    // put back, and its new cgroup's left alone. Also when the process's cgroup can't be read now.
    const bool loweredOne = remembered.has_value() && remembered->cgroup.has_value();
    const bool moved = loweredOne && cgroup.has_value() && *cgroup != *remembered->cgroup;
    if (loweredOne && remembered->cgroupId.has_value())
    {
        const std::expected<std::uint64_t, int> id = m_Syscalls.cgroupIdentity(*remembered->cgroup);
        const bool gone = id.has_value() ? *id != *remembered->cgroupId : id.error() == ENOENT;
        if (gone || !id.has_value())
        {
            // Gone, or another cgroup made at its path since: nothing of on's to put back, so forgotten (#3331). One
            // that can't be checked (no permission now, say) may still be it: kept for another off, its nice spent.
            {
                const std::scoped_lock lock(m_Efficiency->mutex);
                if (gone)
                {
                    m_Efficiency->byProcess.erase(key);
                }
                else if (const auto found = m_Efficiency->byProcess.find(key); found != m_Efficiency->byProcess.end())
                {
                    found->second.turnedOff = true;
                }
            }
            std::string why = gone ? "its old cgroup's CPU weight wasn't put back: that cgroup is gone"
                                   : std::format("its cgroup's CPU weight wasn't put back: its cgroup can't be checked ({})",
                                                 std::system_category().message(id.error()));
            spdlog::info("Efficiency mode off for PID {}: {}", target.pid, why);
            return ProcessActionResult::ok(std::move(why));
        }
    }
    WeightRestore weight = restoreCgroupWeight(m_Syscalls,
                                               target.pid,
                                               loweredOne ? remembered->cgroup : cgroup,
                                               remembered.has_value(),
                                               remembered.has_value() ? remembered->cpuWeight : std::nullopt,
                                               loweredOne ? remembered->cgroupId : std::nullopt);
    if (moved && weight.unfinished)
    {
        // The old cgroup is gone or unreadable: a process that left it won't bring it back, so nothing to retry.
        weight.unfinished = false;
        weight.note = "its old cgroup's CPU weight wasn't put back: it has moved to another cgroup, and the old one can't be changed";
    }
    {
        // A weight still to put back keeps the memory, so turning it off again tries once more (#3332); its nice is
        // spent, which the next on sees.
        const std::scoped_lock lock(m_Efficiency->mutex);
        if (!weight.unfinished)
        {
            m_Efficiency->byProcess.erase(key);
        }
        else if (const auto found = m_Efficiency->byProcess.find(key); found != m_Efficiency->byProcess.end())
        {
            found->second.turnedOff = true;
        }
    }
    spdlog::info("Efficiency mode off for PID {}{}", target.pid, weight.note.empty() ? "" : ": " + weight.note);
    return ProcessActionResult::ok(std::move(weight.note));
}

std::size_t LinuxProcessControl::rememberedEfficiencyCount() const
{
    const std::scoped_lock lock(m_Efficiency->mutex);
    return m_Efficiency->byProcess.size();
}

std::optional<std::int32_t> LinuxProcessControl::efficiencyNiceToRestore(const ProcessTarget& target) const
{
    std::optional<std::int32_t> remembered;
    {
        const std::scoped_lock lock(m_Efficiency->mutex);
        if (const auto found = m_Efficiency->byProcess.find({target.pid, target.startTimeTicks});
            found != m_Efficiency->byProcess.end() && !found->second.turnedOff) // a spent nice isn't one to put back (#3332)
        {
            remembered = found->second.nice;
        }
    }
    const std::optional<std::int32_t> current = remembered.has_value() ? m_Syscalls.currentNice(target.pid) : std::nullopt;
    return current.has_value() ? LinuxEfficiency::niceToRestore(*current, remembered) : std::nullopt;
}

} // namespace Platform
