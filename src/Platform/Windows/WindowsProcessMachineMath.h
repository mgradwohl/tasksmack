#pragma once

// The pure parts of the machine a Windows process runs as (#2152, #2394): the IMAGE_FILE_MACHINE_*
// values Windows reports it in, which of its reads to believe, and the Architecture column's text for
// it. "Create dump file" refuses a process of another architecture from these (WindowsProcessDumpMath.h),
// and the process probe shows them. Plain values, so it reaches no Windows header and its tests build and
// run on every platform (#1133); the reads themselves are WindowsProcessMachine.h's.

#include "Platform/ProcessArchitecture.h"

#include <cstdint>
#include <string>

namespace Platform::MachineMath
{

/// The IMAGE_FILE_MACHINE_* values (winnt.h) told apart, as plain values.
inline constexpr std::uint16_t MACHINE_UNKNOWN = 0x0000;
inline constexpr std::uint16_t MACHINE_I386 = 0x014C;
inline constexpr std::uint16_t MACHINE_ARM = 0x01C0;   ///< 32-bit ARM
inline constexpr std::uint16_t MACHINE_THUMB = 0x01C2; ///< 32-bit ARM (Thumb)
inline constexpr std::uint16_t MACHINE_ARMNT = 0x01C4; ///< 32-bit ARM (Thumb-2): what Windows on ARM ran
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

/// The ProcessArchitecture machine of an IMAGE_FILE_MACHINE_* value; Unknown for any other. ARM64X, a
/// binary holding both ARM64 and ARM64EC code, runs as ARM64 in a process of its own.
[[nodiscard]] constexpr ProcessArchitecture::Machine toMachine(std::uint16_t imageFileMachine) noexcept
{
    using ProcessArchitecture::Machine;
    switch (imageFileMachine)
    {
    case MACHINE_I386:
        return Machine::X86;
    case MACHINE_AMD64:
        return Machine::X64;
    case MACHINE_ARM:
    case MACHINE_THUMB:
    case MACHINE_ARMNT:
        return Machine::Arm32;
    case MACHINE_ARM64:
    case MACHINE_ARM64X:
        return Machine::Arm64;
    case MACHINE_ARM64EC:
        return Machine::Arm64EC;
    default:
        return Machine::Unknown;
    }
}

/// The Architecture column's text (#2394) for a process running as @p processMachine (processMachine())
/// on Windows running as @p nativeMachine (IsWow64Process2()'s native machine): "x64", "ARM64EC",
/// "x64 (emulated)" on ARM64, "x86 (WOW64)" on x64, "x86 (emulated)" on ARM64. Empty when the process's
/// machine is unknown.
[[nodiscard]] inline std::string windowsArchitectureLabel(std::uint16_t processMachine, std::uint16_t nativeMachine)
{
    return ProcessArchitecture::architectureLabel(
        toMachine(processMachine), toMachine(nativeMachine), ProcessArchitecture::Compat32Style::Wow64);
}

} // namespace Platform::MachineMath
