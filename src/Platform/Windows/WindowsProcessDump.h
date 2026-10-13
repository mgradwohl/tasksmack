#pragma once

// Windows' "Create dump file" (#807): a full-memory minidump of a process, through dbghelp's
// MiniDumpWriteDump, as Task Manager writes one. WindowsProcessActions::createDump() opens and
// verifies the process; this writes the file. dbghelp.dll is loaded from System32 the first time a
// dump is written, never at startup, and never from beside TaskSmack.

#include "Platform/IProcessActions.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include <cstdint>
#include <filesystem>
#include <stop_token>

namespace Platform::Windows
{

/// The access createDump() opens the process with: what MiniDumpWriteDump() needs to read it.
inline constexpr DWORD DUMP_PROCESS_ACCESS = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ; // NOLINT(misc-include-cleaner)

/// Whether process @p pid is a protected process (PP or PPL: anti-malware, DRM, some system
/// services), whose memory no program may read. False when that can't be read.
[[nodiscard]] bool isProtectedProcess(DWORD pid); // NOLINT(misc-include-cleaner)

/// Writes a full-memory dump of @p process into a new file in @p directory: Task Manager's dump type
/// (full memory, handles, unloaded modules, memory and thread info), named for the process, its PID
/// and the time (DumpMath::dumpFileName()). The file is deleted again on any failure, and when
/// @p stopToken is requested before it is done.
/// @param process  An open handle with DUMP_PROCESS_ACCESS, already confirmed to be the target.
[[nodiscard]] ProcessDumpResult writeProcessDump(HANDLE process,
                                                 std::int32_t pid,
                                                 const std::filesystem::path& directory,
                                                 const std::stop_token& stopToken); // NOLINT(misc-include-cleaner)

} // namespace Platform::Windows
