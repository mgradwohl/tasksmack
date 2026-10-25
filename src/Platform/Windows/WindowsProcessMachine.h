#pragma once

// The machine a Windows process runs as (#2152, #2394), read from Windows: what "Create dump file"
// checks before it writes a dump (WindowsProcessDump.h), and what the process probe's Architecture
// column shows (WindowsProcessProbe.h). The decisions on what is read are MachineMath's
// (WindowsProcessMachineMath.h), which tests run on every platform.

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

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The machine (an IMAGE_FILE_MACHINE_* value) process @p process runs as, as MachineMath::processMachine()
/// reads it: from GetProcessInformation(ProcessMachineTypeInfo), else IsWow64Process2(). 0
/// (IMAGE_FILE_MACHINE_UNKNOWN) when neither can be read.
/// @param process  An open handle with PROCESS_QUERY_LIMITED_INFORMATION.
[[nodiscard]] std::uint16_t processMachine(HANDLE process);

/// processMachine()'s signature: DumpSource and WindowsProcessProbeFunctions hold it so a test can make
/// a process another architecture.
using ProcessMachineFn = std::uint16_t (*)(HANDLE process);

/// The machine Windows itself runs as (IsWow64Process2()'s native machine, for TaskSmack's own process):
/// ARM64 on Windows on ARM even for the x64 build emulated there. 0 (IMAGE_FILE_MACHINE_UNKNOWN) when it
/// can't be read.
[[nodiscard]] std::uint16_t nativeMachine();

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
