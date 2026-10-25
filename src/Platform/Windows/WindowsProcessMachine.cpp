#include "WindowsProcessMachine.h"

#include "WindowsProcessMachineMath.h"

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

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers

std::uint16_t processMachine(HANDLE process)
{
    // ProcessMachineTypeInfo (Windows 11 and later) tells an emulated x64 process from a native ARM64
    // one; IsWow64Process2() can't (it says neither is WOW64), so it is only the fallback, for Windows 10.
    // Zero-filled for GetProcessInformation() to fill in; MACHINE_ATTRIBUTES has no zero enumerator, but
    // "no attributes" is what Windows itself writes there.
    PROCESS_MACHINE_INFORMATION info{}; // NOLINT(bugprone-invalid-enum-default-initialization)
    const std::uint16_t machineTypeInfo = (GetProcessInformation(process, ProcessMachineTypeInfo, &info, sizeof(info)) != 0)
                                            ? info.ProcessMachine
                                            : MachineMath::MACHINE_UNKNOWN;
    USHORT wow64Machine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT native = IMAGE_FILE_MACHINE_UNKNOWN;
    if (machineTypeInfo == MachineMath::MACHINE_UNKNOWN && IsWow64Process2(process, &wow64Machine, &native) == 0)
    {
        wow64Machine = IMAGE_FILE_MACHINE_UNKNOWN;
        native = IMAGE_FILE_MACHINE_UNKNOWN;
    }
    return MachineMath::processMachine(machineTypeInfo, wow64Machine, native);
}

std::uint16_t nativeMachine()
{
    // Read once: the machine Windows runs as can't change while TaskSmack runs.
    static const std::uint16_t machine = []
    {
        USHORT self = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT native = IMAGE_FILE_MACHINE_UNKNOWN;
        return (IsWow64Process2(GetCurrentProcess(), &self, &native) != 0) ? std::uint16_t{native} : MachineMath::MACHINE_UNKNOWN;
    }();
    return machine;
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
