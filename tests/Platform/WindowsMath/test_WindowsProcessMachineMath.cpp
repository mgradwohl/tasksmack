/// @file test_WindowsProcessMachineMath.cpp
/// @brief The machine a Windows process runs as (#2152, #2394), as plain values: each
/// IMAGE_FILE_MACHINE_* value's machine, which read wins, and the Architecture column's text for every
/// architecture on x64 and ARM64 Windows -- native, ARM64EC, emulated, WOW64 and unknown. Built on every
/// platform (#1133).

#include "Platform/ProcessArchitecture.h"
#include "Platform/Windows/WindowsProcessMachineMath.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

using MachineMath::windowsArchitectureLabel;
using ProcessArchitecture::Machine;
namespace M = MachineMath;

TEST(WindowsProcessMachineMathTest, EachImageFileMachineIsAMachine)
{
    EXPECT_EQ(M::toMachine(M::MACHINE_I386), Machine::X86);
    EXPECT_EQ(M::toMachine(M::MACHINE_AMD64), Machine::X64);
    EXPECT_EQ(M::toMachine(M::MACHINE_ARM), Machine::Arm32);
    EXPECT_EQ(M::toMachine(M::MACHINE_THUMB), Machine::Arm32);
    EXPECT_EQ(M::toMachine(M::MACHINE_ARMNT), Machine::Arm32);
    EXPECT_EQ(M::toMachine(M::MACHINE_ARM64), Machine::Arm64);
    EXPECT_EQ(M::toMachine(M::MACHINE_ARM64X), Machine::Arm64);
    EXPECT_EQ(M::toMachine(M::MACHINE_ARM64EC), Machine::Arm64EC);
    EXPECT_EQ(M::toMachine(M::MACHINE_UNKNOWN), Machine::Unknown);
    EXPECT_EQ(M::toMachine(0x0200), Machine::Unknown) << "IA64: not told apart";
}

TEST(WindowsProcessMachineMathTest, MachineTypeInfoWinsThenTheWow64GuestThenTheNativeMachine)
{
    EXPECT_EQ(M::processMachine(M::MACHINE_AMD64, M::MACHINE_UNKNOWN, M::MACHINE_ARM64), M::MACHINE_AMD64);
    EXPECT_EQ(M::processMachine(M::MACHINE_UNKNOWN, M::MACHINE_I386, M::MACHINE_AMD64), M::MACHINE_I386);
    EXPECT_EQ(M::processMachine(M::MACHINE_UNKNOWN, M::MACHINE_UNKNOWN, M::MACHINE_AMD64), M::MACHINE_AMD64);
    EXPECT_EQ(M::processMachine(M::MACHINE_UNKNOWN, M::MACHINE_UNKNOWN, M::MACHINE_UNKNOWN), M::MACHINE_UNKNOWN);
}

TEST(WindowsProcessMachineMathTest, OnX64Windows)
{
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_AMD64, M::MACHINE_AMD64), "x64");
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_I386, M::MACHINE_AMD64), "x86 (WOW64)");
}

TEST(WindowsProcessMachineMathTest, OnArm64Windows)
{
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_ARM64, M::MACHINE_ARM64), "ARM64");
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_ARM64X, M::MACHINE_ARM64), "ARM64");
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_ARM64EC, M::MACHINE_ARM64), "ARM64EC");
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_AMD64, M::MACHINE_ARM64), "x64 (emulated)");
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_I386, M::MACHINE_ARM64), "x86 (emulated)");
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_ARMNT, M::MACHINE_ARM64), "ARM32 (WOW64)");
}

TEST(WindowsProcessMachineMathTest, AnUnknownMachineIsEmptyAndAnUnknownNativeOneNamesItAlone)
{
    EXPECT_TRUE(windowsArchitectureLabel(M::MACHINE_UNKNOWN, M::MACHINE_AMD64).empty());
    EXPECT_TRUE(windowsArchitectureLabel(0x0200, M::MACHINE_AMD64).empty());
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_I386, M::MACHINE_UNKNOWN), "x86");
    EXPECT_EQ(windowsArchitectureLabel(M::MACHINE_AMD64, M::MACHINE_UNKNOWN), "x64");
}

} // namespace
} // namespace Platform
