#include "LinuxProcessActions.h"

#include "DetachedSpawn.h"
#include "FileManagerReveal.h"
#include "LinuxCoreDump.h"
#include "LinuxProcessActionsDetail.h"
#include "LinuxProcessControl.h"
#include "Platform/IProcessActions.h"
#include "PosixGuards.h"
#include "ProcPrivileges.h"
#include "SessionUser.h"
#include "SyscallTrace.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef TASKSMACK_HAS_SDBUS
#include "Platform/Linux/SdBusCall.h"
#include "Platform/Linux/SystemdBus.h"

#include <systemd/sd-bus.h>
#endif

#include <fcntl.h>
#include <features.h>

// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides kill() function, csignal does not
#include <signal.h>
#include <spawn.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
// NOLINTNEXTLINE(misc-include-cleaner) - WNOHANG, WIFEXITED and WEXITSTATUS, which tidy maps elsewhere (see their uses)
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace Platform
{

namespace
{

using LinuxProcessDetail::openPidfd;
using LinuxProcessDetail::PidfdOpen;
using LinuxProcessDetail::readSmallFile;
using LinuxProcessDetail::sendThroughPidfd;
using LinuxProcessDetail::verifyIdentity;
using Posix::FdGuard;

/// Whether @p path is a regular file this process may execute.
[[nodiscard]] bool isExecutableFile(const std::string& path)
{
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

/// The value of environment variable @p name, or empty when unset.
[[nodiscard]] std::string_view environmentValue(const char* name)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - read once at construction, on the thread that builds the panels
    const char* const value = std::getenv(name);
    return value != nullptr ? std::string_view(value) : std::string_view{};
}

/// Whether TaskSmack runs as root with CAP_SYS_PTRACE in its effective set, which strace exec'd from it
/// keeps (root keeps its effective capabilities across exec). An unreadable or malformed CapEff does
/// not count as having it (fail closed).
[[nodiscard]] bool rootWithSysPtrace()
{
    if (::geteuid() != 0)
    {
        return false;
    }
    const std::optional<std::string> status = readSmallFile("/proc/self/status");
    const std::optional<std::uint64_t> capEff = status ? ProcPrivileges::parseCapEff(*status) : std::nullopt;
    return SyscallTrace::rootTracerHasSysPtrace(true, capEff);
}

/// Whether the tool at @p tracerPath carries CAP_SYS_PTRACE as a file capability, whoever runs it.
[[nodiscard]] bool tracerHasFileCapability(const std::string& tracerPath)
{
    std::array<std::uint8_t, 64> xattr{};
    const auto len = ::getxattr(tracerPath.c_str(), "security.capability", xattr.data(), xattr.size());
    return len > 0 && SyscallTrace::fileCapsGrantSysPtrace(std::span<const std::uint8_t>(xattr.data(), static_cast<std::size_t>(len)));
}

/// What ptrace(2) will check when strace attaches to @p pid, read from /proc.
[[nodiscard]] SyscallTrace::PtraceContext ptraceContextFor(std::int32_t pid, const std::string& tracerPath)
{
    SyscallTrace::PtraceContext context;
    if (const std::optional<std::string> scope = readSmallFile("/proc/sys/kernel/yama/ptrace_scope"))
    {
        context.ptraceScope = SyscallTrace::parsePtraceScope(*scope);
    }
    const bool fileCapability = tracerHasFileCapability(tracerPath);
    const bool rootPrivilege = rootWithSysPtrace();
    context.privileged = fileCapability || rootPrivilege;
    context.privilegeIsInherited = rootPrivilege && !fileCapability;

    // ptrace requires the target's real, effective and saved IDs all to be the tracer's. A process
    // that is not dumpable (it changed credentials, or asked not to be) cannot be attached to either;
    // the kernel shows that by making root the owner of its /proc directory.
    const std::string procDir = "/proc/" + std::to_string(pid);
    const std::optional<std::string> status = readSmallFile(procDir + "/status");
    const auto uids = status ? SyscallTrace::parseStatusIds(*status, "Uid") : std::nullopt;
    const auto gids = status ? SyscallTrace::parseStatusIds(*status, "Gid") : std::nullopt;
    struct stat dirInfo{};
    const bool ownsProcDir = ::stat(procDir.c_str(), &dirInfo) == 0 && dirInfo.st_uid == ::geteuid();
    const auto allEqual = [](const std::optional<std::array<std::uint32_t, 3>>& ids, std::uint32_t expected)
    {
        return ids.has_value() && std::ranges::all_of(*ids, [expected](std::uint32_t id) { return id == expected; });
    };
    context.sameCredentials = ownsProcDir && allEqual(uids, ::geteuid()) && allEqual(gids, ::getegid());
    return context;
}

/// The name the kernel gives @p pid (/proc/<pid>/comm) without its newline; empty when it can't be read.
[[nodiscard]] std::string processComm(std::int32_t pid)
{
    std::string comm = readSmallFile(std::format("/proc/{}/comm", pid)).value_or(std::string{});
    while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r'))
    {
        comm.pop_back();
    }
    return comm;
}

/// The state letter of @p pid in /proc/<pid>/stat ('T' or 't' when stopped, by a signal or a tracer), so a
/// cancelled dump knows whether to wake it; '\0' when it can't be read.
[[nodiscard]] char processStateFromProc(std::int32_t pid)
{
    const std::string stat = readSmallFile(std::format("/proc/{}/stat", pid)).value_or(std::string{});
    return LinuxCoreDump::stateFromStat(stat).value_or('\0');
}

/// Whether @p pid is stopped, by a signal or a tracer, as @p syscalls reads its state.
[[nodiscard]] bool processIsStopped(const LinuxProcessSyscalls& syscalls, std::int32_t pid)
{
    return LinuxCoreDump::isStoppedState(syscalls.processState(pid));
}

/// Who traces @p pid (its TracerPid; 0 for nobody), or nullopt once its status can't be read (it is gone).
[[nodiscard]] std::optional<std::int32_t> tracerPidFromProc(std::int32_t pid)
{
    const std::optional<std::string> status = readSmallFile(std::format("/proc/{}/status", pid));
    return status.has_value() ? LinuxCoreDump::tracerPidFromStatus(*status) : std::nullopt;
}

/// Waits, up to LinuxCoreDump::DETACH_WAIT_MS, until @p pid is traced by @p tracerBefore again (whoever
/// traced it before the dump; usually nobody): the killed gdb has exited and let go of it (#2122).
void waitForDumperToDetach(const LinuxProcessSyscalls& syscalls, std::int32_t pid, std::int32_t tracerBefore)
{
    constexpr auto STEP = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(LinuxCoreDump::DETACH_WAIT_MS);
    for (;;)
    {
        const std::optional<std::int32_t> tracer = syscalls.tracerPid(pid);
        if (!tracer.has_value() || *tracer == tracerBefore || std::chrono::steady_clock::now() >= deadline)
        {
            return;
        }
        std::this_thread::sleep_for(STEP);
    }
}

/// How a tool run by runCancellable() ended.
struct ToolRun
{
    std::error_code spawnError; ///< Set when it couldn't be started.
    bool cancelled = false;     ///< Killed because the stop token was requested.
    std::error_code waitError;  ///< Set when waiting on it failed, so it was killed (#2148, #2751).
    int exitStatus = -1;        ///< Its exit status when it exited on its own; -1 when a signal ended it.
    std::string stderrText;     ///< The last of what it wrote on stderr (MAX_STDERR_KEPT bytes at most).
};

constexpr std::size_t MAX_STDERR_KEPT = 4096;
constexpr int TOOL_POLL_MS = 50;

/// Appends what @p fd has ready to @p text, keeping its last MAX_STDERR_KEPT bytes. False at end of file.
bool readAvailable(int fd, std::string& text)
{
    std::array<char, 1024> chunk{};
    const ssize_t got = ::read(fd, chunk.data(), chunk.size()); // NOLINT(misc-include-cleaner) - ssize_t is from <sys/types.h>
    if (got <= 0)
    {
        return got < 0 && errno == EINTR;
    }
    text.append(chunk.data(), static_cast<std::size_t>(got));
    if (text.size() > MAX_STDERR_KEPT)
    {
        text.erase(0, text.size() - MAX_STDERR_KEPT);
    }
    return true;
}

/// Runs @p argv in a process group of its own, stdin and stdout on /dev/null and stderr kept, until it
/// exits or @p stopToken is requested; then the whole group is killed (gcore runs gdb under it) and
/// reaped, waited on through @p syscalls. Blocking: called on createDump()'s worker thread.
[[nodiscard]] ToolRun
runCancellable(const LinuxProcessSyscalls& syscalls, const std::vector<std::string>& argv, const std::stop_token& stopToken)
{
    ToolRun run;
    std::array<int, 2> pipeFds{-1, -1};
    if (::pipe2(pipeFds.data(), O_CLOEXEC) != 0)
    {
        run.spawnError = std::error_code(errno, std::generic_category());
        return run;
    }
    const FdGuard readEnd(pipeFds[0]);
    FdGuard writeEnd(pipeFds[1]);

    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawnattr_init(&attributes);
    ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_adddup2(&actions, writeEnd.get(), STDERR_FILENO);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    // Nothing else of TaskSmack's: a descriptor a driver or library opened without close-on-exec would
    // otherwise pass into gcore and the gdb it runs, which ptraces another process (#2148).
    ::posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
#endif
    // Its own process group, so a cancel reaches the gdb gcore runs; an empty signal mask and default
    // dispositions, whatever this thread blocks or ignores.
    sigset_t signals{}; // NOLINT(misc-include-cleaner) - sigset_t is provided by <signal.h>
    sigemptyset(&signals);
    ::posix_spawnattr_setsigmask(&attributes, &signals);
    sigaddset(&signals, SIGPIPE);
    ::posix_spawnattr_setsigdefault(&attributes, &signals);
    ::posix_spawnattr_setpgroup(&attributes, 0);
    ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
    {
        args.push_back(const_cast<char*>(arg.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast) - posix_spawn's char* const[]
    }
    args.push_back(nullptr);
    pid_t child = -1;
    const int spawned = ::posix_spawn(&child, argv.front().c_str(), &actions, &attributes, args.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attributes);
    {
        const FdGuard closing(std::move(writeEnd)); // Only the child writes: end of file once it is gone.
    }
    if (spawned != 0)
    {
        run.spawnError = std::error_code(spawned, std::generic_category());
        return run;
    }

    for (;;)
    {
        pollfd ready{.fd = readEnd.get(), .events = POLLIN, .revents = 0};
        if (::poll(&ready, 1, TOOL_POLL_MS) > 0 && (ready.revents & (POLLIN | POLLHUP)) != 0)
        {
            static_cast<void>(readAvailable(readEnd.get(), run.stderrText));
        }
        int status = 0;
        // NOLINTNEXTLINE(misc-include-cleaner) - WNOHANG is provided by <sys/wait.h>
        const pid_t waited = syscalls.waitPid(child, &status, WNOHANG);
        if (waited == child)
        {
            // Whatever it wrote last, up to the end of the pipe (or until nothing more is ready).
            pollfd rest{.fd = readEnd.get(), .events = POLLIN, .revents = 0};
            while (::poll(&rest, 1, 0) > 0 && readAvailable(readEnd.get(), run.stderrText))
            {
            }
            // NOLINTNEXTLINE(misc-include-cleaner) - WIFEXITED/WEXITSTATUS are provided by <sys/wait.h>
            run.exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return run;
        }
        if (waited < 0 && errno != EINTR)
        {
            run.waitError = std::error_code(errno, std::generic_category());
            // Not left running: gdb may be attached to the target, which would stay stopped (#2148).
            ::kill(-child, SIGKILL);
            while (syscalls.waitPid(child, &status, 0) < 0 && errno == EINTR)
            {
            }
            return run;
        }
        if (stopToken.stop_requested())
        {
            ::kill(-child, SIGKILL);
            while (syscalls.waitPid(child, &status, 0) < 0 && errno == EINTR)
            {
            }
            run.cancelled = true;
            return run;
        }
    }
}

/// Where /proc/<pid>/exe points, or the errno readlink gave (#3064).
[[nodiscard]] std::expected<std::string, int> executableLinkFromProc(std::int32_t pid)
{
    std::error_code error;
    const std::filesystem::path link = std::filesystem::read_symlink(std::format("/proc/{}/exe", pid), error);
    if (error)
    {
        return std::unexpected(error.value());
    }
    return link.string();
}

/// org.freedesktop.FileManager1.ShowItems([uri], "") on this user's session bus: Nautilus, Dolphin,
/// Nemo, Caja, Thunar and PCManFM answer it with the file selected (#3064). False when there is no
/// session bus or nothing answers in time.
// LCOV_EXCL_START - opens the user's real file manager; tests fake it (LinuxProcessSyscalls::showItemInFileManager)
[[nodiscard]] bool showItemOverSessionBus(const std::string& uri)
{
#ifdef TASKSMACK_HAS_SDBUS
    namespace Bus = SystemdBus::Detail;
    Bus::Bus bus;
    int result = sd_bus_open_user(bus.out());
    if (result < 0)
    {
        spdlog::debug("Open file location: no session bus ({})",
                      std::strerror(-result)); // NOLINT(concurrency-mt-unsafe) - one worker thread
        return false;
    }
    (void) sd_bus_set_method_call_timeout(bus.get(), SystemdBus::CALL_TIMEOUT_USEC);
    Bus::BusError error;
    result = sd_bus_call_method(bus.get(),
                                "org.freedesktop.FileManager1",
                                "/org/freedesktop/FileManager1",
                                "org.freedesktop.FileManager1",
                                "ShowItems",
                                error.get(),
                                nullptr,
                                "ass",
                                1,
                                uri.c_str(),
                                ""); // no startup notification ID
    if (result < 0)
    {
        spdlog::debug("Open file location: ShowItems failed ({})", error.text(result));
        return false;
    }
    return true;
#else
    static_cast<void>(uri);
    return false;
#endif
}
// LCOV_EXCL_STOP

/// @p syscalls with the trace and dump calls a test left null filled in with the real ones;
/// LinuxProcessControl fills in the thread listing the same way.
[[nodiscard]] LinuxProcessSyscalls withTraceAndDumpDefaults(LinuxProcessSyscalls syscalls)
{
    if (syscalls.ptraceContext == nullptr)
    {
        syscalls.ptraceContext = &ptraceContextFor;
    }
    if (syscalls.waitPid == nullptr)
    {
        syscalls.waitPid = realProcessSyscalls().waitPid;
    }
    if (syscalls.processState == nullptr)
    {
        syscalls.processState = &processStateFromProc;
    }
    if (syscalls.tracerPid == nullptr)
    {
        syscalls.tracerPid = &tracerPidFromProc;
    }
    if (syscalls.executableLink == nullptr)
    {
        syscalls.executableLink = &executableLinkFromProc;
    }
    if (syscalls.showItemInFileManager == nullptr)
    {
        syscalls.showItemInFileManager = &showItemOverSessionBus;
    }
    return syscalls;
}

} // namespace

LinuxProcessActions::LinuxProcessActions() : LinuxProcessActions(discoverSyscallTraceTools(), realProcessSyscalls())
{}

LinuxProcessActions::LinuxProcessActions(SyscallTrace::Tools traceTools) : LinuxProcessActions(std::move(traceTools), realProcessSyscalls())
{}

LinuxProcessActions::LinuxProcessActions(SyscallTrace::Tools traceTools, const LinuxProcessSyscalls& syscalls)
    : LinuxProcessControl(withTraceAndDumpDefaults(syscalls)), m_TraceTools(std::move(traceTools))
{}

ProcessActionCapabilities LinuxProcessActions::actionCapabilities() const
{
    SyscallTraceAvailability syscallTrace = SyscallTraceAvailability::Available;
    if (m_TraceTools.tracerPath.empty())
    {
        syscallTrace = SyscallTraceAvailability::NoTracer;
    }
    else if (!m_TraceTools.terminal.has_value())
    {
        syscallTrace = SyscallTraceAvailability::NoTerminal;
    }
    ProcessActionCapabilities capabilities = LinuxProcessControl::actionCapabilities();
    capabilities.syscallTrace = syscallTrace;
    capabilities.canCreateDump = !m_TraceTools.coreDumperPath.empty(); // gdb's gcore (#807)
#ifdef TASKSMACK_HAS_SDBUS
    capabilities.canOpenFileLocation = true; // ShowItems is tried first; xdg-open, if found, is the fallback
#else
    capabilities.canOpenFileLocation = !m_TraceTools.folderOpenerPath.empty();
#endif
    return capabilities;
}

SyscallTrace::Tools LinuxProcessActions::discoverSyscallTraceTools()
{
    const std::string_view pathEnv = environmentValue("PATH");
    const auto find = [pathEnv](std::string_view name)
    {
        return SyscallTrace::findExecutable(name, pathEnv, isExecutableFile);
    };
    SyscallTrace::Tools tools;
    tools.tracerPath = find("strace").value_or(std::string{});
    tools.terminal = SyscallTrace::selectTerminal(environmentValue("TERMINAL"), find);
    tools.coreDumperPath = find("gcore").value_or(std::string{});
    tools.folderOpenerPath = find("xdg-open").value_or(std::string{});
    spdlog::debug("System call tracing: strace {}, terminal {}; dump files: gcore {}; file locations: xdg-open {}",
                  tools.tracerPath.empty() ? std::string("not found") : tools.tracerPath,
                  tools.terminal ? tools.terminal->path : std::string("not found"),
                  tools.coreDumperPath.empty() ? std::string("not found") : tools.coreDumperPath,
                  tools.folderOpenerPath.empty() ? std::string("not found") : tools.folderOpenerPath);
    return tools;
}

ProcessActionResult LinuxProcessActions::launchSyscallTrace(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    const SyscallTraceAvailability availability = actionCapabilities().syscallTrace;
    if (availability != SyscallTraceAvailability::Available || !m_TraceTools.terminal.has_value())
    {
        return ProcessActionResult::error(syscallTraceUnavailableReason(availability));
    }
    const SyscallTrace::Terminal& terminal = *m_TraceTools.terminal;

    // The same identity check as every other action: the pidfd is opened first, then the start time
    // compared, so the process checked is the one holding the PID now. strace then attaches by PID a
    // moment later -- it has no pidfd form -- so a target that exits in that instant and whose PID is
    // reused at once could still be the one traced; tracing only observes, and strace names the
    // process it attached to.
    const PidfdOpen opened = openPidfd(syscalls(), target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Not tracing PID {}: {}", target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);
    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not tracing PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    const SyscallTrace::PtraceContext context = syscalls().ptraceContext(target.pid, m_TraceTools.tracerPath);
    if (const std::optional<std::string> refusal = SyscallTrace::ptraceRefusal(target.pid, context))
    {
        spdlog::info("Not tracing PID {}: {}", target.pid, *refusal);
        return ProcessActionResult::error(*refusal);
    }
    // x-terminal-emulator and the like are links: what they resolve to decides who runs strace (#2140).
    std::error_code resolveError;
    const std::filesystem::path resolvedTerminal = std::filesystem::canonical(terminal.path, resolveError);
    if (const std::optional<std::string> refusal =
            SyscallTrace::serverTerminalRefusal(target.pid, context, resolveError ? terminal.path : resolvedTerminal.string()))
    {
        spdlog::info("Not tracing PID {}: {}", target.pid, *refusal);
        return ProcessActionResult::error(*refusal);
    }
    // Found at construction; make sure it has not been removed since, or the terminal would open and
    // close again at once.
    if (!isExecutableFile(m_TraceTools.tracerPath))
    {
        return ProcessActionResult::error(std::format("{} is no longer there or not executable", m_TraceTools.tracerPath));
    }

    const std::vector<std::string> command = SyscallTrace::tracerCommand(m_TraceTools.tracerPath, target.pid);
    const std::vector<std::string> argv = SyscallTrace::buildTerminalArgv(terminal, command);
    const auto spawned = DetachedSpawn::spawnDetached(argv);
    if (!spawned.has_value())
    {
        std::string message = DetachedSpawn::spawnFailureMessage(spawned.error(), terminal.path);
        spdlog::warn("Failed to open strace for PID {}: {}", target.pid, message);
        return ProcessActionResult::error(std::move(message));
    }
    spdlog::info("Opened {} running strace on PID {}", terminal.path, target.pid);
    return ProcessActionResult::ok();
}

ProcessActionResult LinuxProcessActions::openFileLocation(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (!actionCapabilities().canOpenFileLocation)
    {
        return IProcessActions::openFileLocation(target);
    }
    // The identity check every action makes, then the link read, then the pidfd asked whether the
    // process survived the read: if it did, its PID never changed hands, so the link was its own.
    const PidfdOpen opened = openPidfd(syscalls(), target.pid);
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
    const std::expected<std::string, int> link = syscalls().executableLink(target.pid);
    int exited = syscalls().pollPidfd(pidfd.get());
    while (exited < 0 && errno == EINTR)
    {
        exited = syscalls().pollPidfd(pidfd.get());
    }
    if (exited != 0)
    {
        return ProcessActionResult::error("The process exited");
    }
    if (!link.has_value())
    {
        if (link.error() == ENOENT)
        {
            return ProcessActionResult::error("It has no executable file (a kernel thread)");
        }
        if (link.error() == EACCES || link.error() == EPERM)
        {
            return ProcessActionResult::error("Its executable can't be read: it belongs to another user");
        }
        return ProcessActionResult::error(std::format("Its executable can't be read: {}", std::system_category().message(link.error())));
    }
    const std::string_view path = FileManagerReveal::withoutDeletedSuffix(*link);
    // Deleted or replaced since it started (an upgraded package): its folder is still worth opening.
    std::string note = (path.size() != link->size()) ? std::string("its file has since been deleted or replaced") : std::string{};

    // Root for another user (sudo, pkexec): xdg-open as them, in their session (#2461), and no D-Bus.
    const std::optional<DetachedSpawn::RunAs> user = SessionUser::invokingUserSession();
    if (!user.has_value() && syscalls().showItemInFileManager(FileManagerReveal::fileUri(path)))
    {
        spdlog::info("Asked the file manager to show {} (PID {})", path, target.pid);
        return ProcessActionResult::ok(std::move(note));
    }
    const std::string& opener = m_TraceTools.folderOpenerPath;
    if (opener.empty())
    {
        return ProcessActionResult::error("No file manager answered, and xdg-open isn't installed");
    }
    const std::array<std::string, 2> argv{opener, std::string(FileManagerReveal::parentFolder(path))};
    const auto spawned = user.has_value() ? DetachedSpawn::spawnDetached(argv, *user) : DetachedSpawn::spawnDetached(argv);
    if (!spawned.has_value())
    {
        std::string message = DetachedSpawn::spawnFailureMessage(spawned.error(), opener);
        spdlog::warn("Failed to open the folder of PID {}'s executable: {}", target.pid, message);
        return ProcessActionResult::error(std::move(message));
    }
    spdlog::info("Opened {} on {} (PID {})", opener, argv[1], target.pid);
    return ProcessActionResult::ok(std::move(note));
}

ProcessDumpResult
LinuxProcessActions::createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken)
{
    const std::string& gcore = m_TraceTools.coreDumperPath;
    if (gcore.empty())
    {
        return std::unexpected(std::string(DUMP_UNSUPPORTED_MESSAGE));
    }
    if (target.pid <= 0)
    {
        return std::unexpected(std::string("Invalid PID"));
    }
    // The identity check every action makes. gcore then attaches by PID -- it has no pidfd form -- so, as
    // with strace, a target that exits in that instant and whose PID is reused at once could be the one
    // dumped; the core names the process it was taken from.
    const PidfdOpen opened = openPidfd(syscalls(), target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Not dumping PID {}: {}", target.pid, opened.refusal);
        return std::unexpected(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);
    if (const ProcessActionResult identity = verifyIdentity(target); !identity.success)
    {
        spdlog::warn("Not dumping PID {}: {}", target.pid, identity.errorMessage);
        return std::unexpected(identity.errorMessage);
    }
    if (const std::optional<std::string> refusal = SyscallTrace::ptraceRefusal(
            target.pid, syscalls().ptraceContext(target.pid, LinuxCoreDump::gcoreDebuggerPath(gcore)), SyscallTrace::GCORE_TOOL))
    {
        spdlog::info("Not dumping PID {}: {}", target.pid, *refusal);
        return std::unexpected(*refusal);
    }
    if (!isExecutableFile(gcore))
    {
        return std::unexpected(std::format("{} is no longer there or not executable", gcore));
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
    {
        return std::unexpected(std::format("The folder {} doesn't exist", directory.string()));
    }

    // A name not taken yet: the process name, PID and local time, then " (2)" and on. Reserved by
    // creating it (O_EXCL), an empty placeholder until the dump is renamed over it (#2147): two dumps
    // started in the same second -- the Processes menu and Process Details each have one -- get
    // different names, and so different ".partial" files, and nothing made there meanwhile is replaced.
    const std::string comm = processComm(target.pid);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);
    std::filesystem::path file;
    for (int attempt = 1; attempt <= LinuxCoreDump::MAX_NAME_ATTEMPTS && file.empty(); ++attempt)
    {
        std::filesystem::path candidate = directory / LinuxCoreDump::dumpFileName(comm, target.pid, local, attempt);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
        const int reserved = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (reserved >= 0)
        {
            ::close(reserved);
            file = std::move(candidate);
        }
        else if (errno != EEXIST)
        {
            return std::unexpected(
                std::format("Couldn't create the dump file in {}: {}", directory.string(), std::system_category().message(errno)));
        }
    }
    if (file.empty())
    {
        return std::unexpected(std::format("Couldn't find a free name for the dump file in {}", directory.string()));
    }
    // Every way out but success gives the reserved name back.
    const auto releaseName = [&file]
    {
        std::error_code removeError;
        std::filesystem::remove(file, removeError);
    };

    // gcore writes "<prefix>.<pid>"; the prefix marks it partial until it is renamed into place.
    const std::filesystem::path prefix = file.string() + ".partial";
    const std::filesystem::path written = LinuxCoreDump::gcoreOutputFile(prefix, target.pid);
    const bool wasStopped = processIsStopped(syscalls(), target.pid);
    const std::int32_t tracerBefore = syscalls().tracerPid(target.pid).value_or(0);
    spdlog::info("Writing a dump of PID {} with {} to {}", target.pid, gcore, file.string());
    const ToolRun run = runCancellable(syscalls(), LinuxCoreDump::gcoreArgv(gcore, prefix.string(), target.pid), stopToken);
    if (run.spawnError)
    {
        releaseName();
        return std::unexpected(std::format("Couldn't start {}: {}", gcore, run.spawnError.message()));
    }
    if (run.cancelled || run.waitError)
    {
        std::filesystem::remove(written, ec);
        releaseName();
        // gdb was killed while attached (cancelled, or TaskSmack lost track of gcore and killed it, #2751):
        // the kernel detaches it, but a process its attach stopped may stay stopped. Killing gcore's group doesn't wait for gdb (gcore's
        // child) to exit, and until it has, the process shows 't' and its stop is still to come (#2122): wait for gdb to let go first. One
        // that was running before is then woken, through the pidfd, so it is still the target.
        waitForDumperToDetach(syscalls(), target.pid, tracerBefore);
        if (!wasStopped && processIsStopped(syscalls(), target.pid))
        {
            static_cast<void>(sendThroughPidfd(syscalls(), pidfd, SIGCONT));
        }
        if (run.waitError)
        {
            // TaskSmack's failure, not gcore's: say so rather than "gcore failed".
            std::string message = std::format(
                "TaskSmack lost track of {} ({}), so it was stopped; the partial dump file was deleted", gcore, run.waitError.message());
            spdlog::warn("Dump of PID {} failed: {}", target.pid, message);
            return std::unexpected(std::move(message));
        }
        spdlog::info("Dump of PID {} cancelled", target.pid);
        return std::unexpected(std::string(LinuxCoreDump::CANCELLED_MESSAGE));
    }
    if (run.exitStatus != 0 || !std::filesystem::exists(written, ec))
    {
        std::filesystem::remove(written, ec);
        releaseName();
        std::string message = LinuxCoreDump::gcoreFailureMessage(target.pid, run.exitStatus, run.stderrText);
        spdlog::warn("Dump of PID {} failed: {}", target.pid, message);
        return std::unexpected(std::move(message));
    }
    // Over the placeholder reserved above, which only this dump made.
    std::filesystem::rename(written, file, ec);
    if (ec)
    {
        std::filesystem::remove(written, ec);
        releaseName();
        return std::unexpected(std::format("Couldn't name the dump file {}: {}", file.string(), ec.message()));
    }
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    spdlog::info("Wrote a dump of PID {} to {}", target.pid, file.string());
    return ProcessDump{.file = file,
                       .sizeBytes = ec ? 0 : static_cast<std::uint64_t>(size),
                       .note = {},
                       .fromSnapshot = false,
                       .liveReason = LinuxCoreDump::LIVE_REASON};
}

DumpSpace LinuxProcessActions::dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory)
{
    DumpSpace space;
    struct statvfs stats{};
    if (!directory.empty() && ::statvfs(directory.c_str(), &stats) == 0)
    {
        space.freeBytes = static_cast<std::uint64_t>(stats.f_bavail) * static_cast<std::uint64_t>(stats.f_frsize);
    }
    if (target.pid > 0)
    {
        // Read first, then confirmed to have been the target's: a PID reused in between is not counted.
        std::ifstream file(std::format("/proc/{}/status", target.pid));
        const std::string status{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (!status.empty() && verifyIdentity(target).success)
        {
            space.processBytes = LinuxCoreDump::coreBytesFromStatus(status);
        }
    }
    return space;
}

} // namespace Platform
