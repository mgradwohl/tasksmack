#pragma once

// The Storage facts on Windows (#1517), all unprivileged:
// - Physical disks: each \\.\PhysicalDriveN opened query-only (desired access 0, #763), then
//   IOCTL_STORAGE_QUERY_PROPERTY for the device descriptor (model, firmware, serial, bus), the seek
//   penalty (SSD or HDD) and the temperature, and IOCTL_DISK_GET_DRIVE_GEOMETRY_EX for the size. An NVMe
//   drive's health log comes from the protocol-specific property; when Windows refuses it to a query-only
//   handle the row says it requires administrator. TaskSmack never asks for elevation or read access.
// - Volumes: every drive letter (GetLogicalDriveStringsW) with its label and file system
//   (GetVolumeInformationW) and size and free space (GetDiskFreeSpaceExW), under SEM_FAILCRITICALERRORS
//   so an empty card reader or optical drive fails at once instead of prompting; those are left out.
//   Network drives are listed but never queried: both calls block for the SMB timeout (tens of seconds)
//   while the server is unreachable, and they can't be cancelled.
// Every call goes through an injectable table so tests drive the buffers and the failures.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::WindowsStorage
{

/// The calls readStorage() makes. Defaults to the real ones; tests substitute fakes.
struct Functions
{
    Windows::UniqueHandle (*openPhysicalDrive)(int index) = &Windows::openPhysicalDriveQueryOnly;
    decltype(&DeviceIoControl) deviceIoControl = &DeviceIoControl;
    decltype(&GetLogicalDriveStringsW) getLogicalDriveStrings = &GetLogicalDriveStringsW;
    decltype(&GetDriveTypeW) getDriveType = &GetDriveTypeW;
    decltype(&GetVolumeInformationW) getVolumeInformation = &GetVolumeInformationW;
    decltype(&GetDiskFreeSpaceExW) getDiskFreeSpaceEx = &GetDiskFreeSpaceExW;
    decltype(&SetThreadErrorMode) setThreadErrorMode = &SetThreadErrorMode;
};

/// \\.\PhysicalDrive0 through 63 are tried: drive numbers can have gaps after a disk is removed, and a
/// missing one fails at once.
inline constexpr int MAX_PHYSICAL_DRIVES = 64;

/// The NVMe SMART / health information log page and its size.
inline constexpr DWORD NVME_HEALTH_LOG_PAGE = 0x02;
inline constexpr std::size_t NVME_LOG_PAGE_BYTES = 512;

/// STORAGE_BUS_TYPE's name; empty for unknown values.
[[nodiscard]] inline std::string_view busTypeName(int busType)
{
    constexpr std::array<std::string_view, 20> NAMES{
        "",
        "SCSI",
        "ATAPI",
        "ATA",
        "IEEE 1394",
        "SSA",
        "Fibre Channel",
        "USB",
        "RAID",
        "iSCSI",
        "SAS",
        "SATA",
        "SD",
        "MMC",
        "Virtual",
        "File-backed virtual",
        "Storage Spaces",
        "NVMe",
        "SCM",
        "UFS",
    };
    return busType >= 0 && static_cast<std::size_t>(busType) < NAMES.size() ? NAMES.at(static_cast<std::size_t>(busType))
                                                                            : std::string_view{};
}

/// The fields of a STORAGE_DEVICE_DESCRIPTOR the section shows.
struct DeviceDescriptor
{
    std::string model;
    std::string firmware;
    std::string serial;
    int busType = 0;
};

/// The NUL-terminated ASCII string at @p offset in @p buffer, trimmed of spaces; empty for offset 0 (none)
/// or one outside the buffer. A string missing its NUL ends at the buffer's end.
[[nodiscard]] inline std::string descriptorString(std::span<const std::byte> buffer, DWORD offset)
{
    if (offset == 0 || offset >= buffer.size())
    {
        return {};
    }
    std::string text;
    for (std::size_t i = offset; i < buffer.size() && buffer[i] != std::byte{0}; ++i)
    {
        text.push_back(static_cast<char>(buffer[i]));
    }
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos)
    {
        return {};
    }
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

/// A STORAGE_DEVICE_DESCRIPTOR buffer's model, firmware, serial and bus. The model is the product ID,
/// with the vendor ID in front when it says more than the bus ("ATA", "NVMe") and isn't already there.
/// nullopt when the buffer is too short for the descriptor.
[[nodiscard]] inline std::optional<DeviceDescriptor> parseDeviceDescriptor(std::span<const std::byte> buffer)
{
    STORAGE_DEVICE_DESCRIPTOR header{};
    if (buffer.size() < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties))
    {
        return std::nullopt;
    }
    std::memcpy(&header, buffer.data(), offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties));
    DeviceDescriptor descriptor;
    const std::string vendor = descriptorString(buffer, header.VendorIdOffset);
    descriptor.model = descriptorString(buffer, header.ProductIdOffset);
    if (!vendor.empty() && vendor != "ATA" && vendor != "NVMe" && !descriptor.model.starts_with(vendor))
    {
        descriptor.model = descriptor.model.empty() ? vendor : vendor + " " + descriptor.model;
    }
    descriptor.firmware = descriptorString(buffer, header.ProductRevisionOffset);
    descriptor.serial = descriptorString(buffer, header.SerialNumberOffset);
    descriptor.busType = static_cast<int>(header.BusType);
    return descriptor;
}

/// The NVMe health information log page (NVMe base specification, Get Log Page 02h): critical warning
/// at byte 0, available spare at 3, percentage used at 5, media errors from 160 (16 bytes, little
/// endian; the low 8 are kept). nullopt when the page is short.
[[nodiscard]] inline std::optional<NvmeHealth> parseNvmeHealthLog(std::span<const std::byte> page)
{
    constexpr std::size_t MEDIA_ERRORS = 160;
    if (page.size() < MEDIA_ERRORS + sizeof(std::uint64_t))
    {
        return std::nullopt;
    }
    NvmeHealth health;
    health.criticalWarning = static_cast<std::uint8_t>(page[0]);
    health.availableSparePercent = static_cast<std::uint8_t>(page[3]);
    health.percentageUsed = static_cast<std::uint8_t>(page[5]);
    for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i)
    {
        health.mediaErrors |= static_cast<std::uint64_t>(page[MEDIA_ERRORS + i]) << (8U * i);
    }
    return health;
}

/// IOCTL_STORAGE_QUERY_PROPERTY's standard query for @p property into @p bytes of output, trimmed to what
/// was returned; nullopt when it fails.
[[nodiscard]] inline std::optional<std::vector<std::byte>>
queryProperty(const Functions& fns, HANDLE disk, STORAGE_PROPERTY_ID property, std::size_t bytes)
{
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = property;
    query.QueryType = PropertyStandardQuery;
    std::vector<std::byte> out(bytes);
    DWORD returned = 0;
    if (fns.deviceIoControl(
            disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), out.data(), static_cast<DWORD>(out.size()), &returned, nullptr) ==
        FALSE)
    {
        return std::nullopt;
    }
    out.resize(std::min<std::size_t>(returned, out.size()));
    return out;
}

/// The drive's NVMe health log, or why it couldn't be read.
[[nodiscard]] inline std::optional<NvmeHealth> queryNvmeHealth(const Functions& fns, HANDLE disk, std::string& unavailableReason)
{
    constexpr std::size_t QUERY_BYTES = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters);
    constexpr std::size_t HEADER_BYTES = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
    std::vector<std::byte> buffer(QUERY_BYTES + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + NVME_LOG_PAGE_BYTES);

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProtocolSpecificProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_PROTOCOL_SPECIFIC_DATA request{};
    request.ProtocolType = ProtocolTypeNvme;
    request.DataType = NVMeDataTypeLogPage;
    request.ProtocolDataRequestValue = NVME_HEALTH_LOG_PAGE;
    request.ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    request.ProtocolDataLength = static_cast<DWORD>(NVME_LOG_PAGE_BYTES);
    std::memcpy(buffer.data(), &query, QUERY_BYTES);
    std::memcpy(buffer.data() + QUERY_BYTES, &request, sizeof(request));

    DWORD returned = 0;
    const auto size = static_cast<DWORD>(buffer.size());
    if (fns.deviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, buffer.data(), size, buffer.data(), size, &returned, nullptr) == FALSE)
    {
        unavailableReason = GetLastError() == ERROR_ACCESS_DENIED ? "Requires administrator" : "The drive didn't return its health log";
        return std::nullopt;
    }
    // The answer is a STORAGE_PROTOCOL_DATA_DESCRIPTOR whose data offset counts from its
    // ProtocolSpecificData member.
    STORAGE_PROTOCOL_SPECIFIC_DATA answer{};
    const std::size_t valid = std::min<std::size_t>(returned, buffer.size());
    std::optional<NvmeHealth> health;
    if (valid >= HEADER_BYTES + sizeof(answer))
    {
        std::memcpy(&answer, buffer.data() + HEADER_BYTES, sizeof(answer));
        const std::size_t at = HEADER_BYTES + answer.ProtocolDataOffset;
        if (answer.ProtocolDataOffset >= sizeof(answer) && at <= valid && answer.ProtocolDataLength <= valid - at)
        {
            health = parseNvmeHealthLog(std::span<const std::byte>(buffer).subspan(at, answer.ProtocolDataLength));
        }
    }
    if (!health.has_value())
    {
        unavailableReason = "The drive's health log was malformed";
    }
    return health;
}

/// One opened disk's facts.
[[nodiscard]] inline PhysicalDisk readDisk(const Functions& fns, HANDLE disk, int index)
{
    PhysicalDisk info;
    info.name = std::format("Disk {}", index);

    constexpr std::size_t DESCRIPTOR_BYTES = 1024;
    if (const auto buffer = queryProperty(fns, disk, StorageDeviceProperty, DESCRIPTOR_BYTES); buffer.has_value())
    {
        if (const std::optional<DeviceDescriptor> descriptor = parseDeviceDescriptor(*buffer); descriptor.has_value())
        {
            info.model = descriptor->model;
            info.firmware = descriptor->firmware;
            info.serial = descriptor->serial;
            info.bus = std::string(busTypeName(descriptor->busType));
        }
    }

    if (const auto buffer = queryProperty(fns, disk, StorageDeviceSeekPenaltyProperty, sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR));
        buffer.has_value() && buffer->size() >= sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR))
    {
        DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
        std::memcpy(&seek, buffer->data(), sizeof(seek));
        info.media = seek.IncursSeekPenalty != FALSE ? DiskMedia::Hdd : DiskMedia::Ssd;
    }

    constexpr std::size_t TEMPERATURE_BYTES = sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) + (4 * sizeof(STORAGE_TEMPERATURE_INFO));
    if (const auto buffer = queryProperty(fns, disk, StorageDeviceTemperatureProperty, TEMPERATURE_BYTES);
        buffer.has_value() && buffer->size() >= sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR))
    {
        STORAGE_TEMPERATURE_DATA_DESCRIPTOR temperature{};
        std::memcpy(&temperature, buffer->data(), sizeof(temperature));
        if (temperature.InfoCount > 0)
        {
            info.temperatureCelsius = temperature.TemperatureInfo[0].Temperature; // the device's own sensor
        }
    }

    DISK_GEOMETRY_EX geometry{};
    DWORD returned = 0;
    if (fns.deviceIoControl(disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof(geometry), &returned, nullptr) != FALSE &&
        returned >= offsetof(DISK_GEOMETRY_EX, Data) && geometry.DiskSize.QuadPart > 0)
    {
        info.sizeBytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
    }

    if (info.bus == "NVMe")
    {
        info.health = queryNvmeHealth(fns, disk, info.healthUnavailableReason);
    }
    return info;
}

/// The drive roots in a GetLogicalDriveStringsW buffer ("C:\", "D:\").
[[nodiscard]] inline std::vector<std::wstring> splitDriveStrings(std::wstring_view buffer)
{
    std::vector<std::wstring> roots;
    std::size_t start = 0;
    while (start < buffer.size())
    {
        std::size_t end = buffer.find(L'\0', start);
        if (end == std::wstring_view::npos)
        {
            end = buffer.size();
        }
        if (end > start)
        {
            roots.emplace_back(buffer.substr(start, end - start));
        }
        start = end + 1;
    }
    return roots;
}

/// Sets this thread's error mode to SEM_FAILCRITICALERRORS (no "insert a disk" prompt; the call fails
/// instead) and restores the previous mode when it goes out of scope, even on a throw.
class ErrorModeScope
{
  public:
    explicit ErrorModeScope(decltype(&SetThreadErrorMode) setThreadErrorMode)
        : m_Set(setThreadErrorMode), m_Active(setThreadErrorMode(SEM_FAILCRITICALERRORS, &m_Previous) != FALSE)
    {}
    ErrorModeScope(const ErrorModeScope&) = delete;
    ErrorModeScope& operator=(const ErrorModeScope&) = delete;
    ErrorModeScope(ErrorModeScope&&) = delete;
    ErrorModeScope& operator=(ErrorModeScope&&) = delete;
    ~ErrorModeScope()
    {
        if (m_Active)
        {
            m_Set(m_Previous, nullptr);
        }
    }

  private:
    decltype(&SetThreadErrorMode) m_Set;
    DWORD m_Previous = 0;
    bool m_Active;
};

/// Every drive letter's volume; nullopt when the drives can't be listed. The calls run under an
/// ErrorModeScope.
[[nodiscard]] inline std::optional<std::vector<Volume>> readVolumes(const Functions& fns)
{
    const DWORD needed = fns.getLogicalDriveStrings(0, nullptr);
    if (needed == 0)
    {
        return std::nullopt;
    }
    std::wstring buffer(needed, L'\0');
    const DWORD written = fns.getLogicalDriveStrings(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0 || written > buffer.size())
    {
        return std::nullopt;
    }
    buffer.resize(written);

    const ErrorModeScope failCriticalErrors(fns.setThreadErrorMode);
    std::vector<Volume> volumes;
    for (const std::wstring& root : splitDriveStrings(buffer))
    {
        const UINT type = fns.getDriveType(root.c_str());
        if (type == DRIVE_NO_ROOT_DIR)
        {
            continue;
        }
        Volume volume;
        // "C:\" is shown as "C:"; a folder mount point (not listed here) would keep its path.
        volume.mountPoint = WinString::wideToUtf8(root.size() == 3 && root[1] == L':' ? std::wstring_view(root).substr(0, 2) : root);
        if (type == DRIVE_REMOTE)
        {
            volume.network = true;
            volumes.push_back(std::move(volume));
            continue;
        }
        std::array<wchar_t, MAX_PATH + 1> label{};
        std::array<wchar_t, MAX_PATH + 1> fileSystem{};
        if (fns.getVolumeInformation(root.c_str(),
                                     label.data(),
                                     static_cast<DWORD>(label.size()),
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     fileSystem.data(),
                                     static_cast<DWORD>(fileSystem.size())) != FALSE)
        {
            volume.label = WinString::wideToUtf8(label.data());
            volume.fileSystem = WinString::wideToUtf8(fileSystem.data());
        }
        else if (type == DRIVE_REMOVABLE || type == DRIVE_CDROM)
        {
            continue; // no media
        }
        ULARGE_INTEGER available{};
        ULARGE_INTEGER total{};
        if (fns.getDiskFreeSpaceEx(root.c_str(), &available, &total, nullptr) != FALSE)
        {
            volume.sizeRead = true;
            volume.sizeBytes = total.QuadPart;
            volume.freeBytes = available.QuadPart;
        }
        volumes.push_back(std::move(volume));
    }
    return volumes;
}

/// Fills @p info's Windows facts; a call that fails leaves its facts unknown.
inline void readStorage(StorageInfo& info, const Functions& fns = {})
{
    info.available = true;
    info.family = OsFamily::Windows;
    info.disksRead = true;
    for (int index = 0; index < MAX_PHYSICAL_DRIVES; ++index)
    {
        if (const Windows::UniqueHandle disk = fns.openPhysicalDrive(index); disk)
        {
            info.disks.push_back(readDisk(fns, disk.get(), index));
        }
    }
    if (std::optional<std::vector<Volume>> volumes = readVolumes(fns); volumes.has_value())
    {
        info.volumesRead = true;
        info.volumes = std::move(*volumes);
    }
}

} // namespace Platform::WindowsStorage
