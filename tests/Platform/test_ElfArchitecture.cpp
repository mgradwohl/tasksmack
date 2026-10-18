/// @file test_ElfArchitecture.cpp
/// @brief A Linux process's architecture from its executable's ELF header (#2394), on byte fixtures:
/// each e_machine and class told apart, either byte order, short reads and files that aren't ELF, uname's
/// machine names, a qemu-user emulator's guest from its name, and the column's text built from them.

#include "Platform/Linux/ElfArchitecture.h"
#include "Platform/ProcessArchitecture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Platform
{
namespace
{

using ElfArchitecture::ELF_HEADER_PREFIX_BYTES;
using ElfArchitecture::emulatedMachine;
using ElfArchitecture::linuxArchitectureLabel;
using ElfArchitecture::machineFromElfHeader;
using ElfArchitecture::machineFromName;
using ProcessArchitecture::Machine;

constexpr std::uint8_t CLASS32 = 1;
constexpr std::uint8_t CLASS64 = 2;
constexpr std::uint8_t LITTLE = 1;
constexpr std::uint8_t BIG = 2;

/// The first ELF_HEADER_PREFIX_BYTES of an ELF header for @p machine (EM_*), of @p elfClass and byte order.
[[nodiscard]] std::array<std::uint8_t, ELF_HEADER_PREFIX_BYTES>
elfHeader(std::uint16_t machine, std::uint8_t elfClass = CLASS64, std::uint8_t byteOrder = LITTLE)
{
    std::array<std::uint8_t, ELF_HEADER_PREFIX_BYTES> header{0x7F, 'E', 'L', 'F', elfClass, byteOrder, 1}; // EV_CURRENT
    header[16] = 2;                                                                                        // e_type ET_EXEC
    const auto low = static_cast<std::uint8_t>(machine & 0xFFU);
    const auto high = static_cast<std::uint8_t>(machine >> 8U);
    header[18] = (byteOrder == LITTLE) ? low : high;
    header[19] = (byteOrder == LITTLE) ? high : low;
    return header;
}

TEST(ElfArchitectureTest, EachMachineAndClassIsToldApart)
{
    EXPECT_EQ(machineFromElfHeader(elfHeader(62)), Machine::X64);            // EM_X86_64
    EXPECT_EQ(machineFromElfHeader(elfHeader(62, CLASS32)), Machine::X32);   // The x32 ABI
    EXPECT_EQ(machineFromElfHeader(elfHeader(3, CLASS32)), Machine::X86);    // EM_386
    EXPECT_EQ(machineFromElfHeader(elfHeader(183)), Machine::Arm64);         // EM_AARCH64
    EXPECT_EQ(machineFromElfHeader(elfHeader(40, CLASS32)), Machine::Arm32); // EM_ARM
    EXPECT_EQ(machineFromElfHeader(elfHeader(243)), Machine::RiscV64);       // EM_RISCV
    EXPECT_EQ(machineFromElfHeader(elfHeader(243, CLASS32)), Machine::RiscV32);
    EXPECT_EQ(machineFromElfHeader(elfHeader(21)), Machine::PowerPC64); // EM_PPC64 (ppc64le)
    EXPECT_EQ(machineFromElfHeader(elfHeader(21, CLASS64, BIG)), Machine::PowerPC64);
    EXPECT_EQ(machineFromElfHeader(elfHeader(20, CLASS32, BIG)), Machine::PowerPC32); // EM_PPC
    EXPECT_EQ(machineFromElfHeader(elfHeader(22, CLASS64, BIG)), Machine::S390X);     // EM_S390
    EXPECT_EQ(machineFromElfHeader(elfHeader(22, CLASS32, BIG)), Machine::S390);
    EXPECT_EQ(machineFromElfHeader(elfHeader(258)), Machine::LoongArch64); // EM_LOONGARCH
    EXPECT_EQ(machineFromElfHeader(elfHeader(258, CLASS32)), Machine::Unknown);
    EXPECT_EQ(machineFromElfHeader(elfHeader(8, CLASS64, BIG)), Machine::Mips64); // EM_MIPS
    EXPECT_EQ(machineFromElfHeader(elfHeader(8, CLASS32)), Machine::Mips32);
    EXPECT_EQ(machineFromElfHeader(elfHeader(0x9026)), Machine::Unknown) << "Alpha: not told apart";
}

TEST(ElfArchitectureTest, ABigEndianMachineIsReadInItsOwnByteOrder)
{
    // 183 = 0x00B7: read little-endian from a big-endian header it would be 0xB700, no machine.
    auto header = elfHeader(183, CLASS64, BIG);
    EXPECT_EQ(machineFromElfHeader(header), Machine::Arm64);
    header[5] = LITTLE;
    EXPECT_EQ(machineFromElfHeader(header), Machine::Unknown);
}

TEST(ElfArchitectureTest, AShortReadOrAFileThatIsNotElfIsUnknown)
{
    const auto header = elfHeader(62);
    EXPECT_EQ(machineFromElfHeader(std::span(header).first(ELF_HEADER_PREFIX_BYTES - 1)), Machine::Unknown);
    EXPECT_EQ(machineFromElfHeader({}), Machine::Unknown);

    auto script = header;
    script[0] = '#';
    script[1] = '!';
    EXPECT_EQ(machineFromElfHeader(script), Machine::Unknown);

    auto badClass = header;
    badClass[4] = 3;
    EXPECT_EQ(machineFromElfHeader(badClass), Machine::Unknown);
    auto badOrder = header;
    badOrder[5] = 0;
    EXPECT_EQ(machineFromElfHeader(badOrder), Machine::Unknown);

    // More than the prefix is fine: only the prefix is looked at.
    std::array<std::uint8_t, 64> whole{};
    std::ranges::copy(header, whole.begin());
    EXPECT_EQ(machineFromElfHeader(whole), Machine::X64);
}

TEST(ElfArchitectureTest, UnameMachineNames)
{
    EXPECT_EQ(machineFromName("x86_64"), Machine::X64);
    EXPECT_EQ(machineFromName("i686"), Machine::X86);
    EXPECT_EQ(machineFromName("i386"), Machine::X86);
    EXPECT_EQ(machineFromName("aarch64"), Machine::Arm64);
    EXPECT_EQ(machineFromName("aarch64_be"), Machine::Arm64);
    EXPECT_EQ(machineFromName("armv7l"), Machine::Arm32);
    EXPECT_EQ(machineFromName("armv8l"), Machine::Arm32) << "a 32-bit kernel on an ARMv8 core";
    EXPECT_EQ(machineFromName("riscv64"), Machine::RiscV64);
    EXPECT_EQ(machineFromName("ppc64le"), Machine::PowerPC64);
    EXPECT_EQ(machineFromName("ppc"), Machine::PowerPC32);
    EXPECT_EQ(machineFromName("s390x"), Machine::S390X);
    EXPECT_EQ(machineFromName("loongarch64"), Machine::LoongArch64);
    EXPECT_EQ(machineFromName("mips64el"), Machine::Mips64);
    EXPECT_EQ(machineFromName("mipsel"), Machine::Mips32);
    EXPECT_EQ(machineFromName("sparc64"), Machine::Unknown);
    EXPECT_EQ(machineFromName(""), Machine::Unknown);
}

TEST(ElfArchitectureTest, AQemuUserEmulatorsNameGivesItsGuest)
{
    EXPECT_EQ(emulatedMachine("/usr/bin/qemu-x86_64"), Machine::X64);
    EXPECT_EQ(emulatedMachine("/usr/bin/qemu-x86_64-static"), Machine::X64);
    EXPECT_EQ(emulatedMachine("/usr/bin/qemu-i386-static"), Machine::X86);
    EXPECT_EQ(emulatedMachine("/usr/bin/qemu-aarch64"), Machine::Arm64);
    EXPECT_EQ(emulatedMachine("/usr/bin/qemu-arm-static"), Machine::Arm32);
    EXPECT_EQ(emulatedMachine("/usr/bin/qemu-riscv64-static (deleted)"), Machine::RiscV64);
    EXPECT_EQ(emulatedMachine("/usr/libexec/qemu-binfmt/x86_64-binfmt-P"), Machine::X64); // Debian's wrapper
    EXPECT_EQ(emulatedMachine("/usr/libexec/qemu-binfmt/aarch64-binfmt-P"), Machine::Arm64);
    // Not an emulator of a process.
    EXPECT_EQ(emulatedMachine("/usr/bin/qemu-system-x86_64"), Machine::Unknown);
    EXPECT_EQ(emulatedMachine("/usr/bin/bash"), Machine::Unknown);
    EXPECT_EQ(emulatedMachine("/usr/bin/qemu-img"), Machine::Unknown);
    EXPECT_EQ(emulatedMachine(""), Machine::Unknown);
}

TEST(ElfArchitectureTest, TheColumnsTextComparesWithTheHost)
{
    const auto x64 = elfHeader(62);
    const auto arm64 = elfHeader(183);
    const auto x86 = elfHeader(3, CLASS32);
    EXPECT_EQ(linuxArchitectureLabel(x64, "/usr/bin/bash", Machine::X64), "x64");
    EXPECT_EQ(linuxArchitectureLabel(arm64, "/usr/bin/bash", Machine::Arm64), "ARM64");
    EXPECT_EQ(linuxArchitectureLabel(x86, "/opt/old/app", Machine::X64), "x86 (32-bit)");
    EXPECT_EQ(linuxArchitectureLabel(elfHeader(40, CLASS32), "/opt/app", Machine::Arm64), "ARM32 (32-bit)");
    // A foreign binary whose exe is itself, not an emulator known by its name: emulated all the same.
    EXPECT_EQ(linuxArchitectureLabel(x64, "/opt/app", Machine::Arm64), "x64 (emulated)");
    // qemu-user through binfmt_misc: the exe is the native emulator, its guest is what runs.
    EXPECT_EQ(linuxArchitectureLabel(arm64, "/usr/bin/qemu-x86_64-static", Machine::Arm64), "x64 (emulated)");
    EXPECT_EQ(linuxArchitectureLabel(x64, "/usr/libexec/qemu-binfmt/aarch64-binfmt-P", Machine::X64), "ARM64 (emulated)");
    // Unreadable or not ELF: unknown, whatever the name says.
    EXPECT_TRUE(linuxArchitectureLabel({}, "/usr/bin/qemu-x86_64", Machine::Arm64).empty());
    EXPECT_TRUE(linuxArchitectureLabel({}, "", Machine::X64).empty());
    // An unknown host: the name alone.
    EXPECT_EQ(linuxArchitectureLabel(x86, "/opt/app", Machine::Unknown), "x86");
}

} // namespace
} // namespace Platform
