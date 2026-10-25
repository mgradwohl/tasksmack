#pragma once

// The architecture a Linux process runs as (#2394), from the ELF header of its executable
// (/proc/[pid]/exe): e_machine and the class (32- or 64-bit), compared against the host's machine
// (uname). A process run by a user-mode emulator through binfmt_misc (qemu-user) has the emulator
// itself as its exe, so an executable named for one -- "qemu-x86_64-static", or Debian's
// "x86_64-binfmt-P" -- is taken for the guest machine its name gives, emulated. Pure, so it is
// unit-tested on byte fixtures on every platform; LinuxProcessProbe reads the bytes.

#include "Platform/ProcessArchitecture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Platform::ElfArchitecture
{

/// The bytes of an ELF header read: e_ident (16) and e_type (2) before e_machine (2).
inline constexpr std::size_t ELF_HEADER_PREFIX_BYTES = 20;

namespace Detail
{
inline constexpr std::size_t EI_CLASS = 4;
inline constexpr std::size_t EI_DATA = 5;
inline constexpr std::size_t E_MACHINE = 18;
inline constexpr std::uint8_t ELFCLASS32 = 1;
inline constexpr std::uint8_t ELFCLASS64 = 2;
inline constexpr std::uint8_t ELFDATA2LSB = 1;
inline constexpr std::uint8_t ELFDATA2MSB = 2;

// The e_machine values (elf.h) told apart.
inline constexpr std::uint16_t EM_386 = 3;
inline constexpr std::uint16_t EM_MIPS = 8;
inline constexpr std::uint16_t EM_PPC = 20;
inline constexpr std::uint16_t EM_PPC64 = 21;
inline constexpr std::uint16_t EM_S390 = 22;
inline constexpr std::uint16_t EM_ARM = 40;
inline constexpr std::uint16_t EM_X86_64 = 62;
inline constexpr std::uint16_t EM_AARCH64 = 183;
inline constexpr std::uint16_t EM_RISCV = 243;
inline constexpr std::uint16_t EM_LOONGARCH = 258;
} // namespace Detail

/// The machine an ELF header's first ELF_HEADER_PREFIX_BYTES (or more) say its code is for; Unknown for
/// a short read, a file that isn't ELF, or a machine not told apart.
[[nodiscard]] constexpr ProcessArchitecture::Machine machineFromElfHeader(std::span<const std::uint8_t> header) noexcept
{
    using Detail::E_MACHINE;
    using Detail::EI_CLASS;
    using Detail::EI_DATA;
    using Detail::ELFCLASS32;
    using Detail::ELFCLASS64;
    using Detail::ELFDATA2LSB;
    using Detail::ELFDATA2MSB;
    using ProcessArchitecture::Machine;
    constexpr std::array<std::uint8_t, 4> MAGIC{0x7F, 'E', 'L', 'F'};
    if (header.size() < ELF_HEADER_PREFIX_BYTES || header[0] != MAGIC[0] || header[1] != MAGIC[1] || header[2] != MAGIC[2] ||
        header[3] != MAGIC[3])
    {
        return Machine::Unknown;
    }
    const std::uint8_t elfClass = header[EI_CLASS];
    const std::uint8_t data = header[EI_DATA];
    if ((elfClass != ELFCLASS32 && elfClass != ELFCLASS64) || (data != ELFDATA2LSB && data != ELFDATA2MSB))
    {
        return Machine::Unknown;
    }
    const auto low = static_cast<std::uint16_t>(header[(data == ELFDATA2LSB) ? E_MACHINE : E_MACHINE + 1]);
    const auto high = static_cast<std::uint16_t>(header[(data == ELFDATA2LSB) ? E_MACHINE + 1 : E_MACHINE]);
    const auto machine = static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));
    const bool is64 = elfClass == ELFCLASS64;
    switch (machine)
    {
    case Detail::EM_386:
        return Machine::X86;
    case Detail::EM_X86_64:
        return is64 ? Machine::X64 : Machine::X32;
    case Detail::EM_ARM:
        return Machine::Arm32;
    case Detail::EM_AARCH64:
        return Machine::Arm64;
    case Detail::EM_RISCV:
        return is64 ? Machine::RiscV64 : Machine::RiscV32;
    case Detail::EM_PPC:
        return Machine::PowerPC32;
    case Detail::EM_PPC64:
        return Machine::PowerPC64;
    case Detail::EM_S390:
        return is64 ? Machine::S390X : Machine::S390;
    case Detail::EM_LOONGARCH:
        return is64 ? Machine::LoongArch64 : Machine::Unknown;
    case Detail::EM_MIPS:
        return is64 ? Machine::Mips64 : Machine::Mips32;
    default:
        return Machine::Unknown;
    }
}

/// The machine a uname(2) machine string, or the architecture in an emulator's name, names: "x86_64",
/// "i686", "aarch64", "armv7l", "riscv64", "ppc64le", "s390x", "loongarch64", "mips64el"; Unknown for
/// any other.
[[nodiscard]] constexpr ProcessArchitecture::Machine machineFromName(std::string_view name) noexcept
{
    using ProcessArchitecture::Machine;
    if (name == "x86_64" || name == "amd64")
    {
        return Machine::X64;
    }
    if (name == "i386" || name == "i486" || name == "i586" || name == "i686" || name == "x86")
    {
        return Machine::X86;
    }
    if (name.starts_with("aarch64") || name == "arm64")
    {
        return Machine::Arm64;
    }
    if (name.starts_with("arm"))
    {
        return Machine::Arm32;
    }
    if (name == "riscv64")
    {
        return Machine::RiscV64;
    }
    if (name == "riscv32")
    {
        return Machine::RiscV32;
    }
    if (name.starts_with("ppc64"))
    {
        return Machine::PowerPC64;
    }
    if (name == "ppc")
    {
        return Machine::PowerPC32;
    }
    if (name == "s390x")
    {
        return Machine::S390X;
    }
    if (name == "s390")
    {
        return Machine::S390;
    }
    if (name == "loongarch64")
    {
        return Machine::LoongArch64;
    }
    if (name.starts_with("mips64"))
    {
        return Machine::Mips64;
    }
    if (name.starts_with("mips"))
    {
        return Machine::Mips32;
    }
    return Machine::Unknown;
}

/// The guest machine a user-mode emulator at @p exePath (/proc/[pid]/exe's target) runs, or Unknown if
/// it isn't one: "qemu-<arch>" or "qemu-<arch>-static", and Debian's binfmt wrapper "<arch>-binfmt-P".
/// A whole-system emulator ("qemu-system-x86_64") runs a VM, not the process, and isn't one.
[[nodiscard]] constexpr ProcessArchitecture::Machine emulatedMachine(std::string_view exePath) noexcept
{
    constexpr std::string_view DELETED = " (deleted)"; // How the kernel marks an exe deleted since it ran
    if (exePath.ends_with(DELETED))
    {
        exePath.remove_suffix(DELETED.size());
    }
    if (const std::size_t slash = exePath.rfind('/'); slash != std::string_view::npos)
    {
        exePath.remove_prefix(slash + 1);
    }
    std::string_view arch;
    constexpr std::string_view QEMU = "qemu-";
    if (exePath.starts_with(QEMU))
    {
        arch = exePath;
        arch.remove_prefix(QEMU.size());
        if (arch.starts_with("system-"))
        {
            return ProcessArchitecture::Machine::Unknown;
        }
        constexpr std::string_view STATIC = "-static";
        if (arch.ends_with(STATIC))
        {
            arch.remove_suffix(STATIC.size());
        }
    }
    else if (const std::size_t binfmt = exePath.find("-binfmt"); binfmt != std::string_view::npos)
    {
        arch = exePath;
        arch.remove_suffix(exePath.size() - binfmt);
    }
    return machineFromName(arch);
}

/// The Architecture column's text for a process whose executable starts with @p header and is at
/// @p exePath, on a @p host machine: "x64", "x86 (32-bit)" on x64, "x64 (emulated)" through qemu-user on
/// ARM64. Empty when the header can't be read (another user's process without CAP_SYS_PTRACE, a kernel
/// thread, which has no exe) or names no machine told apart.
[[nodiscard]] inline std::string
linuxArchitectureLabel(std::span<const std::uint8_t> header, std::string_view exePath, ProcessArchitecture::Machine host)
{
    using ProcessArchitecture::Machine;
    const Machine machine = machineFromElfHeader(header);
    if (machine == Machine::Unknown)
    {
        return {};
    }
    if (const Machine guest = emulatedMachine(exePath); guest != Machine::Unknown)
    {
        return ProcessArchitecture::architectureLabel(guest, host, ProcessArchitecture::Compat32Style::Bits32, /*viaEmulator=*/true);
    }
    return ProcessArchitecture::architectureLabel(machine, host, ProcessArchitecture::Compat32Style::Bits32);
}

} // namespace Platform::ElfArchitecture
