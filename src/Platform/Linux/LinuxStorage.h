#pragma once

// The Linux Storage facts (#1517), read under an injected root ("/" in the app, a fixture tree in tests):
// - Physical disks from /sys/block: size, queue/rotational, device/model, the firmware revision and
//   serial, and the temperature from the disk's hwmon (NVMe, or SATA with the drivetemp module). Udev's
//   plain-file database (/run/udev/data/b<major>:<minor>) supplies the bus and a SATA disk's serial.
//   SMART status needs udisks2 over D-Bus, which this reader doesn't use, so it is left unknown.
// - Volumes from /proc/self/mountinfo: real file systems only (see isPseudoFileSystem()), one per device
//   (a bind mount repeats its device and is left out), each sized by an injected statvfs() so the parsers
//   stay standard library only and the fixture tests and the fuzz target (tests/fuzz/fuzz_mountinfo.cpp)
//   run everywhere. Network file systems are listed but never sized: statvfs() on a dead server hangs.
// All unprivileged; nothing is spawned.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxCommitPaging.h"
#include "Platform/Linux/LinuxOsInfo.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::LinuxStorage
{

/// One /proc/self/mountinfo line, the fields the Storage section uses, octal escapes undone.
struct MountEntry
{
    std::string device;     ///< "major:minor"
    std::string root;       ///< The mount's root within its file system: "/" except for bind mounts and subvolumes
    std::string mountPoint; ///< "/home"
    std::string fileSystem; ///< "ext4"
    std::string source;     ///< "/dev/nvme0n1p2"
};

/// /proc/self/mountinfo: "id parent major:minor root mountpoint options [optional...] - fstype source
/// superoptions". A line without the "-" separator or its fields after it is skipped.
[[nodiscard]] inline std::vector<MountEntry> parseMountInfo(std::string_view text)
{
    std::vector<MountEntry> entries;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        const std::vector<std::string_view> fields = LinuxCommitPaging::splitFields(text.substr(start, end - start));
        start = end + 1;
        const auto separator = std::find(
            fields.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(fields.size(), 6)), fields.end(), std::string_view{"-"});
        if (fields.size() < 6 || separator == fields.end() || fields.end() - separator < 3)
        {
            continue;
        }
        entries.push_back({
            .device = std::string(fields[2]),
            .root = LinuxCommitPaging::unescapeSwapPath(fields[3]),
            .mountPoint = LinuxCommitPaging::unescapeSwapPath(fields[4]),
            .fileSystem = std::string(*(separator + 1)),
            .source = LinuxCommitPaging::unescapeSwapPath(*(separator + 2)),
        });
    }
    return entries;
}

/// Network file systems: listed, but never sized (statvfs() blocks while the server is unreachable).
[[nodiscard]] inline bool isNetworkFileSystem(std::string_view fileSystem)
{
    constexpr std::array<std::string_view, 10> NETWORK{
        "nfs",
        "nfs4",
        "cifs",
        "smb3",
        "smbfs",
        "ncpfs",
        "afs",
        "ceph",
        "glusterfs",
        "fuse.sshfs",
    };
    return std::ranges::find(NETWORK, fileSystem) != NETWORK.end();
}

/// File systems that aren't storage, so the section leaves them out: kernel interfaces (proc, sysfs,
/// cgroup, debugfs, ...), memory-backed ones (tmpfs except at /tmp, ramfs, devtmpfs), container and
/// snap layers (overlay, squashfs), the automounter's placeholders (autofs) and FUSE helpers other than
/// block-backed fuseblk (gvfs, the document portal, lxcfs). Anything mounted under /proc, /sys or /dev too.
[[nodiscard]] inline bool isPseudoFileSystem(std::string_view fileSystem, std::string_view mountPoint)
{
    constexpr std::array<std::string_view, 25> PSEUDO{
        "proc",    "sysfs",    "cgroup",  "cgroup2",    "devtmpfs", "devpts",    "securityfs", "pstore", "efivarfs",
        "bpf",     "debugfs",  "tracefs", "configfs",   "fusectl",  "mqueue",    "hugetlbfs",  "autofs", "binfmt_misc",
        "overlay", "squashfs", "nsfs",    "rpc_pipefs", "ramfs",    "selinuxfs", "fuse",
    };
    if (fileSystem == "tmpfs")
    {
        return mountPoint != "/tmp";
    }
    if (fileSystem.starts_with("fuse.") && !isNetworkFileSystem(fileSystem))
    {
        return true;
    }
    for (const std::string_view under : {std::string_view{"/proc/"}, std::string_view{"/sys/"}, std::string_view{"/dev/"}})
    {
        if (mountPoint.starts_with(under) || mountPoint == under.substr(0, under.size() - 1))
        {
            return true;
        }
    }
    return std::ranges::find(PSEUDO, fileSystem) != PSEUDO.end();
}

/// The mounts the section lists, in mountinfo order: real file systems, one per device. Of a device's
/// mounts the one whose root is "/" wins (the others are bind mounts of a directory in it), else the
/// first (a btrfs subvolume's root is never "/").
[[nodiscard]] inline std::vector<MountEntry> selectVolumes(const std::vector<MountEntry>& entries)
{
    std::vector<MountEntry> kept;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const MountEntry& entry = entries[i];
        if (isPseudoFileSystem(entry.fileSystem, entry.mountPoint))
        {
            continue;
        }
        bool better = false;
        for (std::size_t j = 0; j < entries.size() && !better; ++j)
        {
            const MountEntry& other = entries[j];
            if (j == i || other.device != entry.device || isPseudoFileSystem(other.fileSystem, other.mountPoint))
            {
                continue;
            }
            const bool otherIsRoot = other.root == "/";
            const bool entryIsRoot = entry.root == "/";
            better = (otherIsRoot && !entryIsRoot) || (otherIsRoot == entryIsRoot && j < i);
        }
        if (!better)
        {
            kept.push_back(entry);
        }
    }
    return kept;
}

/// The value of "E:@p key=" in a udev database file (/run/udev/data/b8:0); empty when it isn't there.
[[nodiscard]] inline std::string udevProperty(std::string_view text, std::string_view key)
{
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (line.size() > key.size() + 2 && line.starts_with("E:") && line.substr(2, key.size()) == key && line[key.size() + 2] == '=')
        {
            return std::string(line.substr(key.size() + 3));
        }
    }
    return {};
}

/// @p text without leading and trailing whitespace (sysfs pads SCSI model names).
[[nodiscard]] inline std::string trimmed(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        return {};
    }
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

/// A disk's bus from its kernel name and udev's ID_BUS: "NVMe", "MMC", "virtio", "SATA" (ata), "USB", ...
[[nodiscard]] inline std::string diskBus(std::string_view name, std::string_view udevBus)
{
    if (name.starts_with("nvme"))
    {
        return "NVMe";
    }
    if (name.starts_with("mmcblk"))
    {
        return "MMC";
    }
    if (name.starts_with("vd"))
    {
        return "virtio";
    }
    if (udevBus == "ata")
    {
        return "SATA";
    }
    if (udevBus == "usb")
    {
        return "USB";
    }
    if (udevBus == "scsi")
    {
        return "SCSI";
    }
    return std::string(udevBus);
}

/// /sys/block entries that are never physical disks: loop devices, RAM disks, zram, device-mapper and md
/// arrays (whose members are listed themselves), network block devices.
[[nodiscard]] inline bool isVirtualBlockDevice(std::string_view name)
{
    constexpr std::array<std::string_view, 6> PREFIXES{"loop", "ram", "zram", "dm-", "md", "nbd"};
    return std::ranges::any_of(PREFIXES, [name](std::string_view prefix) { return name.starts_with(prefix); });
}

/// The first readable temp1_input (millidegrees) under @p device's hwmon directories, in whole degrees.
[[nodiscard]] inline std::optional<int> hwmonTemperature(const std::filesystem::path& device)
{
    for (const std::filesystem::path& parent : {device, device / "hwmon"})
    {
        std::error_code ec;
        std::filesystem::directory_iterator it(parent, ec);
        std::vector<std::filesystem::path> hwmons;
        for (; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            if (it->path().filename().string().starts_with("hwmon"))
            {
                hwmons.push_back(it->path());
            }
        }
        std::ranges::sort(hwmons);
        for (const std::filesystem::path& hwmon : hwmons)
        {
            const std::string text = LinuxOsInfo::readLine(hwmon / "temp1_input");
            const std::string_view value = text.starts_with('-') ? std::string_view(text).substr(1) : std::string_view(text);
            if (const std::optional<std::uint64_t> milli = LinuxCommitPaging::parseUnsigned(value); milli.has_value() && *milli < 1'000'000)
            {
                const int degrees = static_cast<int>((*milli + 500) / 1000);
                return text.starts_with('-') ? -degrees : degrees;
            }
        }
    }
    return std::nullopt;
}

/// The physical disks under @p root's /sys/block, sorted by name; nullopt when it can't be listed.
[[nodiscard]] inline std::optional<std::vector<PhysicalDisk>> readDisks(const std::filesystem::path& root)
{
    constexpr std::uint64_t SECTOR = 512; // /sys/block/X/size is always in 512-byte sectors
    std::error_code ec;
    std::filesystem::directory_iterator blocks(root / "sys/block", ec);
    if (ec)
    {
        return std::nullopt;
    }
    std::vector<PhysicalDisk> disks;
    // Incremented with an error code: a range-for would throw if the listing failed part-way.
    for (; !ec && blocks != std::filesystem::directory_iterator{}; blocks.increment(ec))
    {
        const std::filesystem::path& block = blocks->path();
        const std::string name = block.filename().string();
        std::error_code exists;
        if (isVirtualBlockDevice(name) || !std::filesystem::exists(block / "device", exists))
        {
            continue;
        }
        const std::uint64_t sectors = LinuxCommitPaging::parseUnsigned(LinuxOsInfo::readLine(block / "size")).value_or(0);
        if (sectors == 0)
        {
            continue; // no media: an empty card reader or optical drive
        }
        const std::filesystem::path device = block / "device";
        const std::string udev = LinuxOsInfo::readFile(root / "run/udev/data" / ("b" + LinuxOsInfo::readLine(block / "dev")));
        PhysicalDisk disk;
        disk.name = name;
        disk.sizeBytes = sectors * SECTOR;
        disk.model = trimmed(LinuxOsInfo::readFile(device / "model"));
        if (disk.model.empty())
        {
            disk.model = trimmed(LinuxOsInfo::readFile(device / "name")); // MMC
        }
        disk.bus = diskBus(name, udevProperty(udev, "ID_BUS"));
        const std::string rotational = LinuxOsInfo::readLine(block / "queue/rotational");
        if (rotational == "1")
        {
            disk.media = DiskMedia::Hdd;
        }
        else if (rotational == "0")
        {
            disk.media = DiskMedia::Ssd;
        }
        for (const char* file : {"firmware_rev", "rev", "fwrev"})
        {
            if (disk.firmware.empty())
            {
                disk.firmware = trimmed(LinuxOsInfo::readFile(device / file));
            }
        }
        disk.serial = trimmed(LinuxOsInfo::readFile(device / "serial"));
        if (disk.serial.empty())
        {
            disk.serial = udevProperty(udev, "ID_SERIAL_SHORT");
        }
        disk.temperatureCelsius = hwmonTemperature(device);
        disk.healthUnavailableReason = "SMART status needs udisks2, which TaskSmack doesn't read yet";
        disks.push_back(std::move(disk));
    }
    std::ranges::sort(disks, {}, &PhysicalDisk::name);
    return disks;
}

/// Sizes one mounted file system: its total and the bytes free to an unprivileged user; false when
/// it can't be read. The app passes statvfs(); tests pass a fake.
using VolumeSizer = bool (*)(const std::string& mountPoint, std::uint64_t& sizeBytes, std::uint64_t& freeBytes);

/// The facts under @p root into @p info; @p sizer sizes each local volume.
inline void readStorageFacts(const std::filesystem::path& root, StorageInfo& info, VolumeSizer sizer)
{
    info.available = true;
    info.family = OsFamily::Linux;
    if (std::optional<std::vector<PhysicalDisk>> disks = readDisks(root); disks.has_value())
    {
        info.disksRead = true;
        info.disks = std::move(*disks);
    }

    // mountinfo always has the root mount, so an empty read means it couldn't be read.
    const std::string mountInfo = LinuxOsInfo::readFile(root / "proc/self/mountinfo");
    if (mountInfo.empty())
    {
        return;
    }
    info.volumesRead = true;
    for (const MountEntry& entry : selectVolumes(parseMountInfo(mountInfo)))
    {
        Volume volume;
        volume.mountPoint = entry.mountPoint;
        volume.fileSystem = entry.fileSystem;
        volume.device = entry.source;
        volume.network = isNetworkFileSystem(entry.fileSystem);
        volume.label = udevProperty(LinuxOsInfo::readFile(root / "run/udev/data" / ("b" + entry.device)), "ID_FS_LABEL");
        if (!volume.network && sizer != nullptr)
        {
            volume.sizeRead = sizer(entry.mountPoint, volume.sizeBytes, volume.freeBytes);
        }
        info.volumes.push_back(std::move(volume));
    }
}

} // namespace Platform::LinuxStorage
