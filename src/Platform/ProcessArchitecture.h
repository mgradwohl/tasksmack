#pragma once

// The architecture a process runs as (#2394), as the Processes table's Architecture column and Process
// Details' Architecture row show it: the machine its code is for, and how it runs on this one -- natively,
// as a 32-bit program on a 64-bit OS (WOW64 on Windows), or emulated (x64 on ARM64). Each platform reads
// the machine its own way (Windows: WindowsProcessMachine.h; Linux: ElfArchitecture.h) and words it here,
// so the two agree. Pure, so it is unit-tested on every platform.

#include <cstdint>
#include <string>
#include <string_view>

namespace Platform::ProcessArchitecture
{

/// The machines a process's code can be for.
enum class Machine : std::uint8_t
{
    Unknown,
    X86,         ///< 32-bit x86 (i386)
    X64,         ///< x86-64 (AMD64)
    X32,         ///< Linux's x32 ABI: x86-64 code with 32-bit pointers
    Arm32,       ///< 32-bit ARM (Windows' ARMNT, Linux's EM_ARM)
    Arm64,       ///< AArch64 (Windows' ARM64 and ARM64X)
    Arm64EC,     ///< Windows' ARM64EC: ARM64 code that can load x64 code beside it
    RiscV32,     ///< 32-bit RISC-V
    RiscV64,     ///< 64-bit RISC-V
    PowerPC32,   ///< 32-bit PowerPC
    PowerPC64,   ///< 64-bit PowerPC, either byte order
    S390,        ///< 31-bit IBM Z
    S390X,       ///< 64-bit IBM Z
    LoongArch64, ///< 64-bit LoongArch
    Mips32,      ///< 32-bit MIPS
    Mips64,      ///< 64-bit MIPS
};

/// How a 32-bit program its 64-bit OS runs natively is marked: "(WOW64)", Windows' name for the layer
/// that runs one, or "(32-bit)" on Linux, whose kernel runs one with no layer of its own.
enum class Compat32Style : std::uint8_t
{
    Wow64,
    Bits32,
};

/// @p machine's name in the column: "x64", "ARM64", "ARM64EC"; empty for Unknown.
[[nodiscard]] constexpr std::string_view machineName(Machine machine) noexcept
{
    switch (machine)
    {
    case Machine::X86:
        return "x86";
    case Machine::X64:
        return "x64";
    case Machine::X32:
        return "x32";
    case Machine::Arm32:
        return "ARM32";
    case Machine::Arm64:
        return "ARM64";
    case Machine::Arm64EC:
        return "ARM64EC";
    case Machine::RiscV32:
        return "RISC-V 32";
    case Machine::RiscV64:
        return "RISC-V 64";
    case Machine::PowerPC32:
        return "PowerPC";
    case Machine::PowerPC64:
        return "PowerPC 64";
    case Machine::S390:
        return "s390";
    case Machine::S390X:
        return "s390x";
    case Machine::LoongArch64:
        return "LoongArch64";
    case Machine::Mips32:
        return "MIPS";
    case Machine::Mips64:
        return "MIPS64";
    case Machine::Unknown:
        break;
    }
    return {};
}

/// The 32-bit machine a 64-bit @p native one runs natively (x86 on x64, ARM32 on ARM64), or Unknown.
[[nodiscard]] constexpr Machine compat32Of(Machine native) noexcept
{
    switch (native)
    {
    case Machine::X64:
        return Machine::X86;
    case Machine::Arm64:
        return Machine::Arm32;
    case Machine::RiscV64:
        return Machine::RiscV32;
    case Machine::PowerPC64:
        return Machine::PowerPC32;
    case Machine::S390X:
        return Machine::S390;
    case Machine::Mips64:
        return Machine::Mips32;
    default:
        return Machine::Unknown;
    }
}

/// Whether code for @p process runs natively on a @p native machine, with no emulation: the same
/// machine, ARM64EC on ARM64, and x32 on x64. A 32-bit program on its 64-bit sibling is compat32Of().
[[nodiscard]] constexpr bool runsNatively(Machine process, Machine native) noexcept
{
    return process == native || (process == Machine::Arm64EC && native == Machine::Arm64) ||
           (process == Machine::X32 && native == Machine::X64);
}

/// The Architecture column's text for a process whose code is for @p process, on a @p native machine:
/// - its name alone when it runs natively (runsNatively()), or when the native machine is Unknown;
/// - "x86 (WOW64)" / "x86 (32-bit)" (@p style) for the 32-bit sibling of a 64-bit machine (compat32Of());
/// - "x64 (emulated)" for any other, and whenever @p viaEmulator says an emulator runs it (qemu-user).
/// Empty when @p process is Unknown: the column then shows it couldn't be read.
[[nodiscard]] inline std::string architectureLabel(Machine process, Machine native, Compat32Style style, bool viaEmulator = false)
{
    const std::string_view name = machineName(process);
    if (name.empty())
    {
        return {};
    }
    std::string label(name);
    if (!viaEmulator && (native == Machine::Unknown || runsNatively(process, native)))
    {
        return label;
    }
    if (!viaEmulator && process == compat32Of(native))
    {
        label += (style == Compat32Style::Wow64) ? " (WOW64)" : " (32-bit)";
        return label;
    }
    label += " (emulated)";
    return label;
}

} // namespace Platform::ProcessArchitecture
