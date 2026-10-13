#pragma once

// Windows' "Create dump file" (#807): a full-memory minidump of a process, through dbghelp's
// MiniDumpWriteDump, as Task Manager writes one. WindowsProcessActions::createDump() opens and
// verifies the process; this writes the file. dbghelp.dll is loaded from System32 the first time a
// dump is written, never at startup, and never from beside TaskSmack.
//
// The dump is written from a snapshot where Windows can take one (#2001): PssCaptureSnapshot clones
// the process's address space copy-on-write, and MiniDumpWriteDump reads the clone, so the process
// pauses only while the clone is made instead of for the whole write. When the snapshot fails the
// process is dumped live, as before, and the result says why.

#include "Platform/IProcessActions.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <processsnapshot.h> // NOLINT(misc-include-cleaner) - HPSS, PSS_CAPTURE_FLAGS
// clang-format on

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The access a live dump opens the process with: what MiniDumpWriteDump() needs to read it.
inline constexpr DWORD DUMP_PROCESS_ACCESS = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;

/// The access a snapshot dump opens the process with: a live dump's, and what PssCaptureSnapshot()
/// needs for SNAPSHOT_CAPTURE_FLAGS -- PROCESS_CREATE_PROCESS to clone the address space and
/// PROCESS_DUP_HANDLE to capture the handles.
inline constexpr DWORD SNAPSHOT_PROCESS_ACCESS = DUMP_PROCESS_ACCESS | PROCESS_CREATE_PROCESS | PROCESS_DUP_HANDLE;

/// PssCaptureSnapshot()'s signature: WindowsProcessFunctions holds it so a test can make it fail.
using PssCaptureSnapshotFn = DWORD(WINAPI*)(HANDLE process, PSS_CAPTURE_FLAGS flags, DWORD contextFlags, HPSS* snapshot);

/// Whether process @p pid is a protected process (PP or PPL: anti-malware, DRM, some system
/// services), whose memory no program may read. False when that can't be read.
[[nodiscard]] bool isProtectedProcess(DWORD pid);

/// How writeProcessDump() reads the process.
struct DumpSource
{
    /// Takes the snapshot the dump is written from (PssCaptureSnapshot, or a test's stand-in); null:
    /// the dump is written live, for liveReason.
    PssCaptureSnapshotFn captureSnapshot = nullptr;
    /// Why there is no snapshot to take, when captureSnapshot is null (DumpMath::snapshotFailureReason()).
    std::string liveReason;
};

/// Writes a full-memory dump of @p process into a new file in @p directory: Task Manager's dump type
/// (full memory, handles, unloaded modules, memory and thread info), named for the process, its PID
/// and the time (DumpMath::dumpFileName()). From a snapshot when @p source can take one, else from the
/// running process, with the reason in ProcessDump::liveReason. The file is deleted again on any
/// failure, and when @p stopToken is requested before it is done.
/// @param process  An open handle, already confirmed to be the target: with SNAPSHOT_PROCESS_ACCESS
///                 for a snapshot, DUMP_PROCESS_ACCESS at least.
[[nodiscard]] ProcessDumpResult writeProcessDump(
    HANDLE process, std::int32_t pid, const std::filesystem::path& directory, const std::stop_token& stopToken, const DumpSource& source);

/// The process's private bytes (PROCESS_MEMORY_COUNTERS_EX::PrivateUsage); nullopt when unreadable.
/// @param process  An open handle with PROCESS_QUERY_LIMITED_INFORMATION.
[[nodiscard]] std::optional<std::uint64_t> processPrivateBytes(HANDLE process);

/// The bytes free to this user on the volume holding @p directory; nullopt when unreadable.
[[nodiscard]] std::optional<std::uint64_t> freeBytesInFolder(const std::filesystem::path& directory);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
