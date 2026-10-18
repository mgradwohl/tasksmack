/// @file test_ProcessArchitecture.cpp
/// @brief The Architecture column's text (#2394): every machine's name, native, ARM64EC and x32 counted
/// as native, a 32-bit program on its 64-bit sibling (WOW64 on Windows, 32-bit on Linux), everything
/// else emulated, an emulator's guest always emulated, and an unknown machine or host.

#include "Platform/ProcessArchitecture.h"

#include <gtest/gtest.h>

#include <string_view>

namespace Platform
{
namespace
{

using ProcessArchitecture::architectureLabel;
using ProcessArchitecture::Compat32Style;
using ProcessArchitecture::Machine;

TEST(ProcessArchitectureTest, EveryMachineHasAName)
{
    using ProcessArchitecture::machineName;
    EXPECT_EQ(machineName(Machine::X86), "x86");
    EXPECT_EQ(machineName(Machine::X64), "x64");
    EXPECT_EQ(machineName(Machine::X32), "x32");
    EXPECT_EQ(machineName(Machine::Arm32), "ARM32");
    EXPECT_EQ(machineName(Machine::Arm64), "ARM64");
    EXPECT_EQ(machineName(Machine::Arm64EC), "ARM64EC");
    EXPECT_EQ(machineName(Machine::RiscV32), "RISC-V 32");
    EXPECT_EQ(machineName(Machine::RiscV64), "RISC-V 64");
    EXPECT_EQ(machineName(Machine::PowerPC32), "PowerPC");
    EXPECT_EQ(machineName(Machine::PowerPC64), "PowerPC 64");
    EXPECT_EQ(machineName(Machine::S390), "s390");
    EXPECT_EQ(machineName(Machine::S390X), "s390x");
    EXPECT_EQ(machineName(Machine::LoongArch64), "LoongArch64");
    EXPECT_EQ(machineName(Machine::Mips32), "MIPS");
    EXPECT_EQ(machineName(Machine::Mips64), "MIPS64");
    EXPECT_TRUE(machineName(Machine::Unknown).empty());
}

TEST(ProcessArchitectureTest, ANativeProcessIsItsNameAlone)
{
    for (const Machine machine : {Machine::X64, Machine::Arm64, Machine::X86, Machine::RiscV64, Machine::S390X})
    {
        EXPECT_EQ(architectureLabel(machine, machine, Compat32Style::Wow64), ProcessArchitecture::machineName(machine));
        EXPECT_EQ(architectureLabel(machine, machine, Compat32Style::Bits32), ProcessArchitecture::machineName(machine));
    }
    // ARM64EC is ARM64 code, and x32 is x86-64 code: neither is emulated.
    EXPECT_EQ(architectureLabel(Machine::Arm64EC, Machine::Arm64, Compat32Style::Wow64), "ARM64EC");
    EXPECT_EQ(architectureLabel(Machine::X32, Machine::X64, Compat32Style::Bits32), "x32");
}

TEST(ProcessArchitectureTest, AThirtyTwoBitProgramOnItsSixtyFourBitSiblingIsWow64OrThirtyTwoBit)
{
    EXPECT_EQ(architectureLabel(Machine::X86, Machine::X64, Compat32Style::Wow64), "x86 (WOW64)");
    EXPECT_EQ(architectureLabel(Machine::Arm32, Machine::Arm64, Compat32Style::Wow64), "ARM32 (WOW64)");
    EXPECT_EQ(architectureLabel(Machine::X86, Machine::X64, Compat32Style::Bits32), "x86 (32-bit)");
    EXPECT_EQ(architectureLabel(Machine::Arm32, Machine::Arm64, Compat32Style::Bits32), "ARM32 (32-bit)");
    EXPECT_EQ(architectureLabel(Machine::RiscV32, Machine::RiscV64, Compat32Style::Bits32), "RISC-V 32 (32-bit)");
    EXPECT_EQ(architectureLabel(Machine::PowerPC32, Machine::PowerPC64, Compat32Style::Bits32), "PowerPC (32-bit)");
    EXPECT_EQ(architectureLabel(Machine::S390, Machine::S390X, Compat32Style::Bits32), "s390 (32-bit)");
    EXPECT_EQ(architectureLabel(Machine::Mips32, Machine::Mips64, Compat32Style::Bits32), "MIPS (32-bit)");
}

TEST(ProcessArchitectureTest, AnyOtherMachineIsEmulated)
{
    EXPECT_EQ(architectureLabel(Machine::X64, Machine::Arm64, Compat32Style::Wow64), "x64 (emulated)");
    EXPECT_EQ(architectureLabel(Machine::X86, Machine::Arm64, Compat32Style::Wow64), "x86 (emulated)");
    EXPECT_EQ(architectureLabel(Machine::Arm64, Machine::X64, Compat32Style::Bits32), "ARM64 (emulated)");
    EXPECT_EQ(architectureLabel(Machine::RiscV64, Machine::X64, Compat32Style::Bits32), "RISC-V 64 (emulated)");
    // ARM64EC is not native to x64; x32 is not native to ARM64.
    EXPECT_EQ(architectureLabel(Machine::Arm64EC, Machine::X64, Compat32Style::Wow64), "ARM64EC (emulated)");
    EXPECT_EQ(architectureLabel(Machine::X32, Machine::Arm64, Compat32Style::Bits32), "x32 (emulated)");
}

TEST(ProcessArchitectureTest, AnEmulatorsGuestIsEmulatedWhateverTheHost)
{
    EXPECT_EQ(architectureLabel(Machine::X64, Machine::Arm64, Compat32Style::Bits32, /*viaEmulator=*/true), "x64 (emulated)");
    EXPECT_EQ(architectureLabel(Machine::X86, Machine::X64, Compat32Style::Bits32, /*viaEmulator=*/true), "x86 (emulated)");
    EXPECT_EQ(architectureLabel(Machine::X64, Machine::X64, Compat32Style::Bits32, /*viaEmulator=*/true), "x64 (emulated)");
    EXPECT_EQ(architectureLabel(Machine::Arm64, Machine::Unknown, Compat32Style::Bits32, /*viaEmulator=*/true), "ARM64 (emulated)");
}

TEST(ProcessArchitectureTest, AnUnknownMachineIsEmptyAndAnUnknownHostNamesItAlone)
{
    EXPECT_TRUE(architectureLabel(Machine::Unknown, Machine::X64, Compat32Style::Wow64).empty());
    EXPECT_TRUE(architectureLabel(Machine::Unknown, Machine::Unknown, Compat32Style::Bits32, /*viaEmulator=*/true).empty());
    EXPECT_EQ(architectureLabel(Machine::X86, Machine::Unknown, Compat32Style::Wow64), "x86");
    EXPECT_EQ(architectureLabel(Machine::X64, Machine::Unknown, Compat32Style::Bits32), "x64");
}

TEST(ProcessArchitectureTest, OnlySixtyFourBitMachinesHaveAThirtyTwoBitSibling)
{
    using ProcessArchitecture::compat32Of;
    EXPECT_EQ(compat32Of(Machine::X64), Machine::X86);
    EXPECT_EQ(compat32Of(Machine::Arm64), Machine::Arm32);
    EXPECT_EQ(compat32Of(Machine::X86), Machine::Unknown);
    EXPECT_EQ(compat32Of(Machine::Arm64EC), Machine::Unknown);
    EXPECT_EQ(compat32Of(Machine::LoongArch64), Machine::Unknown);
    EXPECT_EQ(compat32Of(Machine::Unknown), Machine::Unknown);
}

} // namespace
} // namespace Platform
