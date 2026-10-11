#pragma once

// The Windows Security facts (#1514), read only and unprivileged:
// - Secure Boot from HKLM\SYSTEM\CurrentControlSet\Control\SecureBoot\State\UEFISecureBootEnabled, and "not
//   supported" on a legacy BIOS boot (GetFirmwareType);
// - the TPM and its version from Tbsi_GetDeviceInfo (tbs.dll, loaded at run time from System32);
// - memory integrity (HVCI) from SystemCodeIntegrityInformation, decoded as the CPU details' row is (#809);
// - Kernel DMA Protection from SystemDmaGuardPolicyInformation;
// - the CPU's speculative-execution vulnerabilities from SystemSpeculationControlInformation and
//   SystemKernelVaShadowInformation, as Microsoft's SpeculationControl script reads them, under the names
//   and status wording the Linux kernel uses, so the section reads the same on both.
// BitLocker's state needs administrator, so the section says so rather than reading it.
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
#include <utility>
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

// SYSTEM_SPECULATION_CONTROL_INFORMATION's first flags word.
inline constexpr std::uint32_t SPEC_BPB_ENABLED = 0x0000'0001U;
inline constexpr std::uint32_t SPEC_BPB_DISABLED_SYSTEM_POLICY = 0x0000'0002U;
inline constexpr std::uint32_t SPEC_BPB_DISABLED_NO_HARDWARE_SUPPORT = 0x0000'0004U;
inline constexpr std::uint32_t SPEC_SSBD_AVAILABLE = 0x0000'0100U;
inline constexpr std::uint32_t SPEC_SSBD_SYSTEM_WIDE = 0x0000'0400U;
inline constexpr std::uint32_t SPEC_SSBD_REQUIRED = 0x0000'1000U;
inline constexpr std::uint32_t SPEC_RETPOLINE_ENABLED = 0x0000'4000U;
inline constexpr std::uint32_t SPEC_ENHANCED_IBRS = 0x0001'0000U;
inline constexpr std::uint32_t SPEC_MDS_HARDWARE_PROTECTED = 0x0100'0000U;
inline constexpr std::uint32_t SPEC_MB_CLEAR_ENABLED = 0x0200'0000U;
inline constexpr std::uint32_t SPEC_MB_CLEAR_REPORTED = 0x0400'0000U;

// SYSTEM_KERNEL_VA_SHADOW_INFORMATION's flags.
inline constexpr std::uint32_t KVA_SHADOW_ENABLED = 0x0001U;
inline constexpr std::uint32_t KVA_SHADOW_REQUIRED = 0x0010U;
inline constexpr std::uint32_t KVA_SHADOW_REQUIRED_AVAILABLE = 0x0020U;
inline constexpr std::uint32_t KVA_L1TF_MITIGATION_PRESENT = 0x2000U;

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
    /// The CPUID vendor ("GenuineIntel", "AuthenticAMD"); empty when there is none (ARM64).
    std::string (*cpuVendor)() = nullptr;
};

/// The real table (WindowsPlatformSecurity.cpp; Windows only).
[[nodiscard]] Functions systemFunctions();

/// Whether a CPU from @p vendor is affected by the Meltdown-class (rogue data cache load) and MDS issues when
/// Windows doesn't say: Intel's are, AMD's and Hygon's aren't; nullopt for anyone else.
[[nodiscard]] inline std::optional<bool> vendorAffectedByMeltdownClass(std::string_view vendor)
{
    if (vendor == "GenuineIntel")
    {
        return true;
    }
    if (vendor == "AuthenticAMD" || vendor == "HygonGenuine")
    {
        return false;
    }
    return std::nullopt;
}

/// "Not affected" when the CPU doesn't need the mitigation, else @p mitigation or @p vulnerable by whether it is on.
[[nodiscard]] inline std::string statusText(bool required, bool mitigated, std::string_view mitigation, std::string_view vulnerable)
{
    if (!required)
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

/// The vulnerabilities Windows reports, by the Linux kernel's names and in its status wording ("Not
/// affected", "Mitigation: ...", "Vulnerable..."), sorted by name. @p speculation is the
/// SystemSpeculationControlInformation flags and @p kvaShadow the SystemKernelVaShadowInformation flags,
/// each nullopt when it couldn't be read; one Windows doesn't report enough about is left out.
[[nodiscard]] inline std::vector<CpuVulnerability>
decodeVulnerabilities(std::optional<std::uint32_t> speculation, std::optional<std::uint32_t> kvaShadow, std::string_view vendor)
{
    std::vector<CpuVulnerability> out;
    const std::optional<bool> vendorAffected = vendorAffectedByMeltdownClass(vendor);
    if (speculation.has_value())
    {
        const std::uint32_t flags = *speculation;
        const auto has = [flags](std::uint32_t bit)
        {
            return (flags & bit) != 0;
        };

        std::string spectre;
        if (has(SPEC_BPB_ENABLED))
        {
            spectre = "Mitigation: IBRS";
            if (has(SPEC_ENHANCED_IBRS))
            {
                spectre = "Mitigation: Enhanced IBRS";
            }
            else if (has(SPEC_RETPOLINE_ENABLED))
            {
                spectre = "Mitigation: Retpolines";
            }
        }
        else if (has(SPEC_BPB_DISABLED_SYSTEM_POLICY))
        {
            spectre = "Vulnerable: the mitigation is turned off by system policy";
        }
        else if (has(SPEC_BPB_DISABLED_NO_HARDWARE_SUPPORT))
        {
            spectre = "Vulnerable: the CPU's microcode doesn't support the mitigation";
        }
        else
        {
            spectre = "Vulnerable";
        }
        out.push_back({.name = "spectre_v2", .status = std::move(spectre)});

        if (has(SPEC_SSBD_AVAILABLE))
        {
            out.push_back({
                .name = "spec_store_bypass",
                .status = statusText(has(SPEC_SSBD_REQUIRED),
                                     has(SPEC_SSBD_SYSTEM_WIDE),
                                     "Mitigation: Speculative Store Bypass disabled system-wide",
                                     "Vulnerable: Speculative Store Bypass Disable is off (the Windows default)"),
            });
        }

        if (has(SPEC_MB_CLEAR_REPORTED))
        {
            std::optional<std::string> status;
            if (has(SPEC_MDS_HARDWARE_PROTECTED))
            {
                status = "Not affected";
            }
            else if (has(SPEC_MB_CLEAR_ENABLED))
            {
                status = "Mitigation: Clear CPU buffers";
            }
            else if (vendorAffected.has_value())
            {
                status = *vendorAffected ? "Vulnerable: Clear CPU buffers is off" : "Not affected";
            }
            if (status.has_value())
            {
                out.push_back({.name = "mds", .status = std::move(*status)});
            }
        }
    }

    if (kvaShadow.has_value())
    {
        const std::uint32_t flags = *kvaShadow;
        const std::optional<bool> required =
            (flags & KVA_SHADOW_REQUIRED_AVAILABLE) != 0 ? std::optional<bool>((flags & KVA_SHADOW_REQUIRED) != 0) : vendorAffected;
        if (required.has_value())
        {
            out.push_back({
                .name = "meltdown",
                .status = statusText(
                    *required, (flags & KVA_SHADOW_ENABLED) != 0, "Mitigation: Kernel VA shadow", "Vulnerable: Kernel VA shadow is off"),
            });
            out.push_back({
                .name = "l1tf",
                .status = statusText(*required, (flags & KVA_L1TF_MITIGATION_PRESENT) != 0, "Mitigation: Invalid PTE bit", "Vulnerable"),
            });
        }
    }
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

    const std::optional<std::uint32_t> speculation = queryWord(fns, SYSTEM_SPECULATION_CONTROL_INFORMATION);
    const std::optional<std::uint32_t> kvaShadow = queryWord(fns, SYSTEM_KERNEL_VA_SHADOW_INFORMATION);
    info.vulnerabilitiesRead = speculation.has_value() || kvaShadow.has_value();
    if (info.vulnerabilitiesRead)
    {
        info.vulnerabilities = decodeVulnerabilities(speculation, kvaShadow, fns.cpuVendor());
    }
}

} // namespace Platform::WindowsPlatformSecurity
