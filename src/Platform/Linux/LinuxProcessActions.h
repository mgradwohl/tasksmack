#pragma once

#include "LinuxProcessControl.h"
#include "Platform/IProcessActions.h"
#include "SyscallTrace.h"

#include <filesystem>
#include <stop_token>

namespace Platform
{

/// Linux implementation of IProcessActions: LinuxProcessControl's signals, nice, I/O priority and CPU
/// affinity, plus the actions that start another program. TaskSmack uses this; tasksmack-helper links
/// only LinuxProcessControl (#2640).
///
/// launchSyscallTrace() opens a terminal running `strace -p` (#182). strace and the terminal are looked
/// up once, at construction (discoverSyscallTraceTools()), and actionCapabilities() reports what was
/// found; installing either later takes effect on the next start.
///
/// createDump() writes a core file with gdb's gcore (#807), found the same way: the action is offered
/// only where gcore is installed, and refused with the reason where ptrace would refuse gcore.
///
/// openFileLocation() asks the session's file manager to show the executable selected
/// (org.freedesktop.FileManager1.ShowItems over D-Bus), and opens its folder with xdg-open when none
/// answers (#3064).
class LinuxProcessActions : public LinuxProcessControl
{
  public:
    /// Finds strace and a terminal emulator on PATH (and $TERMINAL) for launchSyscallTrace().
    LinuxProcessActions();
    /// Uses @p traceTools instead of searching (tests: a fake terminal that records its argv).
    explicit LinuxProcessActions(SyscallTrace::Tools traceTools);
    /// As above, making its system calls through @p syscalls (tests: calls that fail on demand, #1548).
    LinuxProcessActions(SyscallTrace::Tools traceTools, const LinuxProcessSyscalls& syscalls);
    ~LinuxProcessActions() override = default;

    LinuxProcessActions(const LinuxProcessActions&) = delete;
    LinuxProcessActions& operator=(const LinuxProcessActions&) = delete;
    LinuxProcessActions(LinuxProcessActions&&) = default;
    LinuxProcessActions& operator=(LinuxProcessActions&&) = default;

    /// LinuxProcessControl's, with the system call trace and dump files as found at construction.
    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;
    [[nodiscard]] ProcessActionResult launchSyscallTrace(const ProcessTarget& target) override;
    /// A core file of the process, written by gcore into @p directory (#807). Live: gcore pauses the
    /// process while it copies its memory out. Once @p stopToken is requested, gcore (and the gdb it
    /// runs) is killed and the partial file deleted.
    [[nodiscard]] ProcessDumpResult
    createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken) override;
    /// The free space on @p directory's filesystem (statvfs) and what a core of the target holds at
    /// least (LinuxCoreDump::coreBytesFromStatus()), read from the target itself through the identity
    /// check (#807, #2001).
    [[nodiscard]] DumpSpace dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory) override;
    /// The executable /proc/<pid>/exe names, read through the identity check, shown selected by the
    /// session's file manager, or its folder opened with xdg-open (#3064). Running as root for another
    /// user (sudo, pkexec), the folder is opened as that user, in their session, and D-Bus isn't tried:
    /// root's session bus, if it has one, isn't theirs.
    [[nodiscard]] ProcessActionResult openFileLocation(const ProcessTarget& target) override;

    /// strace and the terminal to run it in, from this process's PATH and $TERMINAL, gcore for
    /// createDump() (#807), and xdg-open for openFileLocation() (#3064).
    [[nodiscard]] static SyscallTrace::Tools discoverSyscallTraceTools();

  private:
    SyscallTrace::Tools m_TraceTools;
};

} // namespace Platform
