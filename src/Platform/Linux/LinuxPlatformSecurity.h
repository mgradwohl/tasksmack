#pragma once

// The Linux Security section's facts (#1514), read under an injected root ("/" in the app, a fixture
// tree in tests): Secure Boot from the EFI variable, the TPM from /sys/class/tpm, the active security
// modules, SELinux and AppArmor state and the lockdown mode from securityfs and sysfs, and the CPU
// vulnerability list, and the open dm-crypt volumes from /sys/block with their mounts (#2585). Only
// standard-library file access, so the parsing and the fixture tests build and run everywhere. Every
// file read here is world-readable.

#include "LinuxCommitPaging.h"
#include "LinuxOsInfo.h"
#include "LinuxStorage.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::LinuxPlatformSecurity
{

/// The EFI global variable GUID SecureBoot is stored under (UEFI spec, EFI_GLOBAL_VARIABLE).
inline constexpr std::string_view SECURE_BOOT_VARIABLE = "SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c";

/// An efivarfs variable's contents: four bytes of attributes, then the value. SecureBoot's value is
/// one byte, 1 when on. nullopt when the contents are too short to hold it.
[[nodiscard]] inline std::optional<bool> parseSecureBootVariable(std::string_view contents)
{
    constexpr std::size_t ATTRIBUTE_BYTES = 4;
    if (contents.size() <= ATTRIBUTE_BYTES)
    {
        return std::nullopt;
    }
    return contents[ATTRIBUTE_BYTES] == '\x01';
}

/// The comma-separated list in /sys/kernel/security/lsm ("lockdown,capability,yama,apparmor").
[[nodiscard]] inline std::vector<std::string> parseLsmList(std::string_view text)
{
    std::vector<std::string> modules;
    while (!text.empty())
    {
        const std::size_t comma = text.find(',');
        std::string_view name = text.substr(0, comma);
        text = (comma == std::string_view::npos) ? std::string_view{} : text.substr(comma + 1);
        while (!name.empty() && (name.back() == '\n' || name.back() == ' ' || name.back() == '\r'))
        {
            name.remove_suffix(1);
        }
        if (!name.empty())
        {
            modules.emplace_back(name);
        }
    }
    return modules;
}

/// /sys/fs/selinux/enforce: 1 enforcing, 0 permissive; nullopt otherwise.
[[nodiscard]] inline std::optional<bool> parseSelinuxEnforce(std::string_view text)
{
    if (text.starts_with('1'))
    {
        return true;
    }
    if (text.starts_with('0'))
    {
        return false;
    }
    return std::nullopt;
}

/// The legacy efivars interface's data file: the value alone, without the attributes.
[[nodiscard]] inline std::optional<bool> parseLegacySecureBootData(std::string_view contents)
{
    if (contents.empty())
    {
        return std::nullopt;
    }
    return contents[0] == '\x01';
}

/// Secure Boot's state: NotSupported without /sys/firmware/efi (a legacy BIOS boot); else the variable
/// under efivarfs, or the older efivars sysfs interface. A UEFI system whose firmware has no SecureBoot
/// variable doesn't support it either. Unknown when the variable exists but can't be read.
[[nodiscard]] inline SecurityFeatureState readSecureBoot(const std::filesystem::path& root)
{
    std::error_code ec;
    const std::filesystem::path efi = root / "sys/firmware/efi";
    if (!std::filesystem::is_directory(efi, ec))
    {
        return SecurityFeatureState::NotSupported;
    }
    const std::filesystem::path efivarfs = efi / "efivars" / SECURE_BOOT_VARIABLE;
    const std::filesystem::path legacy = efi / "vars" / SECURE_BOOT_VARIABLE / "data";
    for (const std::filesystem::path& path : {efivarfs, legacy})
    {
        if (!std::filesystem::exists(path, ec))
        {
            continue;
        }
        const std::string contents = LinuxOsInfo::readFile(path);
        const std::optional<bool> on = (path == legacy) ? parseLegacySecureBootData(contents) : parseSecureBootVariable(contents);
        if (!on.has_value())
        {
            return SecurityFeatureState::Unknown;
        }
        return *on ? SecurityFeatureState::On : SecurityFeatureState::Off;
    }
    // Neither interface mounted: whether the firmware has the variable is unknown.
    const bool interfaceMounted = std::filesystem::is_directory(efi / "efivars", ec) && !std::filesystem::is_empty(efi / "efivars", ec);
    return interfaceMounted ? SecurityFeatureState::NotSupported : SecurityFeatureState::Unknown;
}

/// The CPU vulnerability list, sorted by name; @p read set when the directory was listed.
[[nodiscard]] inline std::vector<CpuVulnerability> readVulnerabilities(const std::filesystem::path& root, bool& read)
{
    std::vector<CpuVulnerability> vulnerabilities;
    std::error_code ec;
    std::filesystem::directory_iterator entries(root / "sys/devices/system/cpu/vulnerabilities", ec);
    read = !ec;
    // Incremented with an error code: a range-for would throw if the listing failed part-way.
    for (; !ec && entries != std::filesystem::directory_iterator{}; entries.increment(ec))
    {
        const std::string status = LinuxOsInfo::readLine(entries->path());
        if (!status.empty())
        {
            vulnerabilities.push_back({.name = entries->path().filename().string(), .status = status});
        }
    }
    std::ranges::sort(vulnerabilities, {}, &CpuVulnerability::name);
    return vulnerabilities;
}

/// The dm-crypt flavour a device-mapper UUID names (#2585): cryptsetup prefixes it "CRYPT-LUKS2-",
/// "CRYPT-LUKS1-" or "CRYPT-PLAIN-". Empty for any other device (an LVM volume's "LVM-...", dm-verity's
/// "CRYPT-VERITY-", which doesn't encrypt, or no UUID at all).
[[nodiscard]] inline std::string_view cryptType(std::string_view uuid)
{
    if (uuid.starts_with("CRYPT-LUKS2-"))
    {
        return "LUKS2";
    }
    if (uuid.starts_with("CRYPT-LUKS1-"))
    {
        return "LUKS1";
    }
    if (uuid.starts_with("CRYPT-PLAIN-"))
    {
        return "Plain";
    }
    return {};
}

/// One /sys/block/dm-N, the parts the encryption rows use.
struct DmDevice
{
    std::string kernelName;          ///< "dm-0"
    std::string name;                ///< dm/name, the /dev/mapper name: "dm_crypt-0", "ubuntu--vg-ubuntu--lv"
    std::string uuid;                ///< dm/uuid: "CRYPT-LUKS2-<uuid>-dm_crypt-0", "LVM-<vg><lv>"
    std::string device;              ///< dev: "252:0"
    std::vector<std::string> slaves; ///< slaves/: what it's built on, "nvme0n1p3" or another "dm-0"
};

/// Every /sys/block/dm-* under @p root, in dm-N order; @p read set when /sys/block was listed.
[[nodiscard]] inline std::vector<DmDevice> readDmDevices(const std::filesystem::path& root, bool& read)
{
    std::vector<DmDevice> devices;
    std::error_code ec;
    std::filesystem::directory_iterator entries(root / "sys/block", ec);
    read = !ec;
    // Incremented with an error code: a range-for would throw if the listing failed part-way.
    for (; !ec && entries != std::filesystem::directory_iterator{}; entries.increment(ec))
    {
        const std::filesystem::path block = entries->path();
        std::string kernelName = block.filename().string();
        if (!kernelName.starts_with("dm-"))
        {
            continue;
        }
        DmDevice device{
            .kernelName = std::move(kernelName),
            .name = LinuxOsInfo::readLine(block / "dm/name"),
            .uuid = LinuxOsInfo::readLine(block / "dm/uuid"),
            .device = LinuxOsInfo::readLine(block / "dev"),
            .slaves = {},
        };
        std::error_code slavesEc;
        std::filesystem::directory_iterator slaves(block / "slaves", slavesEc);
        for (; !slavesEc && slaves != std::filesystem::directory_iterator{}; slaves.increment(slavesEc))
        {
            device.slaves.push_back(slaves->path().filename().string());
        }
        std::ranges::sort(device.slaves);
        devices.push_back(std::move(device));
    }
    // "dm-2" before "dm-10": shorter names first, then by name.
    std::ranges::sort(devices,
                      [](const DmDevice& a, const DmDevice& b)
                      {
                          return std::pair(a.kernelName.size(), std::string_view(a.kernelName)) <
                                 std::pair(b.kernelName.size(), std::string_view(b.kernelName));
                      });
    return devices;
}

/// The open dm-crypt volumes among @p devices (#2585), each with what it encrypts and the mount points of
/// @p mounts on it or on a device stacked on it (an LVM volume on a LUKS physical volume, Ubuntu's
/// encrypted install). @p mounts are real file systems (not isPseudoFileSystem()) whose device is the
/// block device's major:minor (LinuxStorage::blockDeviceOf()); one also matches by its source,
/// /dev/mapper/<name> or /dev/dm-N.
[[nodiscard]] inline std::vector<EncryptedVolume> encryptedVolumes(const std::vector<DmDevice>& devices,
                                                                   const std::vector<LinuxStorage::MountEntry>& mounts)
{
    const auto byKernelName = [&devices](std::string_view kernelName)
    {
        return std::ranges::find(devices, kernelName, &DmDevice::kernelName);
    };

    std::vector<EncryptedVolume> volumes;
    for (const DmDevice& crypt : devices)
    {
        std::string type(cryptType(crypt.uuid));
        if (type.empty())
        {
            continue;
        }
        EncryptedVolume volume{.name = crypt.name, .type = std::move(type), .device = {}, .mountPoints = {}};
        for (const std::string& slave : crypt.slaves)
        {
            const auto dm = byKernelName(slave);
            const std::string& shown = (dm != devices.end() && !dm->name.empty()) ? dm->name : slave;
            volume.device += (volume.device.empty() ? "" : ", ") + shown;
        }

        // The crypt device and every device stacked on it, however deep (LVM on LUKS, LUKS on LUKS).
        std::vector<const DmDevice*> stack{&crypt};
        for (bool grew = true; grew;)
        {
            grew = false;
            for (const DmDevice& candidate : devices)
            {
                if (std::ranges::find(stack, &candidate) != stack.end())
                {
                    continue;
                }
                const bool onStack = std::ranges::any_of(
                    candidate.slaves,
                    [&stack](const std::string& slave)
                    { return std::ranges::any_of(stack, [&slave](const DmDevice* member) { return member->kernelName == slave; }); });
                if (onStack)
                {
                    stack.push_back(&candidate);
                    grew = true;
                }
            }
        }

        for (const LinuxStorage::MountEntry& mount : mounts)
        {
            const bool onVolume = std::ranges::any_of(stack,
                                                      [&mount](const DmDevice* member)
                                                      {
                                                          return (!member->device.empty() && mount.device == member->device) ||
                                                                 (!member->name.empty() && mount.source == "/dev/mapper/" + member->name) ||
                                                                 mount.source == "/dev/" + member->kernelName;
                                                      });
            if (onVolume && std::ranges::find(volume.mountPoints, mount.mountPoint) == volume.mountPoints.end())
            {
                volume.mountPoints.push_back(mount.mountPoint);
            }
        }
        volumes.push_back(std::move(volume));
    }
    return volumes;
}

/// The open dm-crypt volumes under @p root (#2585) from /sys/block and /proc/self/mountinfo; @p read set
/// when /sys/block was listed, so an empty result with it set means there are none. Without mountinfo
/// the volumes are still listed, their mount points empty.
[[nodiscard]] inline std::vector<EncryptedVolume> readEncryptedVolumes(const std::filesystem::path& root, bool& read)
{
    const std::vector<DmDevice> devices = readDmDevices(root, read);
    if (std::ranges::none_of(devices, [](const DmDevice& device) { return !cryptType(device.uuid).empty(); }))
    {
        return {};
    }
    std::vector<LinuxStorage::MountEntry> mounts;
    for (LinuxStorage::MountEntry& entry : LinuxStorage::parseMountInfo(LinuxOsInfo::readFile(root / "proc/self/mountinfo")))
    {
        if (LinuxStorage::isPseudoFileSystem(entry.fileSystem, entry.mountPoint))
        {
            continue;
        }
        entry.device = LinuxStorage::blockDeviceOf(root, entry); // btrfs names an anonymous device (#2432)
        mounts.push_back(std::move(entry));
    }
    return encryptedVolumes(devices, mounts);
}

/// The facts under @p root into @p info.
inline void readPlatformSecurityFacts(const std::filesystem::path& root, PlatformSecurityInfo& info)
{
    info.available = true;
    info.secureBoot = readSecureBoot(root);

    std::error_code ec;
    if (std::filesystem::is_directory(root / "sys/class/tpm/tpm0", ec))
    {
        info.tpm = SecurityFeatureState::On;
        const std::optional<std::uint64_t> major =
            LinuxCommitPaging::parseUnsigned(LinuxOsInfo::readLine(root / "sys/class/tpm/tpm0/tpm_version_major"));
        info.tpmVersionMajor = (major.has_value() && *major <= 9) ? static_cast<std::uint32_t>(*major) : 0;
    }
    else
    {
        // sysfs always has /sys/class; no tpm0 under it means no TPM the kernel can use.
        info.tpm =
            std::filesystem::is_directory(root / "sys/class", ec) ? SecurityFeatureState::NotSupported : SecurityFeatureState::Unknown;
    }

    if (const std::string lsm = LinuxOsInfo::readFile(root / "sys/kernel/security/lsm"); !lsm.empty())
    {
        info.lsmRead = true;
        info.lsms = parseLsmList(lsm);
    }
    info.selinuxEnforcing = parseSelinuxEnforce(LinuxOsInfo::readLine(root / "sys/fs/selinux/enforce"));
    info.apparmorEnabled = LinuxCommitPaging::parseSysfsBool(LinuxOsInfo::readLine(root / "sys/module/apparmor/parameters/enabled"));
    info.lockdown = LinuxCommitPaging::parseBracketedChoice(LinuxOsInfo::readFile(root / "sys/kernel/security/lockdown"));
    info.vulnerabilities = readVulnerabilities(root, info.vulnerabilitiesRead);
    info.encryptedVolumes = readEncryptedVolumes(root, info.encryptedVolumesRead);
}

} // namespace Platform::LinuxPlatformSecurity
