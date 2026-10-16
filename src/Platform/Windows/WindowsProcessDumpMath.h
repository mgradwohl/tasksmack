#pragma once

// The pure parts of Windows' "Create dump file" (#807): the dump file's name, whether TaskSmack's
// architecture can dump the process's (#2152), and what each failure says. Win32 error codes come in as plain integers and are compared
// against this header's own constants, so it reaches no Windows header and its tests build and run on every platform (#1133).

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace Platform::DumpMath
{

// The Win32 error codes the messages tell apart (winerror.h), as plain values.
inline constexpr std::uint32_t ERROR_CODE_FILE_NOT_FOUND = 2;
inline constexpr std::uint32_t ERROR_CODE_PATH_NOT_FOUND = 3;
inline constexpr std::uint32_t ERROR_CODE_ACCESS_DENIED = 5;
inline constexpr std::uint32_t ERROR_CODE_NOT_ENOUGH_MEMORY = 8;
inline constexpr std::uint32_t ERROR_CODE_HANDLE_DISK_FULL = 39;
inline constexpr std::uint32_t ERROR_CODE_NOT_SUPPORTED = 50;
inline constexpr std::uint32_t ERROR_CODE_INVALID_PARAMETER = 87;
inline constexpr std::uint32_t ERROR_CODE_DISK_FULL = 112;
inline constexpr std::uint32_t ERROR_CODE_PARTIAL_COPY = 299;
inline constexpr std::uint32_t ERROR_CODE_CANCELLED = 1223;
inline constexpr std::uint32_t ERROR_CODE_COMMITMENT_LIMIT = 1455;

/// How many names dumpFileName() tries in turn when the one before exists already.
inline constexpr int MAX_NAME_ATTEMPTS = 20;

/// MiniDumpWriteDump() reports some failures through GetLastError() as an HRESULT that wraps a Win32
/// error (HRESULT_FROM_WIN32: 0x8007xxxx, e.g. 0x80070070 for a full disk). That is unwrapped to the
/// Win32 code; any other value is returned as it is.
[[nodiscard]] constexpr std::uint32_t win32CodeFromDumpError(std::uint32_t error) noexcept
{
    constexpr std::uint32_t WIN32_FACILITY_MASK = 0xFFFF0000U;
    constexpr std::uint32_t HRESULT_FROM_WIN32_PREFIX = 0x80070000U;
    constexpr std::uint32_t CODE_MASK = 0x0000FFFFU;
    return ((error & WIN32_FACILITY_MASK) == HRESULT_FROM_WIN32_PREFIX) ? (error & CODE_MASK) : error;
}

/// Whether @p error (a Win32 code or a wrapped one) means the disk is full.
[[nodiscard]] constexpr bool isDiskFull(std::uint32_t error) noexcept
{
    const std::uint32_t code = win32CodeFromDumpError(error);
    return code == ERROR_CODE_DISK_FULL || code == ERROR_CODE_HANDLE_DISK_FULL;
}

/// The local time a dump file is named after.
struct DumpTime
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

/// The process's image name made safe to start a file name with: its directory and ".exe" dropped,
/// and every character Windows forbids in a file name (and every control character) replaced by '_'.
/// "process" when nothing is left.
[[nodiscard]] inline std::string dumpFileStem(std::string_view imageName)
{
    if (const std::size_t slash = imageName.find_last_of("\\/"); slash != std::string_view::npos)
    {
        imageName.remove_prefix(slash + 1);
    }
    constexpr std::string_view EXE = ".exe";
    const auto asciiLower = [](char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>((c - 'A') + 'a') : c;
    };
    if (imageName.size() > EXE.size() && std::ranges::equal(imageName.substr(imageName.size() - EXE.size()), EXE, {}, asciiLower))
    {
        imageName.remove_suffix(EXE.size());
    }
    constexpr std::string_view FORBIDDEN = "<>:\"/\\|?*";
    std::string stem;
    stem.reserve(imageName.size());
    for (const char c : imageName)
    {
        const bool control = static_cast<unsigned char>(c) < 0x20U;
        stem.push_back((control || FORBIDDEN.contains(c)) ? '_' : c);
    }
    // A name may not end in a dot or a space on Windows.
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
    {
        stem.pop_back();
    }
    return stem.empty() ? std::string("process") : stem;
}

/// The dump file's name: "notepad_1234_2026-10-13_14-05-09.dmp" -- the image name, the PID and the
/// local time, so dumps of several processes, or several of one, sort together and never collide.
/// @p attempt 2 and up (the name before it existed already) adds " (2)" and so on before ".dmp".
[[nodiscard]] inline std::string dumpFileName(std::string_view imageName, std::int32_t pid, const DumpTime& time, int attempt = 1)
{
    std::string name = std::format("{}_{}_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}",
                                   dumpFileStem(imageName),
                                   pid,
                                   time.year,
                                   time.month,
                                   time.day,
                                   time.hour,
                                   time.minute,
                                   time.second);
    if (attempt > 1)
    {
        name += std::format(" ({})", attempt);
    }
    name += ".dmp";
    return name;
}

/// Why the process could not be opened to read its memory. @p isProtected: Windows says it is a
/// protected process (anti-malware, DRM, some system services), whose memory no program may read,
/// administrator or not.
[[nodiscard]] inline std::string openFailureMessage(std::uint32_t error, std::int32_t pid, bool isProtected)
{
    if (error == ERROR_CODE_ACCESS_DENIED)
    {
        if (isProtected)
        {
            return std::format("Process {} is a protected process: Windows lets no program read its memory, even as administrator, "
                               "so no dump file can be made",
                               pid);
        }
        return std::format("Access denied: can't read the memory of process {}. It may run elevated or as another user (run TaskSmack "
                           "as administrator for those).",
                           pid);
    }
    if (error == ERROR_CODE_INVALID_PARAMETER)
    {
        return "Process not found - may have already exited";
    }
    return std::format("Failed to open process {}: error {}", pid, error);
}

/// Why the dump file could not be created in @p directory (UTF-8).
[[nodiscard]] inline std::string createFileFailureMessage(std::uint32_t error, std::string_view directory)
{
    switch (error)
    {
    case ERROR_CODE_ACCESS_DENIED:
        return std::format("Access denied: can't write a file in {}", directory);
    case ERROR_CODE_PATH_NOT_FOUND:
    case ERROR_CODE_FILE_NOT_FOUND:
        return std::format("The folder {} doesn't exist", directory);
    default:
        break;
    }
    if (isDiskFull(error))
    {
        return std::format("The disk holding {} is full", directory);
    }
    return std::format("Can't create a dump file in {}: error {}", directory, error);
}

/// Why writing the dump failed, once it had started; the partial file has been deleted by then.
/// @p cancelled: it stopped because it was asked to.
[[nodiscard]] inline std::string writeFailureMessage(std::uint32_t error, std::int32_t pid, std::string_view directory, bool cancelled)
{
    if (cancelled || win32CodeFromDumpError(error) == ERROR_CODE_CANCELLED)
    {
        return "Cancelled; the partial dump file was deleted";
    }
    if (isDiskFull(error))
    {
        return std::format("Not enough disk space in {} for a full dump of process {}; the partial file was deleted. Free some space "
                           "and try again.",
                           directory,
                           pid);
    }
    switch (win32CodeFromDumpError(error))
    {
    case ERROR_CODE_ACCESS_DENIED:
        return std::format("Access denied while reading the memory of process {}; the partial dump file was deleted", pid);
    case ERROR_CODE_PARTIAL_COPY:
        return std::format("Could not read all of the memory of process {} (it may have exited while it was written); the partial "
                           "dump file was deleted",
                           pid);
    default:
        break;
    }
    return std::format("Failed to write the dump of process {}: error 0x{:08X}; the partial file was deleted", pid, error);
}

/// Why a dump is written from the running process rather than from a snapshot (#2001), for
/// ProcessDump::liveReason. @p openRefused: Windows refused to open the process with the access a
/// snapshot needs (SNAPSHOT_PROCESS_ACCESS), though it allowed the access a live dump needs; else
/// @p error is what PssCaptureSnapshot() returned.
[[nodiscard]] inline std::string snapshotFailureReason(std::uint32_t error, bool openRefused)
{
    if (openRefused || error == ERROR_CODE_ACCESS_DENIED)
    {
        return "Windows refused the access a snapshot of it needs";
    }
    switch (error)
    {
    case ERROR_CODE_NOT_ENOUGH_MEMORY:
    case ERROR_CODE_COMMITMENT_LIMIT:
        return "there wasn't enough memory to take a snapshot of it";
    case ERROR_CODE_NOT_SUPPORTED:
        return "this version of Windows can't take a snapshot of it";
    default:
        break;
    }
    return std::format("the snapshot failed with error {}", error);
}

/// The IMAGE_FILE_MACHINE_* values (winnt.h) a dump's architecture check tells apart, as plain values.
inline constexpr std::uint16_t MACHINE_UNKNOWN = 0x0000;
inline constexpr std::uint16_t MACHINE_I386 = 0x014C;
inline constexpr std::uint16_t MACHINE_ARMNT = 0x01C4; ///< 32-bit ARM (Thumb-2)
inline constexpr std::uint16_t MACHINE_AMD64 = 0x8664;
inline constexpr std::uint16_t MACHINE_ARM64 = 0xAA64;
inline constexpr std::uint16_t MACHINE_ARM64EC = 0xA641;
inline constexpr std::uint16_t MACHINE_ARM64X = 0xA64E;

/// The machine a process's code runs as, from what Windows says about it (#2152):
/// @p machineTypeInfo  GetProcessInformation(ProcessMachineTypeInfo)'s ProcessMachine (Windows 11 and
///                     later; MACHINE_UNKNOWN where it can't be read). The only one of the three that
///                     tells an x64 process emulated on ARM64 (AMD64) from a native ARM64 one.
/// @p wow64Machine     IsWow64Process2()'s process machine: the 32-bit guest (I386, ARMNT) of a WOW64
///                     process, else MACHINE_UNKNOWN -- an emulated x64 process is not a WOW64 one.
/// @p nativeMachine    IsWow64Process2()'s native machine: the machine Windows itself runs as.
/// MACHINE_UNKNOWN when none of them was read.
[[nodiscard]] constexpr std::uint16_t
processMachine(std::uint16_t machineTypeInfo, std::uint16_t wow64Machine, std::uint16_t nativeMachine) noexcept
{
    if (machineTypeInfo != MACHINE_UNKNOWN)
    {
        return machineTypeInfo;
    }
    return (wow64Machine != MACHINE_UNKNOWN) ? wow64Machine : nativeMachine;
}

/// A machine's name in the messages: "x86", "x64", "ARM64", "ARM64EC" or "ARM32"; empty when unknown.
[[nodiscard]] constexpr std::string_view machineName(std::uint16_t machine) noexcept
{
    switch (machine)
    {
    case MACHINE_I386:
        return "x86";
    case MACHINE_AMD64:
        return "x64";
    case MACHINE_ARM64:
    case MACHINE_ARM64X:
        return "ARM64";
    case MACHINE_ARM64EC:
        return "ARM64EC";
    case MACHINE_ARMNT:
        return "ARM32";
    default:
        return {};
    }
}

/// How TaskSmack's architecture and the process's compare, for a dump.
enum class DumpArchitecture : std::uint8_t
{
    Same,        ///< The same architecture, or the process's couldn't be read: an ordinary dump.
    Wow64Target, ///< A 32-bit (WOW64) process on TaskSmack's own architecture: a 64-bit dump of it, with a note.
    Unsupported, ///< Another architecture (an ARM64 process from x64 TaskSmack): no usable dump can be written.
};

/// Whether TaskSmack, built for @p selfMachine, can dump a process running as @p targetMachine
/// (processMachine()). TaskSmack is built 64-bit only: x64, and maybe one day ARM64 (ARM64EC and
/// ARM64X count as ARM64). A process of its own architecture is dumped as usual; one of its 32-bit
/// WOW64 guests (x86 on x64; x86 and ARM32 on ARM64) as a 64-bit dump, with a note; anything else is
/// refused -- notably a native ARM64 process from the x64 TaskSmack emulated on Windows on ARM, which
/// IsWow64Process() alone took for "the same". A target whose machine couldn't be read is tried as
/// usual, as before; a TaskSmack built for anything but x64 or ARM64 refuses every process.
[[nodiscard]] constexpr DumpArchitecture dumpArchitecture(std::uint16_t selfMachine, std::uint16_t targetMachine) noexcept
{
    const auto isArm64 = [](std::uint16_t machine)
    {
        return machine == MACHINE_ARM64 || machine == MACHINE_ARM64EC || machine == MACHINE_ARM64X;
    };
    if (selfMachine == MACHINE_AMD64)
    {
        if (targetMachine == MACHINE_AMD64 || targetMachine == MACHINE_UNKNOWN)
        {
            return DumpArchitecture::Same;
        }
        return (targetMachine == MACHINE_I386) ? DumpArchitecture::Wow64Target : DumpArchitecture::Unsupported;
    }
    if (isArm64(selfMachine))
    {
        if (isArm64(targetMachine) || targetMachine == MACHINE_UNKNOWN)
        {
            return DumpArchitecture::Same;
        }
        return (targetMachine == MACHINE_I386 || targetMachine == MACHINE_ARMNT) ? DumpArchitecture::Wow64Target
                                                                                 : DumpArchitecture::Unsupported;
    }
    return DumpArchitecture::Unsupported;
}

/// What a 64-bit dump of a 32-bit (WOW64) process running as @p targetMachine needs to be read, as
/// Task Manager's are: WinDbg's effective machine set to the process's own.
[[nodiscard]] inline std::string wow64DumpNote(std::uint16_t targetMachine)
{
    const std::string_view effectiveMachine = (targetMachine == MACHINE_ARMNT) ? "arm" : "x86";
    return std::format("This is a 32-bit process, so the dump is a 64-bit one: in WinDbg, run .effmach {} (or !wow64exts.sw) to see "
                       "its 32-bit threads and stacks.",
                       effectiveMachine);
}

/// Why TaskSmack, built for @p selfMachine, refuses process @p pid, running as @p targetMachine
/// (DumpArchitecture::Unsupported).
[[nodiscard]] inline std::string unsupportedArchitectureMessage(std::int32_t pid, std::uint16_t selfMachine, std::uint16_t targetMachine)
{
    const std::string_view selfName = machineName(selfMachine);
    const std::string_view targetName = machineName(targetMachine);
    const std::string self =
        selfName.empty() ? std::format("built for machine type 0x{:04X}", selfMachine) : std::format("an {} build", selfName);
    const std::string target =
        targetName.empty() ? std::format("runs as machine type 0x{:04X}", targetMachine) : std::format("is an {} process", targetName);
    return std::format("Process {} {}, and this TaskSmack is {}: it can't write a usable dump of a process of another architecture. "
                       "Task Manager can dump it (Details, right-click, Create memory dump file).",
                       pid,
                       target,
                       self);
}

} // namespace Platform::DumpMath
