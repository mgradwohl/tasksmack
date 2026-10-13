#pragma once

// The pure parts of Windows' "Create dump file" (#807): the dump file's name, and what each failure
// says. Win32 error codes come in as plain integers and are compared against this header's own
// constants, so it reaches no Windows header and its tests build and run on every platform (#1133).

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

/// How TaskSmack's bitness and the process's compare, for a dump.
enum class DumpBitness : std::uint8_t
{
    Same,                ///< Both 64-bit, or both 32-bit: an ordinary dump.
    Wow64Target,         ///< A 32-bit process (WOW64) dumped from 64-bit TaskSmack: a 64-bit dump of it.
    NativeTargetOfWow64, ///< A 32-bit TaskSmack and a 64-bit process: it can't read the process's memory.
};

[[nodiscard]] constexpr DumpBitness dumpBitness(bool selfIsWow64, bool targetIsWow64) noexcept
{
    if (selfIsWow64 == targetIsWow64)
    {
        return DumpBitness::Same;
    }
    return targetIsWow64 ? DumpBitness::Wow64Target : DumpBitness::NativeTargetOfWow64;
}

/// What a dump of a 32-bit process from 64-bit TaskSmack needs to be read, as Task Manager's are.
inline constexpr std::string_view WOW64_DUMP_NOTE =
    "This is a 32-bit process, so the dump is a 64-bit one: in WinDbg, run .effmach x86 (or !wow64exts.sw) to see its 32-bit "
    "threads and stacks.";

/// Why a 32-bit TaskSmack refuses a 64-bit process.
[[nodiscard]] inline std::string nativeTargetOfWow64Message(std::int32_t pid)
{
    return std::format("Process {} is a 64-bit process, and this TaskSmack is 32-bit: run the 64-bit TaskSmack to dump it", pid);
}

} // namespace Platform::DumpMath
