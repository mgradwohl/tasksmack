#pragma once

// The Windows Security facts (#1514), read only and unprivileged:
// - Secure Boot from HKLM\SYSTEM\CurrentControlSet\Control\SecureBoot\State\UEFISecureBootEnabled, and "not
//   supported" on a legacy BIOS boot (GetFirmwareType);
// - the TPM and its version from Tbsi_GetDeviceInfo (tbs.dll, loaded at run time from System32);
// - memory integrity (HVCI) from SystemCodeIntegrityInformation, decoded as the CPU details' row is (#809);
// - Kernel DMA Protection from SystemDmaGuardPolicyInformation;
// - the CPU's speculative-execution vulnerabilities from SystemSpeculationControlInformation and
//   SystemKernelVaShadowInformation, as Microsoft's SpeculationControl script reads them, under the names
//   and status wording the Linux kernel uses, so the section reads the same on both. Every status comes
//   from what Windows reports; where it doesn't say whether the CPU needs a mitigation, the status is
//   "Unknown" rather than a guess from the CPU's vendor (#1781).
// - each fixed volume's BitLocker protection status from the shell's System.Volume.BitLockerProtection
//   property, which Explorer reads for its drive icons and which needs no administrator (#2584).
// Every call goes through an injectable table; WindowsPlatformSecurity.cpp supplies the real one, tests
// substitute fakes. The decoding is standard library only, so its tests run on every platform.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsSystemProbeMath.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::WindowsPlatformSecurity
{

// NtQuerySystemInformation's classes.
inline constexpr std::uint32_t SYSTEM_CODE_INTEGRITY_INFORMATION = 103;
inline constexpr std::uint32_t SYSTEM_KERNEL_VA_SHADOW_INFORMATION = 196;
inline constexpr std::uint32_t SYSTEM_SPECULATION_CONTROL_INFORMATION = 201;
inline constexpr std::uint32_t SYSTEM_DMA_GUARD_POLICY_INFORMATION = 202;

// Tbsi_GetDeviceInfo's results and TPM_DEVICE_INFO::tpmVersion.
inline constexpr std::uint32_t TBS_SUCCESS_CODE = 0;
inline constexpr std::uint32_t TBS_E_TPM_NOT_FOUND_CODE = 0x8028'400FU;
inline constexpr std::uint32_t TPM_VERSION_12_CODE = 1;
inline constexpr std::uint32_t TPM_VERSION_20_CODE = 2;

// The speculation-control bits, as Microsoft's SpeculationControl PowerShell module names and reads them
// (Get-SpeculationControlSettings in SpeculationControl.psm1, https://github.com/microsoft/SpeculationControl),
// at the positions phnt's ntexapi.h gives SYSTEM_SPECULATION_CONTROL_INFORMATION's and
// SYSTEM_KERNEL_VA_SHADOW_INFORMATION's bitfields; the Windows SDK declares neither. The module is the
// reference for what each bit means. Where it falls back on the CPU's vendor or model because Windows didn't
// say (KvaShadowRequired before Windows reported it, L1TF, AMD's and ARM's MDS and MMIO), this file reports
// Unknown instead.

/// SYSTEM_SPECULATION_CONTROL_INFORMATION: two ULONG flag words. Builds before Windows 11 23H2 know only the
/// first; the second then reads as zero, which reports nothing, because every field in it is gated by a
/// "...Reported" bit or read only as "protected".
struct SpeculationControlInformation
{
    std::uint32_t flags = 0;  ///< SpeculationControlFlags
    std::uint32_t flags2 = 0; ///< SpeculationControlFlags2 (since 23H2)
};
static_assert(sizeof(std::uint32_t) == 4); // a ULONG
static_assert(sizeof(SpeculationControlInformation) == 8);
static_assert(offsetof(SpeculationControlInformation, flags2) == 4);

// SpeculationControlFlags (the module's $scf*).
inline constexpr std::uint32_t SPEC_BPB_ENABLED = 0x0000'0001U;                      // bit 0
inline constexpr std::uint32_t SPEC_BPB_DISABLED_SYSTEM_POLICY = 0x0000'0002U;       // bit 1
inline constexpr std::uint32_t SPEC_BPB_DISABLED_NO_HARDWARE_SUPPORT = 0x0000'0004U; // bit 2
inline constexpr std::uint32_t SPEC_SPEC_CTRL_ENUMERATED = 0x0000'0008U;             // bit 3
inline constexpr std::uint32_t SPEC_SPEC_CMD_ENUMERATED = 0x0000'0010U;              // bit 4
inline constexpr std::uint32_t SPEC_IBRS_PRESENT = 0x0000'0020U;                     // bit 5
inline constexpr std::uint32_t SPEC_STIBP_PRESENT = 0x0000'0040U;                    // bit 6
inline constexpr std::uint32_t SPEC_SSBD_AVAILABLE = 0x0000'0100U;                   // bit 8: Windows supports SSBD
inline constexpr std::uint32_t SPEC_SSBD_SUPPORTED = 0x0000'0200U;                   // bit 9: the CPU supports it
inline constexpr std::uint32_t SPEC_SSBD_SYSTEM_WIDE = 0x0000'0400U;                 // bit 10
inline constexpr std::uint32_t SPEC_SSBD_REQUIRED = 0x0000'1000U;                    // bit 12
inline constexpr std::uint32_t SPEC_RETPOLINE_ENABLED = 0x0000'4000U;                // bit 14
inline constexpr std::uint32_t SPEC_ENHANCED_IBRS = 0x0001'0000U;                    // bit 16
inline constexpr std::uint32_t SPEC_HV_L1TF_STATUS_AVAILABLE = 0x0002'0000U;         // bit 17
inline constexpr std::uint32_t SPEC_HV_L1TF_PROCESSOR_NOT_AFFECTED = 0x0004'0000U;   // bit 18
inline constexpr std::uint32_t SPEC_MDS_HARDWARE_PROTECTED = 0x0100'0000U;           // bit 24
inline constexpr std::uint32_t SPEC_MB_CLEAR_ENABLED = 0x0200'0000U;                 // bit 25
inline constexpr std::uint32_t SPEC_MB_CLEAR_REPORTED = 0x0400'0000U;                // bit 26

// SpeculationControlFlags2 (the module's $scf2*).
inline constexpr std::uint32_t SPEC2_SBDR_SSDP_HARDWARE_PROTECTED = 0x0000'0001U;     // bit 0
inline constexpr std::uint32_t SPEC2_FBSDP_HARDWARE_PROTECTED = 0x0000'0002U;         // bit 1
inline constexpr std::uint32_t SPEC2_PSDP_HARDWARE_PROTECTED = 0x0000'0004U;          // bit 2
inline constexpr std::uint32_t SPEC2_FB_CLEAR_ENABLED = 0x0000'0008U;                 // bit 3
inline constexpr std::uint32_t SPEC2_FB_CLEAR_REPORTED = 0x0000'0010U;                // bit 4
inline constexpr std::uint32_t SPEC2_RDCL_HARDWARE_PROTECTED_REPORTED = 0x0000'0800U; // bit 11
inline constexpr std::uint32_t SPEC2_RDCL_HARDWARE_PROTECTED = 0x0000'1000U;          // bit 12
inline constexpr std::uint32_t SPEC2_GDS_REPORTED = 0x0000'2000U;                     // bit 13
inline constexpr std::uint32_t SPEC2_GDS_STATUS_MASK = 0x0001'C000U;                  // bits 14-16
inline constexpr std::uint32_t SPEC2_GDS_STATUS_SHIFT = 14;
inline constexpr std::uint32_t SPEC2_SRSO_REPORTED = 0x0002'0000U;    // bit 17
inline constexpr std::uint32_t SPEC2_SRSO_STATUS_MASK = 0x000C'0000U; // bits 18-19
inline constexpr std::uint32_t SPEC2_SRSO_STATUS_SHIFT = 18;
inline constexpr std::uint32_t SPEC2_RFDS_REPORTED = 0x0040'0000U;    // bit 22
inline constexpr std::uint32_t SPEC2_RFDS_STATUS_MASK = 0x0180'0000U; // bits 23-24
inline constexpr std::uint32_t SPEC2_RFDS_STATUS_SHIFT = 23;
static_assert(SPEC2_GDS_STATUS_MASK >> SPEC2_GDS_STATUS_SHIFT == 0x7U);
static_assert(SPEC2_SRSO_STATUS_MASK >> SPEC2_SRSO_STATUS_SHIFT == 0x3U);
static_assert(SPEC2_RFDS_STATUS_MASK >> SPEC2_RFDS_STATUS_SHIFT == 0x3U);

// The GDS, SRSO and RFDS status fields' values (the module's SYSTEM_SPECULATION_CONTROL_*_MITIGATION_*).
inline constexpr std::uint32_t MITIGATION_STATUS_UNSUPPORTED = 0;
inline constexpr std::uint32_t MITIGATION_STATUS_DISABLED = 1;
inline constexpr std::uint32_t MITIGATION_STATUS_HARDWARE_IMMUNE = 2;
inline constexpr std::uint32_t MITIGATION_STATUS_MITIGATED = 3;
inline constexpr std::uint32_t MITIGATION_STATUS_MITIGATED_AND_LOCKED = 4; // GDS only

/// SYSTEM_KERNEL_VA_SHADOW_INFORMATION: one ULONG of flags (the module's $kvaShadow* and $l1tf*).
inline constexpr std::uint32_t KVA_SHADOW_ENABLED = 0x0001U;            // bit 0
inline constexpr std::uint32_t KVA_SHADOW_REQUIRED = 0x0010U;           // bit 4
inline constexpr std::uint32_t KVA_SHADOW_REQUIRED_AVAILABLE = 0x0020U; // bit 5
inline constexpr std::uint32_t KVA_INVALID_PTE_BIT_MASK = 0x0FC0U;      // bits 6-11
inline constexpr std::uint32_t KVA_L1D_FLUSH_SUPPORTED = 0x1000U;       // bit 12
inline constexpr std::uint32_t KVA_L1TF_MITIGATION_PRESENT = 0x2000U;   // bit 13

/// A vulnerability's status when Windows doesn't report whether the CPU needs its mitigation.
inline constexpr std::string_view UNKNOWN_STATUS = "Unknown";

// System.Volume.BitLockerProtection's values, as the property's schema lists them (its enum's display
// texts on Windows 11): IPropertyDescription::GetEnumTypeList on the key PSGetPropertyKeyFromName returns.
inline constexpr std::uint32_t BITLOCKER_PROTECTION_ON = 1;
inline constexpr std::uint32_t BITLOCKER_PROTECTION_OFF = 2;
inline constexpr std::uint32_t BITLOCKER_PROTECTION_ENCRYPTING = 3;
inline constexpr std::uint32_t BITLOCKER_PROTECTION_DECRYPTING = 4;
inline constexpr std::uint32_t BITLOCKER_PROTECTION_SUSPENDED = 5;
inline constexpr std::uint32_t BITLOCKER_PROTECTION_LOCKED = 6;
inline constexpr std::uint32_t BITLOCKER_PROTECTION_NOT_ENCRYPTABLE = 7;
inline constexpr std::uint32_t BITLOCKER_PROTECTION_WAITING_FOR_ACTIVATION = 8;

/// The calls readPlatformSecurity() makes.
struct Functions
{
    /// UEFISecureBootEnabled under the SecureBoot\State key; nullopt when it can't be read.
    std::optional<std::uint32_t> (*secureBootEnabledValue)() = nullptr;
    /// GetFirmwareType: true for a UEFI boot, false for a legacy BIOS one; nullopt when unknown.
    std::optional<bool> (*bootedWithUefi)() = nullptr;
    /// Tbsi_GetDeviceInfo's result, with TPM_DEVICE_INFO::tpmVersion in @p version; nullopt when tbs.dll or
    /// the function can't be loaded.
    std::optional<std::uint32_t> (*tpmDeviceInfo)(std::uint32_t& version) = nullptr;
    /// NtQuerySystemInformation into @p buffer, whose size is the length asked for; the NTSTATUS (0 on success).
    std::int32_t (*querySystemInformation)(std::uint32_t infoClass, std::span<std::byte> buffer) = nullptr;
    /// The fixed volumes' drives ("C:"), in drive-letter order (GetLogicalDrives, GetDriveTypeW); nullopt
    /// when the drives can't be listed.
    std::optional<std::vector<std::string>> (*fixedVolumes)() = nullptr;
    /// System.Volume.BitLockerProtection of @p volume ("C:") from the shell's property store; nullopt when
    /// the shell doesn't report it, as for a volume BitLocker knows nothing of.
    std::optional<std::uint32_t> (*bitLockerProtection)(const std::string& volume) = nullptr;
};

/// The real table (WindowsPlatformSecurity.cpp; Windows only).
[[nodiscard]] Functions systemFunctions();

/// "Not affected" when the CPU doesn't need the mitigation, else @p mitigation or @p vulnerable by whether
/// it is on; UNKNOWN_STATUS when Windows didn't say whether it is needed.
[[nodiscard]] inline std::string
statusText(std::optional<bool> required, bool mitigated, std::string_view mitigation, std::string_view vulnerable)
{
    if (!required.has_value())
    {
        return std::string(UNKNOWN_STATUS);
    }
    if (!*required)
    {
        return "Not affected";
    }
    return std::string(mitigated ? mitigation : vulnerable);
}

/// TPM_DEVICE_INFO::tpmVersion as the major version: 1 or 2, 0 when it is neither.
[[nodiscard]] constexpr std::uint32_t tpmVersionMajor(std::uint32_t tpmVersion) noexcept
{
    if (tpmVersion == TPM_VERSION_20_CODE)
    {
        return 2;
    }
    return tpmVersion == TPM_VERSION_12_CODE ? 1 : 0;
}

/// System.Volume.BitLockerProtection's value as a state; Unknown when absent or not one the schema lists.
[[nodiscard]] constexpr BitLockerState bitLockerState(std::optional<std::uint32_t> protection) noexcept
{
    if (!protection.has_value())
    {
        return BitLockerState::Unknown;
    }
    switch (*protection)
    {
    case BITLOCKER_PROTECTION_ON:
        return BitLockerState::On;
    case BITLOCKER_PROTECTION_OFF:
        return BitLockerState::Off;
    case BITLOCKER_PROTECTION_ENCRYPTING:
        return BitLockerState::Encrypting;
    case BITLOCKER_PROTECTION_DECRYPTING:
        return BitLockerState::Decrypting;
    case BITLOCKER_PROTECTION_SUSPENDED:
        return BitLockerState::Suspended;
    case BITLOCKER_PROTECTION_LOCKED:
        return BitLockerState::Locked;
    case BITLOCKER_PROTECTION_NOT_ENCRYPTABLE:
        return BitLockerState::NotEncryptable;
    case BITLOCKER_PROTECTION_WAITING_FOR_ACTIVATION:
        return BitLockerState::WaitingForActivation;
    default:
        return BitLockerState::Unknown;
    }
}

/// Spectre v2 (branch target injection) from SpeculationControlFlags. Windows has no "not affected" state
/// for it, and the module treats every CPU as needing it, so this is a mitigation or Vulnerable.
[[nodiscard]] inline std::string spectreV2Status(std::uint32_t flags)
{
    const auto has = [flags](std::uint32_t bit)
    {
        return (flags & bit) != 0;
    };
    if (has(SPEC_BPB_ENABLED))
    {
        if (has(SPEC_ENHANCED_IBRS))
        {
            return "Mitigation: Enhanced IBRS";
        }
        if (has(SPEC_RETPOLINE_ENABLED))
        {
            return "Mitigation: Retpolines";
        }
        return has(SPEC_IBRS_PRESENT) ? "Mitigation: IBRS" : "Mitigation: Branch prediction barriers";
    }
    if (has(SPEC_BPB_DISABLED_SYSTEM_POLICY))
    {
        return "Vulnerable: the mitigation is turned off by system policy";
    }
    // The module's btiHardwarePresent is SpecCtrlEnumerated or SpecCmdEnumerated; IBRS and STIBP are controls
    // in the first of those MSRs, so either one present means the microcode has the mitigation too.
    const bool hardware =
        has(SPEC_SPEC_CTRL_ENUMERATED) || has(SPEC_SPEC_CMD_ENUMERATED) || has(SPEC_IBRS_PRESENT) || has(SPEC_STIBP_PRESENT);
    if (has(SPEC_BPB_DISABLED_NO_HARDWARE_SUPPORT) || !hardware)
    {
        return "Vulnerable: the CPU's microcode doesn't support the mitigation";
    }
    return "Vulnerable";
}

/// Speculative Store Bypass from SpeculationControlFlags: Unknown until Windows supports SSBD
/// (SsbdAvailable), as the module reads SsbdRequired only then.
[[nodiscard]] inline std::string storeBypassStatus(std::uint32_t flags)
{
    if ((flags & SPEC_SSBD_AVAILABLE) == 0)
    {
        return std::string(UNKNOWN_STATUS);
    }
    if ((flags & SPEC_SSBD_REQUIRED) == 0)
    {
        return "Not affected";
    }
    if ((flags & SPEC_SSBD_SYSTEM_WIDE) != 0)
    {
        return "Mitigation: Speculative Store Bypass disabled system-wide";
    }
    if ((flags & SPEC_SSBD_SUPPORTED) == 0)
    {
        return "Vulnerable: the CPU's microcode doesn't support Speculative Store Bypass Disable";
    }
    return "Vulnerable: Speculative Store Bypass Disable is off (the Windows default)";
}

/// A data-sampling issue Windows reports as "hardware protected" plus a buffer-clearing mitigation (MDS:
/// MbClear; MMIO stale data: FbClear). Neither protected nor mitigated is Unknown, not Vulnerable: Windows
/// leaves the protected bits clear on CPUs it doesn't track them for (the module overrides them to
/// protected on AMD and ARM), so a clear bit doesn't say the CPU needs the mitigation.
[[nodiscard]] inline std::string bufferClearStatus(bool hardwareProtected, bool clearEnabled)
{
    if (hardwareProtected)
    {
        return "Not affected";
    }
    return clearEnabled ? "Mitigation: Clear CPU buffers" : std::string(UNKNOWN_STATUS);
}

/// The GDS, SRSO and RFDS status fields. "Mitigation unsupported" doesn't say whether the CPU is affected,
/// so it is Unknown; "disabled" is a mitigation Windows has for this CPU and has turned off.
[[nodiscard]] inline std::string mitigationStatusText(std::uint32_t status, std::string_view mitigation)
{
    switch (status)
    {
    case MITIGATION_STATUS_DISABLED:
        return "Vulnerable";
    case MITIGATION_STATUS_HARDWARE_IMMUNE:
        return "Not affected";
    case MITIGATION_STATUS_MITIGATED:
        return "Mitigation: " + std::string(mitigation);
    case MITIGATION_STATUS_MITIGATED_AND_LOCKED:
        return "Mitigation: " + std::string(mitigation) + " (locked)";
    case MITIGATION_STATUS_UNSUPPORTED:
    default:
        return std::string(UNKNOWN_STATUS);
    }
}

/// The vulnerabilities Windows reports, by the Linux kernel's names and in its status wording ("Not
/// affected", "Mitigation: ...", "Vulnerable..."), sorted by name. @p speculation is
/// SystemSpeculationControlInformation and @p kvaShadow the SystemKernelVaShadowInformation flags, each
/// nullopt when it couldn't be read. Every status comes from those bits, never from the CPU's vendor:
/// - Spectre v2, speculative store bypass, MDS, Meltdown and L1TF, which every Windows since 2018 tracks,
///   always have a row: UNKNOWN_STATUS when Windows doesn't say whether this CPU needs the mitigation;
/// - MMIO stale data, GDS, SRSO and RFDS have one only where Windows reports them (newer builds), as a
///   Linux kernel lists only the issues it knows about.
[[nodiscard]] inline std::vector<CpuVulnerability> decodeVulnerabilities(std::optional<SpeculationControlInformation> speculation,
                                                                         std::optional<std::uint32_t> kvaShadow)
{
    std::vector<CpuVulnerability> out;
    const std::uint32_t flags = speculation.has_value() ? speculation->flags : 0;
    const std::uint32_t flags2 = speculation.has_value() ? speculation->flags2 : 0;
    const std::uint32_t kva = kvaShadow.value_or(0);

    out.push_back({.name = "spectre_v2", .status = speculation.has_value() ? spectreV2Status(flags) : std::string(UNKNOWN_STATUS)});
    out.push_back(
        {.name = "spec_store_bypass", .status = speculation.has_value() ? storeBypassStatus(flags) : std::string(UNKNOWN_STATUS)});
    out.push_back({
        .name = "mds",
        .status = (flags & SPEC_MB_CLEAR_REPORTED) != 0
                    ? bufferClearStatus((flags & SPEC_MDS_HARDWARE_PROTECTED) != 0, (flags & SPEC_MB_CLEAR_ENABLED) != 0)
                    : std::string(UNKNOWN_STATUS),
    });

    // Meltdown (rogue data cache load): KvaShadowRequired once Windows reports it (KvaShadowRequiredAvailable),
    // else RdclHardwareProtected (23H2). Whether KVA shadowing is on needs the VA shadow class.
    std::optional<bool> meltdownRequired;
    if (kvaShadow.has_value() && (kva & KVA_SHADOW_REQUIRED_AVAILABLE) != 0)
    {
        meltdownRequired = (kva & KVA_SHADOW_REQUIRED) != 0;
    }
    else if ((flags2 & SPEC2_RDCL_HARDWARE_PROTECTED_REPORTED) != 0)
    {
        meltdownRequired = (flags2 & SPEC2_RDCL_HARDWARE_PROTECTED) == 0;
    }
    if (meltdownRequired == true && !kvaShadow.has_value())
    {
        meltdownRequired = std::nullopt; // needed, but whether it is mitigated isn't reported
    }
    out.push_back({
        .name = "meltdown",
        .status = statusText(
            meltdownRequired, (kva & KVA_SHADOW_ENABLED) != 0, "Mitigation: Kernel VA shadow", "Vulnerable: Kernel VA shadow is off"),
    });

    // L1TF is a rogue-data-cache-load issue, so a CPU that doesn't need KVA shadowing isn't affected; the
    // hypervisor's L1TF status says so either way. A CPU that does need KVA shadowing may or may not be
    // affected (the module decides by model), so without the hypervisor's word it is Unknown. The module's
    // mitigation is the invalid PTE bit with KVA shadowing on.
    std::optional<bool> l1tfRequired;
    if ((flags & SPEC_HV_L1TF_STATUS_AVAILABLE) != 0)
    {
        l1tfRequired = (flags & SPEC_HV_L1TF_PROCESSOR_NOT_AFFECTED) == 0;
    }
    if (meltdownRequired == false)
    {
        l1tfRequired = false;
    }
    if (l1tfRequired == true && !kvaShadow.has_value())
    {
        l1tfRequired = std::nullopt;
    }
    const bool l1tfMitigated = (kva & KVA_INVALID_PTE_BIT_MASK) != 0 && (kva & KVA_SHADOW_ENABLED) != 0;
    const bool l1tfPresent = (kva & (KVA_L1TF_MITIGATION_PRESENT | KVA_L1D_FLUSH_SUPPORTED)) != 0 || l1tfMitigated;
    out.push_back({
        .name = "l1tf",
        .status = statusText(l1tfRequired,
                             l1tfMitigated,
                             "Mitigation: Invalid PTE bit",
                             l1tfPresent ? "Vulnerable: the invalid PTE bit is off" : "Vulnerable: Windows has no L1TF mitigation"),
    });

    if ((flags2 & SPEC2_FB_CLEAR_REPORTED) != 0)
    {
        constexpr std::uint32_t ALL_PROTECTED =
            SPEC2_SBDR_SSDP_HARDWARE_PROTECTED | SPEC2_FBSDP_HARDWARE_PROTECTED | SPEC2_PSDP_HARDWARE_PROTECTED;
        out.push_back({
            .name = "mmio_stale_data",
            .status = bufferClearStatus((flags2 & ALL_PROTECTED) == ALL_PROTECTED, (flags2 & SPEC2_FB_CLEAR_ENABLED) != 0),
        });
    }
    if ((flags2 & SPEC2_GDS_REPORTED) != 0)
    {
        out.push_back({
            .name = "gather_data_sampling",
            .status = mitigationStatusText((flags2 & SPEC2_GDS_STATUS_MASK) >> SPEC2_GDS_STATUS_SHIFT, "Microcode"),
        });
    }
    if ((flags2 & SPEC2_SRSO_REPORTED) != 0)
    {
        out.push_back({
            .name = "spec_rstack_overflow",
            .status = mitigationStatusText((flags2 & SPEC2_SRSO_STATUS_MASK) >> SPEC2_SRSO_STATUS_SHIFT, "Windows SRSO mitigation"),
        });
    }
    if ((flags2 & SPEC2_RFDS_REPORTED) != 0)
    {
        out.push_back({
            .name = "reg_file_data_sampling",
            .status = mitigationStatusText((flags2 & SPEC2_RFDS_STATUS_MASK) >> SPEC2_RFDS_STATUS_SHIFT, "Clear Register File"),
        });
    }
    // Not read: the branch confusion (Retbleed) status, which the module trusts only on AMD and derives from
    // the Spectre v2 bits on Intel, and the divide-by-zero status, which the Linux kernel doesn't list.
    std::ranges::sort(out, {}, &CpuVulnerability::name);
    return out;
}

/// The first 32-bit word of @p bytes, in the machine's byte order.
[[nodiscard]] inline std::uint32_t firstWord(std::span<const std::byte> bytes)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data(), std::min(bytes.size(), sizeof(value)));
    return value;
}

/// A fixed-size NtQuerySystemInformation class's first 32-bit word; nullopt when the query fails.
[[nodiscard]] inline std::optional<std::uint32_t> queryWord(const Functions& fns, std::uint32_t infoClass)
{
    std::array<std::byte, sizeof(std::uint32_t)> buffer{};
    if (fns.querySystemInformation(infoClass, buffer) != 0)
    {
        return std::nullopt;
    }
    return firstWord(buffer);
}

/// SystemSpeculationControlInformation: both words, asked for as the module does; the first alone when this
/// Windows rejects the longer length; nullopt when the class isn't supported.
[[nodiscard]] inline std::optional<SpeculationControlInformation> querySpeculationControl(const Functions& fns)
{
    std::array<std::byte, sizeof(SpeculationControlInformation)> buffer{};
    if (fns.querySystemInformation(SYSTEM_SPECULATION_CONTROL_INFORMATION, buffer) == 0)
    {
        const std::span<const std::byte> bytes(buffer);
        return SpeculationControlInformation{.flags = firstWord(bytes), .flags2 = firstWord(bytes.subspan(sizeof(std::uint32_t)))};
    }
    const std::optional<std::uint32_t> first = queryWord(fns, SYSTEM_SPECULATION_CONTROL_INFORMATION);
    if (!first.has_value())
    {
        return std::nullopt;
    }
    return SpeculationControlInformation{.flags = *first, .flags2 = 0};
}

/// Every Security fact through @p fns. A fact that can't be read is left Unknown or nullopt.
inline void readPlatformSecurity(PlatformSecurityInfo& info, const Functions& fns)
{
    info.available = true;
    info.family = OsFamily::Windows;

    if (fns.bootedWithUefi() == false)
    {
        info.secureBoot = SecurityFeatureState::NotSupported;
    }
    else if (const std::optional<std::uint32_t> enabled = fns.secureBootEnabledValue(); enabled.has_value())
    {
        info.secureBoot = *enabled != 0 ? SecurityFeatureState::On : SecurityFeatureState::Off;
    }

    std::uint32_t version = 0;
    if (const std::optional<std::uint32_t> result = fns.tpmDeviceInfo(version); result == TBS_SUCCESS_CODE)
    {
        info.tpm = SecurityFeatureState::On;
        info.tpmVersionMajor = tpmVersionMajor(version);
    }
    else if (result == TBS_E_TPM_NOT_FOUND_CODE)
    {
        info.tpm = SecurityFeatureState::NotSupported;
    }

    // SYSTEM_CODEINTEGRITY_INFORMATION: a ULONG Length, which must be set, then the options.
    std::array<std::byte, 2 * sizeof(std::uint32_t)> codeIntegrity{};
    constexpr auto CODE_INTEGRITY_LENGTH = static_cast<std::uint32_t>(2 * sizeof(std::uint32_t));
    std::memcpy(codeIntegrity.data(), &CODE_INTEGRITY_LENGTH, sizeof(CODE_INTEGRITY_LENGTH));
    if (fns.querySystemInformation(SYSTEM_CODE_INTEGRITY_INFORMATION, codeIntegrity) == 0)
    {
        info.memoryIntegrity = hvciEnabled(firstWord(std::span<const std::byte>(codeIntegrity).subspan(sizeof(std::uint32_t))));
    }

    // SYSTEM_DMA_GUARD_POLICY_INFORMATION: one BOOLEAN.
    std::array<std::byte, 1> dmaGuard{};
    if (fns.querySystemInformation(SYSTEM_DMA_GUARD_POLICY_INFORMATION, dmaGuard) == 0)
    {
        info.kernelDmaProtection = dmaGuard[0] != std::byte{0};
    }

    const std::optional<SpeculationControlInformation> speculation = querySpeculationControl(fns);
    const std::optional<std::uint32_t> kvaShadow = queryWord(fns, SYSTEM_KERNEL_VA_SHADOW_INFORMATION);
    info.vulnerabilitiesRead = speculation.has_value() || kvaShadow.has_value();
    if (info.vulnerabilitiesRead)
    {
        info.vulnerabilities = decodeVulnerabilities(speculation, kvaShadow);
    }

    std::optional<std::vector<std::string>> volumes = fns.fixedVolumes();
    info.bitLockerRead = volumes.has_value();
    if (volumes.has_value())
    {
        for (std::string& volume : *volumes)
        {
            const BitLockerState state = bitLockerState(fns.bitLockerProtection(volume));
            info.bitLocker.push_back({.volume = std::move(volume), .state = state});
        }
    }
}

} // namespace Platform::WindowsPlatformSecurity
