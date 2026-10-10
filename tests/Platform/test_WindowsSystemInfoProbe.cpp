/// @file test_WindowsSystemInfoProbe.cpp
/// @brief The real WindowsSystemInfoProbe (#1512), smoke-tested: it reports an edition and a build,
/// and the session facts any signed-in test run has; the real SMBIOS table (#1513) parses; and the
/// memory modules and installed/usable memory (#1515), real and through a faked function table; and commit,
/// page files and compressed memory (#1516), real and faked, including the NT-to-DOS path conversion; and
/// the Storage section's disks and volumes (#1517), real and through a faked function table, with each
/// disk's partitions and the drive letters on them (#1632); and the Graphics & displays adapters, drivers
/// and monitors (#1519), real and faked; and the Devices section's USB link speeds from faked hubs (#1642).

#include "EdidTestData.h"
#include "PartitionLayoutTestData.h"
#include "Platform/GPUTypes.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/PartitionTable.h"
#include "Platform/SmbiosParser.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"
#include "Platform/Windows/WindowsCommitPaging.h"
#include "Platform/Windows/WindowsDevices.h"
#include "Platform/Windows/WindowsGraphics.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsInstalledMemory.h"
#include "Platform/Windows/WindowsStorage.h"
#include "Platform/Windows/WindowsSystemInfoProbe.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <usbspec.h>
// clang-format on

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

TEST(WindowsSystemInfoProbeTest, ReadsAnEditionAndABuild)
{
    WindowsSystemInfoProbe probe;
    ASSERT_TRUE(probe.capabilities().hasOs);

    const OsInfo info = probe.readOs();
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.name.starts_with("Windows")) << info.name;
    EXPECT_FALSE(info.build.empty());
    EXPECT_NE(info.build.find('.'), std::string::npos) << info.build; // CurrentBuild.UBR
    EXPECT_FALSE(info.architecture.empty());
    EXPECT_FALSE(info.computerName.empty());
    EXPECT_FALSE(info.locale.empty());
    EXPECT_FALSE(info.timeZone.empty());
    EXPECT_TRUE(info.utcOffsetMinutes.has_value());
    EXPECT_FALSE(info.systemDirectory.empty());
    EXPECT_FALSE(info.windowsDirectory.empty());

    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    EXPECT_GT(info.bootUnixSeconds, 0U);
    EXPECT_LE(info.bootUnixSeconds, now);
}

TEST(WindowsSystemInfoProbeTest, RealSmbiosTableParses)
{
    constexpr DWORD RSMB = 0x52534D42;
    const UINT size = GetSystemFirmwareTable(RSMB, 0, nullptr, 0);
    if (size == 0)
    {
        GTEST_SKIP() << "GetSystemFirmwareTable('RSMB') failed: " << GetLastError();
    }
    std::vector<std::uint8_t> raw(size);
    const UINT written = GetSystemFirmwareTable(RSMB, 0, raw.data(), size);
    if (written == 0 || written > size)
    {
        GTEST_SKIP() << "GetSystemFirmwareTable('RSMB') failed on the second call";
    }
    raw.resize(written);

    const auto parsed = Smbios::parseRawSmbiosData(raw);
    ASSERT_TRUE(parsed.has_value());
    const Smbios::Table table = parsed.value_or(Smbios::Table{});
    EXPECT_GE(table.majorVersion, 2);
    const auto structures = Smbios::parseStructures(table.data);
    ASSERT_NE(Smbios::findFirst(structures, Smbios::TYPE_SYSTEM), nullptr);
    EXPECT_FALSE(Smbios::decodeSystem(*Smbios::findFirst(structures, Smbios::TYPE_SYSTEM), table).manufacturer.empty());

    WindowsSystemInfoProbe probe;
    const FirmwareInfo info = probe.readFirmware();
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.systemManufacturer.empty());
    EXPECT_FALSE(info.biosVersion.empty());
    EXPECT_FALSE(info.smbiosVersion.empty());
    EXPECT_NE(info.firmwareMode, FirmwareMode::Unknown);
}

TEST(WindowsSystemInfoProbeTest, ReadsMemoryModules)
{
    WindowsSystemInfoProbe probe;
    const MemoryModulesInfo info = probe.readMemoryModules();
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.tableNeedsAdmin);
    EXPECT_GT(info.usableBytes, 0U);
    // A VM's or a test runner's firmware may list no memory devices; when it lists some, they are
    // consistent.
    if (info.tableRead && !info.modules.empty())
    {
        EXPECT_GE(info.slotCount, info.modules.size());
    }
    if (info.installedBytes != 0)
    {
        EXPECT_GE(info.installedBytes, info.usableBytes);
    }
}

BOOL WINAPI installedSixteenGib(PULONGLONG kib)
{
    *kib = 16ULL * 1024 * 1024;
    return TRUE;
}

BOOL WINAPI installedFails(PULONGLONG /*kib*/)
{
    SetLastError(ERROR_INVALID_DATA);
    return FALSE;
}

BOOL WINAPI statusFifteenGib(LPMEMORYSTATUSEX status)
{
    EXPECT_EQ(status->dwLength, sizeof(MEMORYSTATUSEX));
    status->ullTotalPhys = 15ULL * 1024 * 1024 * 1024;
    return TRUE;
}

BOOL WINAPI statusFails(LPMEMORYSTATUSEX /*status*/)
{
    return FALSE;
}

TEST(WindowsSystemInfoProbeTest, InstalledAndUsableMemoryThroughTheFunctionTable)
{
    MemoryModulesInfo info;
    readInstalledMemory(info, {.getPhysicallyInstalledSystemMemory = &installedSixteenGib, .globalMemoryStatusEx = &statusFifteenGib});
    EXPECT_EQ(info.installedBytes, 16ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(info.usableBytes, 15ULL * 1024 * 1024 * 1024);

    // A failed call leaves its value unknown and doesn't touch the other.
    MemoryModulesInfo failed;
    readInstalledMemory(failed, {.getPhysicallyInstalledSystemMemory = &installedFails, .globalMemoryStatusEx = &statusFifteenGib});
    EXPECT_EQ(failed.installedBytes, 0U);
    EXPECT_EQ(failed.usableBytes, 15ULL * 1024 * 1024 * 1024);
    MemoryModulesInfo neither;
    readInstalledMemory(neither, {.getPhysicallyInstalledSystemMemory = &installedFails, .globalMemoryStatusEx = &statusFails});
    EXPECT_EQ(neither.installedBytes, 0U);
    EXPECT_EQ(neither.usableBytes, 0U);
}

// --- Commit & paging (#1516) ---

TEST(WindowsSystemInfoProbeTest, ReadsCommitAndPageFiles)
{
    WindowsSystemInfoProbe probe;
    const CommitPagingInfo info = probe.readCommitPaging();
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_GT(info.committedBytes, 0U);
    EXPECT_GE(info.commitLimitBytes, info.committedBytes);
    EXPECT_GE(info.commitPeakBytes, info.committedBytes);
    EXPECT_GT(info.pageSizeBytes, 0U);
    EXPECT_TRUE(info.pageFilesRead);
    for (const PageFile& file : info.pageFiles)
    {
        // Every page file resolves to a DOS path, never an NT one.
        EXPECT_FALSE(file.path.starts_with("\\??\\")) << file.path;
        EXPECT_FALSE(file.path.starts_with("\\Device\\")) << file.path;
        EXPECT_GE(file.peakBytes, file.usedBytes);
    }
}

TEST(WindowsSystemInfoProbeTest, NtPathsBecomeDosPaths)
{
    using WindowsCommitPaging::ntPathToDosPath;
    const std::vector<WindowsCommitPaging::DriveDevice> drives{
        {.letter = L'C', .device = L"\\Device\\HarddiskVolume3"},
        {.letter = L'D', .device = L"\\Device\\HarddiskVolume1"},
    };
    EXPECT_EQ(ntPathToDosPath(L"\\??\\C:\\pagefile.sys", drives), L"C:\\pagefile.sys");
    EXPECT_EQ(ntPathToDosPath(L"\\\\?\\D:\\swapfile.sys", drives), L"D:\\swapfile.sys");
    EXPECT_EQ(ntPathToDosPath(L"\\??\\UNC\\server\\share\\pagefile.sys", drives), L"\\\\server\\share\\pagefile.sys");
    EXPECT_EQ(ntPathToDosPath(L"\\Device\\HarddiskVolume1\\pagefile.sys", drives), L"D:\\pagefile.sys");
    // "HarddiskVolume3" must not match "HarddiskVolume31".
    EXPECT_EQ(ntPathToDosPath(L"\\Device\\HarddiskVolume31\\pagefile.sys", drives), L"\\Device\\HarddiskVolume31\\pagefile.sys");
    EXPECT_EQ(ntPathToDosPath(L"C:\\pagefile.sys", {}), L"C:\\pagefile.sys");
}

constexpr LONG STATUS_INVALID_INFO_CLASS_VALUE = static_cast<LONG>(0xC0000003L);
constexpr std::uint64_t MIB = std::uint64_t{1024} * 1024;

/// Writes the entries Fill builds into @p out, or asks for more room the way the kernel does.
template<typename Fill> LONG writeEntries(PVOID out, ULONG length, PULONG returned, ULONG needed, Fill fill)
{
    *returned = needed;
    if (length < needed)
    {
        return WindowsCommitPaging::STATUS_INFO_LENGTH_MISMATCH_VALUE;
    }
    fill(static_cast<std::byte*>(out));
    return 0;
}

/// Two page files: one by its \??\ path, one by its device path, each name after its entry.
LONG writePageFiles(PVOID out, ULONG length, PULONG returned)
{
    static const std::array<std::wstring_view, 2> NAMES{L"\\??\\C:\\pagefile.sys", L"\\Device\\HarddiskVolume5\\swapfile.sys"};
    constexpr std::size_t STRIDE = 128; // an entry and its name, 8-byte aligned
    return writeEntries(out,
                        length,
                        returned,
                        static_cast<ULONG>(STRIDE * NAMES.size()),
                        [](std::byte* base)
                        {
                            for (std::size_t i = 0; i < NAMES.size(); ++i)
                            {
                                std::byte* at = base + (i * STRIDE);
                                auto* name = reinterpret_cast<wchar_t*>(at + sizeof(WindowsCommitPaging::PageFileEntry)); // NOLINT
                                std::memcpy(name, NAMES.at(i).data(), NAMES.at(i).size() * sizeof(wchar_t));
                                WindowsCommitPaging::PageFileEntry entry{};
                                entry.nextEntryOffset = i + 1 < NAMES.size() ? static_cast<ULONG>(STRIDE) : 0;
                                entry.totalSize = static_cast<ULONG>((i + 1) * 4096); // 16 MiB, 32 MiB in 4 KiB pages
                                entry.totalInUse = 256;
                                entry.peakUsage = 512;
                                entry.pageFileName.Length = static_cast<USHORT>(NAMES.at(i).size() * sizeof(wchar_t));
                                entry.pageFileName.MaximumLength = entry.pageFileName.Length;
                                entry.pageFileName.Buffer = name;
                                std::memcpy(at, &entry, sizeof(entry));
                            }
                        });
}

/// The idle process (no name), then "Memory Compression" with a 300 MiB working set.
LONG writeProcesses(PVOID out, ULONG length, PULONG returned)
{
    static constexpr std::wstring_view NAME = L"Memory Compression";
    constexpr std::size_t STRIDE = sizeof(SYSTEM_PROCESS_INFORMATION) + 64;
    return writeEntries(out,
                        length,
                        returned,
                        static_cast<ULONG>(STRIDE * 2),
                        [](std::byte* base)
                        {
                            SYSTEM_PROCESS_INFORMATION idle{};
                            idle.NextEntryOffset = static_cast<ULONG>(STRIDE);
                            std::memcpy(base, &idle, sizeof(idle));
                            std::byte* at = base + STRIDE;
                            auto* name = reinterpret_cast<wchar_t*>(at + sizeof(SYSTEM_PROCESS_INFORMATION)); // NOLINT
                            std::memcpy(name, NAME.data(), NAME.size() * sizeof(wchar_t));
                            SYSTEM_PROCESS_INFORMATION compression{};
                            compression.ImageName.Length = static_cast<USHORT>(NAME.size() * sizeof(wchar_t));
                            compression.ImageName.MaximumLength = compression.ImageName.Length;
                            compression.ImageName.Buffer = name;
                            compression.WorkingSetSize = 300 * MIB;
                            std::memcpy(at, &compression, sizeof(compression));
                        });
}

/// The _EX class is unknown (older Windows): the reader falls back to the plain one.
LONG NTAPI fakeQuery(ULONG infoClass, PVOID out, ULONG length, PULONG returned)
{
    switch (infoClass)
    {
    case WindowsCommitPaging::SYSTEM_PAGE_FILE_INFORMATION_CLASS:
        return writePageFiles(out, length, returned);
    case WindowsCommitPaging::SYSTEM_PROCESS_INFORMATION_CLASS:
        return writeProcesses(out, length, returned);
    default:
        return STATUS_INVALID_INFO_CLASS_VALUE;
    }
}

LONG NTAPI failingQuery(ULONG /*infoClass*/, PVOID /*out*/, ULONG /*length*/, PULONG /*returned*/)
{
    return STATUS_INVALID_INFO_CLASS_VALUE;
}

BOOL WINAPI performance(PPERFORMANCE_INFORMATION info, DWORD size)
{
    EXPECT_EQ(size, sizeof(PERFORMANCE_INFORMATION));
    info->PageSize = 4096;
    info->CommitTotal = 1000;
    info->CommitLimit = 4000;
    info->CommitPeak = 2000;
    return TRUE;
}

BOOL WINAPI performanceFails(PPERFORMANCE_INFORMATION /*info*/, DWORD /*size*/)
{
    return FALSE;
}

DWORD WINAPI drivesCAndE()
{
    return (1U << 2U) | (1U << 4U);
}

DWORD WINAPI dosDevice(LPCWSTR name, LPWSTR target, DWORD size)
{
    const std::wstring_view device = name[0] == L'E' ? L"\\Device\\HarddiskVolume5" : L"\\Device\\HarddiskVolume3";
    EXPECT_GT(size, device.size() + 1);
    std::memcpy(target, device.data(), device.size() * sizeof(wchar_t));
    target[device.size()] = L'\0';
    target[device.size() + 1] = L'\0';
    return static_cast<DWORD>(device.size() + 2);
}

TEST(WindowsSystemInfoProbeTest, CommitPagingThroughTheFunctionTable)
{
    const WindowsCommitPaging::Functions fakes{
        .getPerformanceInfo = &performance,
        .ntQuerySystemInformation = &fakeQuery,
        .getLogicalDrives = &drivesCAndE,
        .queryDosDevice = &dosDevice,
    };
    CommitPagingInfo info;
    WindowsCommitPaging::readCommitPaging(info, fakes);
    EXPECT_EQ(info.committedBytes, 1000ULL * 4096);
    EXPECT_EQ(info.commitLimitBytes, 4000ULL * 4096);
    EXPECT_EQ(info.commitPeakBytes, 2000ULL * 4096);
    EXPECT_EQ(info.pageSizeBytes, 4096U);
    EXPECT_TRUE(info.pageFilesRead);
    ASSERT_EQ(info.pageFiles.size(), 2U);
    EXPECT_EQ(info.pageFiles[0].path, "C:\\pagefile.sys");
    EXPECT_EQ(info.pageFiles[0].sizeBytes, 16 * MIB);
    EXPECT_EQ(info.pageFiles[0].usedBytes, 1 * MIB);
    EXPECT_EQ(info.pageFiles[0].peakBytes, 2 * MIB);
    EXPECT_EQ(info.pageFiles[1].path, "E:\\swapfile.sys");
    EXPECT_EQ(info.pageFiles[1].sizeBytes, 32 * MIB);
    EXPECT_EQ(info.compressedBytes, 300 * MIB);

    // Every call failing: nothing known, nothing thrown.
    const WindowsCommitPaging::Functions failing{
        .getPerformanceInfo = &performanceFails,
        .ntQuerySystemInformation = &failingQuery,
        .getLogicalDrives = &drivesCAndE,
        .queryDosDevice = &dosDevice,
    };
    CommitPagingInfo failed;
    WindowsCommitPaging::readCommitPaging(failed, failing);
    EXPECT_TRUE(failed.available);
    EXPECT_EQ(failed.committedBytes, 0U);
    EXPECT_EQ(failed.pageSizeBytes, 0U);
    EXPECT_FALSE(failed.pageFilesRead);
    EXPECT_FALSE(failed.compressedBytes.has_value());
}

TEST(WindowsSystemInfoProbeTest, MalformedPageFileBuffersStayInBounds)
{
    // A name pointing outside the buffer is left empty; a link shorter than an entry ends the walk.
    std::vector<std::byte> buffer(sizeof(WindowsCommitPaging::PageFileEntry) * 2);
    WindowsCommitPaging::PageFileEntry entry{};
    entry.nextEntryOffset = 4;
    entry.totalSize = 7;
    entry.pageFileName.Length = 64;
    entry.pageFileName.Buffer = nullptr;
    std::memcpy(buffer.data(), &entry, sizeof(entry));
    const auto files = WindowsCommitPaging::parsePageFiles(buffer);
    ASSERT_EQ(files.size(), 1U);
    EXPECT_TRUE(files[0].ntPath.empty());
    EXPECT_EQ(files[0].totalPages, 7U);
    EXPECT_TRUE(WindowsCommitPaging::parsePageFiles(std::span(buffer.data(), 3)).empty());
    EXPECT_FALSE(WindowsCommitPaging::memoryCompressionWorkingSet(buffer).has_value());
}

// --- Storage (#1517) ---

TEST(WindowsSystemInfoProbeTest, ReadsDisksAndVolumes)
{
    WindowsSystemInfoProbe probe;
    const StorageInfo info = probe.readStorage();
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.disksRead);
    ASSERT_TRUE(info.volumesRead);
    // The system drive is always a local volume with a size.
    const auto system = std::ranges::find_if(info.volumes, [](const Volume& volume) { return volume.mountPoint == "C:"; });
    ASSERT_NE(system, info.volumes.end());
    EXPECT_TRUE(system->sizeRead);
    EXPECT_GT(system->sizeBytes, 0U);
    EXPECT_FALSE(system->fileSystem.empty());
    for (const PhysicalDisk& disk : info.disks)
    {
        EXPECT_TRUE(disk.name.starts_with("Disk ")) << disk.name;
    }
    // The layout needs no access beyond the query-only handle (#1632): read without administrator.
    const auto partitioned = std::ranges::find_if(info.disks, [](const PhysicalDisk& disk) { return disk.partitionsRead; });
    if (partitioned != info.disks.end())
    {
        // The system drive lives on a partition of a basic disk.
        const bool systemFound = std::ranges::any_of(
            info.disks,
            [](const PhysicalDisk& disk)
            {
                return std::ranges::any_of(disk.partitions, [](const Partition& partition) { return partition.mountPoint.contains("C:"); });
            });
        EXPECT_TRUE(systemFound);
    }
}

// DRIVE_LAYOUT_INFORMATION_EX as PartitionTable.h reads it, checked against <winioctl.h> (#1632).
static_assert(PartitionTable::LAYOUT_HEADER_BYTES == offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry));
static_assert(PartitionTable::LAYOUT_ENTRY_BYTES == sizeof(PARTITION_INFORMATION_EX));
static_assert(PartitionTable::ENTRY_STARTING_OFFSET == offsetof(PARTITION_INFORMATION_EX, StartingOffset));
static_assert(PartitionTable::ENTRY_LENGTH == offsetof(PARTITION_INFORMATION_EX, PartitionLength));
static_assert(PartitionTable::ENTRY_NUMBER == offsetof(PARTITION_INFORMATION_EX, PartitionNumber));
static_assert(PartitionTable::ENTRY_TYPE == offsetof(PARTITION_INFORMATION_EX, Mbr.PartitionType));
static_assert(PartitionTable::ENTRY_TYPE == offsetof(PARTITION_INFORMATION_EX, Gpt.PartitionType));
static_assert(PartitionTable::STYLE_MBR == PARTITION_STYLE_MBR);
static_assert(PartitionTable::STYLE_GPT == PARTITION_STYLE_GPT);
static_assert(PartitionTable::STYLE_RAW == PARTITION_STYLE_RAW);

TEST(WindowsSystemInfoProbeTest, LayoutParserMatchesWinIoctlStructs)
{
    // A layout built with <winioctl.h>'s own structs parses the same as the hand-built one.
    std::vector<std::byte> buffer(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + (2 * sizeof(PARTITION_INFORMATION_EX)));
    DRIVE_LAYOUT_INFORMATION_EX header{};
    header.PartitionStyle = PARTITION_STYLE_GPT;
    header.PartitionCount = 2;
    std::memcpy(buffer.data(), &header, offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry));
    PARTITION_INFORMATION_EX efi{};
    efi.PartitionStyle = PARTITION_STYLE_GPT;
    efi.StartingOffset.QuadPart = 1LL << 20U;
    efi.PartitionLength.QuadPart = 260LL << 20U;
    efi.PartitionNumber = 1;
    efi.Gpt.PartitionType = GUID{0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
    PARTITION_INFORMATION_EX data = efi;
    data.StartingOffset.QuadPart = 277LL << 20U;
    data.PartitionLength.QuadPart = 930LL << 30U;
    data.PartitionNumber = 3;
    data.Gpt.PartitionType = GUID{0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};
    std::memcpy(buffer.data() + offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry), &efi, sizeof(efi));
    std::memcpy(buffer.data() + offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + sizeof(efi), &data, sizeof(data));

    const std::optional<PartitionTable::DriveLayout> layout = PartitionTable::parseDriveLayout(buffer);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->style, PartitionStyle::Gpt);
    ASSERT_EQ(layout->partitions.size(), 2U);
    EXPECT_EQ(layout->partitions[0].typeName, "EFI System");
    EXPECT_EQ(layout->partitions[0].offsetBytes, 1ULL << 20U);
    EXPECT_EQ(layout->partitions[1].number, 3U);
    EXPECT_EQ(layout->partitions[1].typeName, "Basic data");
    EXPECT_EQ(layout->partitions[1].sizeBytes, 930ULL << 30U);

    PARTITION_INFORMATION_EX mbr{};
    mbr.PartitionStyle = PARTITION_STYLE_MBR;
    mbr.StartingOffset.QuadPart = 1LL << 20U;
    mbr.PartitionLength.QuadPart = 1LL << 30U;
    mbr.PartitionNumber = 1;
    mbr.Mbr.PartitionType = PARTITION_IFS;
    header.PartitionStyle = PARTITION_STYLE_MBR;
    header.PartitionCount = 1;
    std::memcpy(buffer.data(), &header, offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry));
    std::memcpy(buffer.data() + offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry), &mbr, sizeof(mbr));
    const std::optional<PartitionTable::DriveLayout> mbrLayout = PartitionTable::parseDriveLayout(buffer);
    ASSERT_EQ(mbrLayout.value_or(PartitionTable::DriveLayout{}).partitions.size(), 1U);
    EXPECT_EQ(mbrLayout->partitions[0].typeName, "NTFS/exFAT");
}

/// A STORAGE_DEVICE_DESCRIPTOR with its strings after it, as the driver returns it.
[[nodiscard]] std::vector<std::byte> deviceDescriptor(
    std::string_view vendor, std::string_view product, std::string_view revision, std::string_view serial, STORAGE_BUS_TYPE bus)
{
    std::vector<std::byte> buffer(sizeof(STORAGE_DEVICE_DESCRIPTOR));
    const auto append = [&buffer](std::string_view text) -> DWORD
    {
        if (text.empty())
        {
            return 0;
        }
        const auto offset = static_cast<DWORD>(buffer.size());
        for (const char c : text)
        {
            buffer.push_back(static_cast<std::byte>(c));
        }
        buffer.push_back(std::byte{0});
        return offset;
    };
    STORAGE_DEVICE_DESCRIPTOR header{};
    header.Version = sizeof(header);
    header.VendorIdOffset = append(vendor);
    header.ProductIdOffset = append(product);
    header.ProductRevisionOffset = append(revision);
    header.SerialNumberOffset = append(serial);
    header.BusType = bus;
    header.Size = static_cast<DWORD>(buffer.size());
    std::memcpy(buffer.data(), &header, sizeof(header));
    return buffer;
}

TEST(WindowsSystemInfoProbeTest, ParsesDeviceDescriptorsAndHealthLogs)
{
    const auto nvme = WindowsStorage::parseDeviceDescriptor(
        deviceDescriptor("NVMe", "Samsung SSD 980 PRO 1TB", "5B2QGXA7", "  S5GX_1234.  ", BusTypeNvme));
    ASSERT_TRUE(nvme.has_value());
    EXPECT_EQ(nvme.value_or(WindowsStorage::DeviceDescriptor{}).model, "Samsung SSD 980 PRO 1TB"); // "NVMe" vendor dropped
    EXPECT_EQ(nvme.value_or(WindowsStorage::DeviceDescriptor{}).serial, "S5GX_1234.");             // trimmed
    EXPECT_EQ(WindowsStorage::busTypeName(nvme.value_or(WindowsStorage::DeviceDescriptor{}).busType), "NVMe");

    const auto usb = WindowsStorage::parseDeviceDescriptor(deviceDescriptor("SanDisk", "Ultra", "1.00", "", BusTypeUsb));
    EXPECT_EQ(usb.value_or(WindowsStorage::DeviceDescriptor{}).model, "SanDisk Ultra");
    EXPECT_EQ(usb.value_or(WindowsStorage::DeviceDescriptor{}).serial, "");

    // Offsets past the end leave strings empty; a short buffer isn't a descriptor.
    std::vector<std::byte> bad = deviceDescriptor("", "X", "", "", BusTypeSata);
    STORAGE_DEVICE_DESCRIPTOR header{};
    std::memcpy(&header, bad.data(), sizeof(header));
    header.ProductIdOffset = 5000;
    std::memcpy(bad.data(), &header, sizeof(header));
    EXPECT_EQ(WindowsStorage::parseDeviceDescriptor(bad).value_or(WindowsStorage::DeviceDescriptor{}).model, "");
    EXPECT_FALSE(WindowsStorage::parseDeviceDescriptor(std::span(bad.data(), 8)).has_value());
    EXPECT_EQ(WindowsStorage::busTypeName(999), "");

    std::array<std::byte, WindowsStorage::NVME_LOG_PAGE_BYTES> page{};
    page[0] = std::byte{0x04};
    page[3] = std::byte{100};
    page[5] = std::byte{3};
    page[160] = std::byte{0x02};
    page[161] = std::byte{0x01};
    const auto health = WindowsStorage::parseNvmeHealthLog(page);
    ASSERT_TRUE(health.has_value());
    EXPECT_EQ(health.value_or(NvmeHealth{}).criticalWarning, 0x04);
    EXPECT_EQ(health.value_or(NvmeHealth{}).availableSparePercent, 100);
    EXPECT_EQ(health.value_or(NvmeHealth{}).percentageUsed, 3);
    EXPECT_EQ(health.value_or(NvmeHealth{}).mediaErrors, 0x0102U);
    EXPECT_FALSE(WindowsStorage::parseNvmeHealthLog(std::span(page.data(), 100)).has_value());

    const std::wstring_view drives{L"C:\\\0D:\\\0\0", 9};
    EXPECT_EQ(WindowsStorage::splitDriveStrings(drives), (std::vector<std::wstring>{L"C:\\", L"D:\\"}));
}

// The fake machine: Disk 0 is an NVMe SSD with a temperature and a health log; Disk 2 a SATA HDD that
// reports no temperature; the rest don't exist. Drives C: (fixed), D: (an empty optical drive), E:
// (removable, with media) and Z: (a network drive, which must never be queried).
HANDLE g_Nvme = nullptr;                           // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
bool g_HealthDenied = false;                       // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
DWORD g_LayoutError = ERROR_SUCCESS;               // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<std::pair<HANDLE, wchar_t>> g_Volumes; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<std::wstring> g_Queried;               // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<DWORD> g_ErrorModes;                   // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

Windows::UniqueHandle openFakeDrive(int index)
{
    if (index != 0 && index != 2)
    {
        return {};
    }
    Windows::UniqueHandle handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)); // any real handle to close
    // Disk 0 is closed before Disk 2 is opened, so Disk 2's handle can reuse its value.
    g_Nvme = index == 0 ? handle.get() : nullptr;
    return handle;
}

template<typename T> BOOL answer(const T& value, LPVOID out, DWORD outSize, LPDWORD returned)
{
    if (outSize < sizeof(T))
    {
        return FALSE;
    }
    std::memcpy(out, &value, sizeof(T));
    *returned = sizeof(T);
    return TRUE;
}

BOOL WINAPI fakeIoControl(
    HANDLE disk, DWORD code, LPVOID in, DWORD /*inSize*/, LPVOID out, DWORD outSize, LPDWORD returned, LPOVERLAPPED /*overlapped*/)
{
    const bool nvme = disk == g_Nvme;
    if (code == IOCTL_DISK_GET_DRIVE_LAYOUT_EX)
    {
        // Disk 0 is the Windows install disk (GPT), Disk 2 an MBR data disk.
        if (g_LayoutError != ERROR_SUCCESS)
        {
            SetLastError(g_LayoutError);
            return FALSE;
        }
        const std::vector<std::byte> layout = nvme ? PartitionTable::TestData::windowsGptLayout() : PartitionTable::TestData::mbrLayout();
        if (outSize < layout.size())
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        std::memcpy(out, layout.data(), layout.size());
        *returned = static_cast<DWORD>(layout.size());
        return TRUE;
    }
    if (code == IOCTL_STORAGE_GET_DEVICE_NUMBER)
    {
        // C: is Disk 0's partition 3, E: Disk 2's partition 2; any other volume isn't on one disk.
        const auto volume = std::ranges::find(g_Volumes, disk, &std::pair<HANDLE, wchar_t>::first);
        if (volume == g_Volumes.end() || (volume->second != L'C' && volume->second != L'E'))
        {
            SetLastError(ERROR_INVALID_FUNCTION);
            return FALSE;
        }
        STORAGE_DEVICE_NUMBER number{};
        number.DeviceType = FILE_DEVICE_DISK;
        number.DeviceNumber = volume->second == L'C' ? 0 : 2;
        number.PartitionNumber = volume->second == L'C' ? 3 : 2;
        return answer(number, out, outSize, returned);
    }
    if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY_EX)
    {
        DISK_GEOMETRY_EX geometry{};
        geometry.DiskSize.QuadPart = nvme ? 1'000'204'886'016LL : 2'000'398'934'016LL;
        return answer(geometry, out, outSize, returned);
    }
    if (code != IOCTL_STORAGE_QUERY_PROPERTY)
    {
        return FALSE;
    }
    STORAGE_PROPERTY_QUERY query{};
    std::memcpy(&query, in, offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters));
    switch (query.PropertyId)
    {
    case StorageDeviceProperty:
    {
        const std::vector<std::byte> descriptor = nvme
                                                    ? deviceDescriptor("NVMe", "Samsung SSD 980 PRO 1TB", "5B2QGXA7", "S5GX", BusTypeNvme)
                                                    : deviceDescriptor("", "WDC WD20EZRZ-00Z5HB0", "80.00A80", "WD-WCC4M", BusTypeSata);
        if (outSize < descriptor.size())
        {
            return FALSE;
        }
        std::memcpy(out, descriptor.data(), descriptor.size());
        *returned = static_cast<DWORD>(descriptor.size());
        return TRUE;
    }
    case StorageDeviceSeekPenaltyProperty:
    {
        DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
        seek.Version = sizeof(seek);
        seek.Size = sizeof(seek);
        seek.IncursSeekPenalty = nvme ? FALSE : TRUE;
        return answer(seek, out, outSize, returned);
    }
    case StorageDeviceTemperatureProperty:
    {
        if (!nvme)
        {
            return FALSE;
        }
        STORAGE_TEMPERATURE_DATA_DESCRIPTOR temperature{};
        temperature.InfoCount = 1;
        temperature.TemperatureInfo[0].Temperature = 41;
        return answer(temperature, out, outSize, returned);
    }
    case StorageDeviceProtocolSpecificProperty:
    {
        EXPECT_TRUE(nvme); // only NVMe drives are asked for the health log
        if (g_HealthDenied)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        constexpr std::size_t HEADER = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
        STORAGE_PROTOCOL_SPECIFIC_DATA data{};
        data.ProtocolDataOffset = sizeof(data);
        data.ProtocolDataLength = WindowsStorage::NVME_LOG_PAGE_BYTES;
        auto* bytes = static_cast<std::byte*>(out);
        EXPECT_GE(outSize, HEADER + sizeof(data) + WindowsStorage::NVME_LOG_PAGE_BYTES);
        std::memset(bytes, 0, outSize);
        std::memcpy(bytes + HEADER, &data, sizeof(data));
        bytes[HEADER + sizeof(data) + 5] = std::byte{7}; // percentage used
        bytes[HEADER + sizeof(data) + 3] = std::byte{99};
        *returned = outSize;
        return TRUE;
    }
    default:
        return FALSE;
    }
}

DWORD WINAPI fakeDriveStrings(DWORD size, LPWSTR buffer)
{
    constexpr std::wstring_view DRIVES{L"C:\\\0D:\\\0E:\\\0Z:\\\0\0", 17};
    if (size < DRIVES.size() || buffer == nullptr)
    {
        return static_cast<DWORD>(DRIVES.size());
    }
    std::memcpy(buffer, DRIVES.data(), DRIVES.size() * sizeof(wchar_t));
    return static_cast<DWORD>(DRIVES.size() - 1);
}

UINT WINAPI fakeDriveType(LPCWSTR root)
{
    switch (root[0])
    {
    case L'C':
        return DRIVE_FIXED;
    case L'D':
        return DRIVE_CDROM;
    case L'E':
        return DRIVE_REMOVABLE;
    default:
        return DRIVE_REMOTE;
    }
}

BOOL WINAPI fakeVolumeInformation(LPCWSTR root,
                                  LPWSTR label,
                                  DWORD labelSize,
                                  LPDWORD /*serial*/,
                                  LPDWORD /*maxComponent*/,
                                  LPDWORD /*flags*/,
                                  LPWSTR fileSystem,
                                  DWORD fileSystemSize)
{
    g_Queried.emplace_back(root);
    if (root[0] == L'D')
    {
        SetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    const std::wstring_view name = root[0] == L'C' ? L"Windows" : L"USB";
    const std::wstring_view fs = root[0] == L'C' ? L"NTFS" : L"exFAT";
    EXPECT_GT(labelSize, name.size());
    EXPECT_GT(fileSystemSize, fs.size());
    std::memcpy(label, name.data(), name.size() * sizeof(wchar_t));
    label[name.size()] = L'\0';
    std::memcpy(fileSystem, fs.data(), fs.size() * sizeof(wchar_t));
    fileSystem[fs.size()] = L'\0';
    return TRUE;
}

BOOL WINAPI fakeFreeSpace(LPCWSTR root, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER /*totalFree*/)
{
    g_Queried.emplace_back(root);
    available->QuadPart = 100ULL << 30U;
    total->QuadPart = 400ULL << 30U;
    return TRUE;
}

BOOL WINAPI fakeErrorMode(DWORD mode, LPDWORD previous)
{
    g_ErrorModes.push_back(mode);
    if (previous != nullptr)
    {
        *previous = 0x8000; // SEM_NOOPENFILEERRORBOX: restored afterwards
    }
    return TRUE;
}

Windows::UniqueHandle openFakeVolume(wchar_t letter)
{
    Windows::UniqueHandle handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::erase_if(g_Volumes, [&handle](const std::pair<HANDLE, wchar_t>& volume) { return volume.first == handle.get(); });
    g_Volumes.emplace_back(handle.get(), letter);
    return handle;
}

[[nodiscard]] WindowsStorage::Functions fakeStorage()
{
    return {
        .openPhysicalDrive = &openFakeDrive,
        .deviceIoControl = &fakeIoControl,
        .getLogicalDriveStrings = &fakeDriveStrings,
        .getDriveType = &fakeDriveType,
        .getVolumeInformation = &fakeVolumeInformation,
        .getDiskFreeSpaceEx = &fakeFreeSpace,
        .setThreadErrorMode = &fakeErrorMode,
        .openVolume = &openFakeVolume,
    };
}

TEST(WindowsSystemInfoProbeTest, StorageThroughTheFunctionTable)
{
    g_Queried.clear();
    g_Volumes.clear();
    g_ErrorModes.clear();
    g_HealthDenied = false;
    StorageInfo info;
    WindowsStorage::readStorage(info, fakeStorage());
    EXPECT_TRUE(info.available);
    ASSERT_EQ(info.disks.size(), 2U);
    const PhysicalDisk& nvme = info.disks[0];
    EXPECT_EQ(nvme.name, "Disk 0");
    EXPECT_EQ(nvme.model, "Samsung SSD 980 PRO 1TB");
    EXPECT_EQ(nvme.bus, "NVMe");
    EXPECT_EQ(nvme.media, DiskMedia::Ssd);
    EXPECT_EQ(nvme.sizeBytes, 1'000'204'886'016ULL);
    EXPECT_EQ(nvme.firmware, "5B2QGXA7");
    EXPECT_EQ(nvme.serial, "S5GX");
    EXPECT_EQ(nvme.temperatureCelsius, 41);
    ASSERT_TRUE(nvme.health.has_value());
    EXPECT_EQ(nvme.health.value_or(NvmeHealth{}).percentageUsed, 7);
    EXPECT_EQ(nvme.health.value_or(NvmeHealth{}).availableSparePercent, 99);
    const PhysicalDisk& hdd = info.disks[1];
    EXPECT_EQ(hdd.name, "Disk 2");
    EXPECT_EQ(hdd.model, "WDC WD20EZRZ-00Z5HB0");
    EXPECT_EQ(hdd.bus, "SATA");
    EXPECT_EQ(hdd.media, DiskMedia::Hdd);
    EXPECT_FALSE(hdd.temperatureCelsius.has_value());
    EXPECT_FALSE(hdd.health.has_value());
    EXPECT_TRUE(hdd.healthUnavailableReason.empty()); // not NVMe: no health row at all

    ASSERT_TRUE(info.volumesRead);
    ASSERT_EQ(info.volumes.size(), 3U); // the empty optical drive is left out
    EXPECT_EQ(info.volumes[0].mountPoint, "C:");
    EXPECT_EQ(info.volumes[0].label, "Windows");
    EXPECT_EQ(info.volumes[0].fileSystem, "NTFS");
    EXPECT_TRUE(info.volumes[0].sizeRead);
    EXPECT_EQ(info.volumes[0].freeBytes, 100ULL << 30U);
    EXPECT_EQ(info.volumes[1].mountPoint, "E:");
    EXPECT_EQ(info.volumes[2].mountPoint, "Z:");
    EXPECT_TRUE(info.volumes[2].network);
    EXPECT_FALSE(info.volumes[2].sizeRead);
    EXPECT_EQ(std::ranges::count_if(g_Queried, [](const std::wstring& root) { return root.starts_with(L'Z'); }), 0);
    // Critical-error prompts off for the calls, the old mode back after: the volumes, then their locations.
    EXPECT_EQ(g_ErrorModes, (std::vector<DWORD>{SEM_FAILCRITICALERRORS, 0x8000, SEM_FAILCRITICALERRORS, 0x8000}));

    // Partitions (#1632), each tied to the drive letter on it.
    EXPECT_TRUE(nvme.partitionsRead);
    EXPECT_EQ(nvme.partitionStyle, PartitionStyle::Gpt);
    ASSERT_EQ(nvme.partitions.size(), 4U);
    EXPECT_EQ(nvme.partitions[0].typeName, "EFI System");
    EXPECT_EQ(nvme.partitions[0].mountPoint, "");
    EXPECT_EQ(nvme.partitions[2].typeName, "Basic data");
    EXPECT_EQ(nvme.partitions[2].mountPoint, "C:");
    EXPECT_EQ(hdd.partitionStyle, PartitionStyle::Mbr);
    ASSERT_EQ(hdd.partitions.size(), 2U);
    EXPECT_EQ(hdd.partitions[0].mountPoint, "");
    EXPECT_EQ(hdd.partitions[1].mountPoint, "E:");
    // The network drive's volume is never opened.
    EXPECT_EQ(std::ranges::count(g_Volumes, L'Z', &std::pair<HANDLE, wchar_t>::second), 0);

    // Health refused to a query-only handle: says administrator, never asks for more access.
    g_HealthDenied = true;
    StorageInfo denied;
    WindowsStorage::readStorage(denied, fakeStorage());
    ASSERT_FALSE(denied.disks.empty());
    EXPECT_FALSE(denied.disks[0].health.has_value());
    EXPECT_EQ(denied.disks[0].healthUnavailableReason, "Requires administrator");
    g_HealthDenied = false;

    // A driver that refuses the layout to a query-only handle: administrator, never a read open.
    g_LayoutError = ERROR_ACCESS_DENIED;
    StorageInfo noLayout;
    WindowsStorage::readStorage(noLayout, fakeStorage());
    ASSERT_FALSE(noLayout.disks.empty());
    EXPECT_FALSE(noLayout.disks[0].partitionsRead);
    EXPECT_TRUE(noLayout.disks[0].partitions.empty());
    EXPECT_EQ(noLayout.disks[0].partitionsUnavailableReason, "Requires administrator");
    g_LayoutError = ERROR_NOT_SUPPORTED;
    StorageInfo unsupported;
    WindowsStorage::readStorage(unsupported, fakeStorage());
    ASSERT_FALSE(unsupported.disks.empty());
    EXPECT_EQ(unsupported.disks[0].partitionsUnavailableReason, "The drive didn't return its partition layout");
    g_LayoutError = ERROR_SUCCESS;
}

/// A drive with 40 partitions: more than the first try's buffer holds.
BOOL WINAPI bigLayoutIoControl(
    HANDLE /*disk*/, DWORD code, LPVOID /*in*/, DWORD /*inSize*/, LPVOID out, DWORD outSize, LPDWORD returned, LPOVERLAPPED /*overlapped*/)
{
    EXPECT_EQ(code, static_cast<DWORD>(IOCTL_DISK_GET_DRIVE_LAYOUT_EX));
    std::vector<PartitionTable::TestData::Entry> entries;
    std::uint32_t number = 1;
    while (number <= 40)
    {
        entries.push_back({.offset = number * PartitionTable::TestData::MIB,
                           .length = PartitionTable::TestData::MIB,
                           .number = number,
                           .mbrType = 0,
                           .gptType = PartitionTable::TestData::BASIC_DATA});
        ++number;
    }
    const std::vector<std::byte> layout = PartitionTable::TestData::driveLayout(PartitionTable::STYLE_GPT, entries);
    if (outSize < layout.size())
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(out, layout.data(), layout.size());
    *returned = static_cast<DWORD>(layout.size());
    return TRUE;
}

TEST(WindowsSystemInfoProbeTest, PartitionLayoutBufferGrows)
{
    WindowsStorage::Functions fns = fakeStorage();
    fns.deviceIoControl = &bigLayoutIoControl;
    PhysicalDisk disk;
    WindowsStorage::readPartitions(fns, nullptr, disk);
    EXPECT_TRUE(disk.partitionsRead);
    EXPECT_EQ(disk.partitions.size(), 40U);
    EXPECT_TRUE(disk.partitionsUnavailableReason.empty());
}

TEST(WindowsSystemInfoProbeTest, AssignsDriveLettersToPartitions)
{
    std::vector<PhysicalDisk> disks(2);
    const auto numbered = [](std::uint32_t number)
    {
        Partition partition;
        partition.number = number;
        return partition;
    };
    disks[0].partitions = {numbered(1), numbered(2)};
    disks[1].partitions = {numbered(1)};
    const std::vector<int> numbers{0, 3};
    const std::vector<WindowsStorage::VolumeLocation> locations{
        {.mountPoint = "C:", .disk = 0, .partition = 2},
        {.mountPoint = "D:", .disk = 3, .partition = 1},
        {.mountPoint = "F:", .disk = 3, .partition = 1}, // a second letter on the same partition
        {.mountPoint = "G:", .disk = 1, .partition = 1}, // a disk that wasn't opened
        {.mountPoint = "H:", .disk = 0, .partition = 9}, // a partition the layout doesn't list
    };
    WindowsStorage::assignMountPoints(disks, numbers, locations);
    EXPECT_EQ(disks[0].partitions[0].mountPoint, "");
    EXPECT_EQ(disks[0].partitions[1].mountPoint, "C:");
    EXPECT_EQ(disks[1].partitions[0].mountPoint, "D:, F:");
}

TEST(WindowsSystemInfoProbeTest, ReadsGraphics)
{
    WindowsSystemInfoProbe probe;
    const GraphicsInfo info = probe.readGraphics();
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.adaptersRead); // a DXGI factory exists on every supported Windows; a CI VM may list no GPU
    for (const GraphicsAdapter& adapter : info.adapters)
    {
        EXPECT_FALSE(adapter.name.empty());
    }
}

// The fake graphics table's state: whether DXGI can be used at all.
bool g_DxgiFails = false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] DXGI_ADAPTER_DESC1 adapterDesc(std::wstring_view name, UINT vendor, UINT device, DWORD luidLow, UINT flags)
{
    DXGI_ADAPTER_DESC1 desc{};
    std::ranges::copy(name, std::begin(desc.Description));
    desc.VendorId = vendor;
    desc.DeviceId = device;
    desc.AdapterLuid = LUID{.LowPart = luidLow, .HighPart = 0};
    desc.DedicatedVideoMemory = SIZE_T{12} << 30U;
    desc.SharedSystemMemory = SIZE_T{16} << 30U;
    desc.Flags = flags;
    return desc;
}

[[nodiscard]] FILETIME fileTimeOf(std::chrono::sys_days day)
{
    const auto days = (day - std::chrono::sys_days{std::chrono::year{1601} / 1 / 1}).count();
    const std::uint64_t ticks = static_cast<std::uint64_t>(days) * 864'000'000'000ULL;
    return FILETIME{.dwLowDateTime = static_cast<DWORD>(ticks), .dwHighDateTime = static_cast<DWORD>(ticks >> 32U)};
}

[[nodiscard]] std::optional<std::vector<WindowsGraphics::AdapterRecord>> fakeAdapters()
{
    if (g_DxgiFails)
    {
        return std::nullopt;
    }
    WindowsGraphics::AdapterRecord gpu{.desc = adapterDesc(L"NVIDIA GeForce RTX 4070", 0x10DE, 0x2786, 1, 0), .outputs = {}};
    const WindowsGraphics::OutputRecord hdr{
        .deviceName = L"\\\\.\\DISPLAY1",
        .desktop = RECT{.left = 0, .top = 0, .right = 3840, .bottom = 2160},
        .hasDesc1 = true,
        .colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
        .bitsPerColor = 10,
    };
    gpu.outputs.push_back(hdr);
    gpu.outputs.push_back(hdr); // listed twice: kept once
    const WindowsGraphics::AdapterRecord basic{
        .desc = adapterDesc(L"Microsoft Basic Render Driver", 0x1414, 0x8C, 2, DXGI_ADAPTER_FLAG_SOFTWARE),
        .outputs = {},
    };
    WindowsGraphics::AdapterRecord dock{.desc = adapterDesc(L"NVIDIA GeForce RTX 4070", 0x10DE, 0x2786, 3, 0), .outputs = {}};
    dock.outputs.push_back({
        .deviceName = L"\\\\.\\DISPLAY3",
        .desktop = RECT{.left = 3840, .top = 0, .right = 5760, .bottom = 1080},
        .hasDesc1 = false,
        .colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
        .bitsPerColor = 0,
    });
    return std::vector{gpu, basic, dock};
}

[[nodiscard]] std::vector<WindowsGraphics::DriverNode> fakeDriverNodes()
{
    using namespace std::chrono;
    return {
        {
            .hardwareId = "PCI\\VEN_10DE&DEV_2786&SUBSYS_00000000",
            .bus = 2,
            .address = 0,
            .driverVersion = "1.0",
            .driverDate = std::nullopt,
        },
        {
            .hardwareId = "pci\\ven_10de&dev_2786&subsys_00000000",
            .bus = 1,
            .address = 0,
            .driverVersion = "32.0.15.6094",
            .driverDate = fileTimeOf(sys_days{year{2024} / September / 5}),
        },
    };
}

[[nodiscard]] WindowsGraphics::Functions fakeGraphics()
{
    return {
        .listAdapters = &fakeAdapters,
        .adapterType = [](const LUID& luid) -> std::optional<AdapterTypeBits>
        { return AdapterTypeBits{.softwareDevice = false, .indirectDisplayDevice = luid.LowPart == 3}; },
        .pciLocation = [](const LUID& luid) -> std::optional<PciLocation>
        { return luid.LowPart == 1 ? std::optional(PciLocation{.bus = 1, .device = 0, .function = 0}) : std::nullopt; },
        .listDisplayDrivers = &fakeDriverNodes,
        .readMonitorEdid = [](const std::wstring& output)
        { return output == L"\\\\.\\DISPLAY1" ? TestSupport::makeEdid() : std::vector<std::uint8_t>{}; },
    };
}

TEST(WindowsSystemInfoProbeTest, GraphicsThroughTheFunctionTable)
{
    GraphicsInfo info;
    WindowsGraphics::readGraphics(info, fakeGraphics());
    ASSERT_TRUE(info.adaptersRead);
    ASSERT_EQ(info.adapters.size(), 1U); // the software and indirect-display adapters are left out
    const GraphicsAdapter& gpu = info.adapters[0];
    EXPECT_EQ(gpu.name, "NVIDIA GeForce RTX 4070");
    EXPECT_EQ(gpu.location, "01:00.0");
    EXPECT_EQ(gpu.dedicatedBytes, 12ULL << 30U);
    EXPECT_EQ(gpu.driverVersion, "32.0.15.6094"); // the node at its PCI location, not the first with its ids
    EXPECT_EQ(gpu.driverDate, "2024-09-05");

    ASSERT_TRUE(info.monitorsRead);
    ASSERT_EQ(info.monitors.size(), 2U); // the indirect display's monitor is kept
    EXPECT_EQ(info.monitors[0].name, "DELL U2720Q");
    EXPECT_EQ(info.monitors[0].serial, "ABC1234");
    EXPECT_TRUE(info.monitors[0].hdr);
    EXPECT_EQ(info.monitors[0].colorSpace, "BT.2020 PQ");
    EXPECT_EQ(info.monitors[0].bitsPerColor, 10U);
    EXPECT_EQ(info.monitors[0].desktopWidth, 3840);
    EXPECT_TRUE(info.monitors[1].name.empty()); // no EDID
    EXPECT_TRUE(info.monitors[1].colorSpace.empty());
    EXPECT_EQ(info.monitors[1].desktopX, 3840);

    g_DxgiFails = true;
    GraphicsInfo failed;
    WindowsGraphics::readGraphics(failed, fakeGraphics());
    g_DxgiFails = false;
    EXPECT_TRUE(failed.available);
    EXPECT_FALSE(failed.adaptersRead);
}

TEST(WindowsSystemInfoProbeTest, GraphicsHelpers)
{
    using namespace std::chrono;
    EXPECT_EQ(WindowsGraphics::fileTimeDate(fileTimeOf(sys_days{year{2024} / September / 5})), "2024-09-05");
    EXPECT_EQ(WindowsGraphics::colorSpaceName(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709), "sRGB");
    EXPECT_FALSE(WindowsGraphics::isHdrColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    EXPECT_TRUE(WindowsGraphics::isHdrColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));

    // Without a PCI location the first node with the ids is used; with one, only the node there.
    const std::vector<WindowsGraphics::DriverNode> nodes{
        {.hardwareId = "PCI\\VEN_8086&DEV_A7A0", .bus = 0, .address = 0x20000, .driverVersion = "31.0", .driverDate = std::nullopt},
    };
    const auto* node = WindowsGraphics::findDriverNode(nodes, 0x8086, 0xA7A0, std::nullopt);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->driverVersion, "31.0");
    EXPECT_NE(WindowsGraphics::findDriverNode(nodes, 0x8086, 0xA7A0, PciLocation{.bus = 0, .device = 2, .function = 0}), nullptr);
    EXPECT_EQ(WindowsGraphics::findDriverNode(nodes, 0x8086, 0xA7A0, PciLocation{.bus = 0, .device = 3, .function = 0}), nullptr);
    EXPECT_EQ(WindowsGraphics::findDriverNode(nodes, 0x10DE, 0xA7A0, std::nullopt), nullptr);
}

TEST(WindowsSystemInfoProbeTest, ReadsDevices)
{
    WindowsSystemInfoProbe probe;
    const DevicesInfo info = probe.readDevices();
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.pciRead); // SetupAPI lists every present device; a VM may have no PCI bus at all
    for (const Device& device : info.problems)
    {
        EXPECT_FALSE(device.problem.empty());
    }
}

TEST(WindowsSystemInfoProbeTest, ReadsDrivers)
{
    WindowsSystemInfoProbe probe;
    const DriversInfo info = probe.readDrivers();
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    ASSERT_TRUE(info.listed); // the SCM lists driver services without administrator
    EXPECT_FALSE(info.drivers.empty());
    const bool anyVersioned = std::ranges::any_of(info.drivers, [](const KernelDriver& driver) { return !driver.version.empty(); });
    EXPECT_TRUE(anyVersioned); // the in-box drivers carry a version resource
    for (const KernelDriver& driver : info.drivers)
    {
        EXPECT_FALSE(driver.name.empty());
        EXPECT_TRUE(driver.moduleState.empty());
    }
}

TEST(WindowsSystemInfoProbeTest, ReadsCrashes)
{
    WindowsSystemInfoProbe probe;
    const CrashesInfo info = probe.readCrashes();
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    ASSERT_TRUE(info.listed) << info.unavailableReason; // the Application log is readable without administrator
    EXPECT_LE(info.events.size(), CRASH_LIST_MAX);
    for (std::size_t i = 1; i < info.events.size(); ++i)
    {
        EXPECT_GE(info.events[i - 1].unixSeconds, info.events[i].unixSeconds); // newest first
    }
}

// The fake devices table's state: whether SetupAPI and MMDevice can be used.
bool g_DevicesFail = false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] WindowsDevices::DevNodeRecord devNode(std::string id, std::string parent, std::string name, ULONG problem = 0)
{
    WindowsDevices::DevNodeRecord node;
    node.instanceId = std::move(id);
    node.parentId = std::move(parent);
    node.name = std::move(name);
    node.statusRead = true;
    node.status = DN_DRIVER_LOADED | DN_STARTED | (problem != 0 ? DN_HAS_PROBLEM : 0U);
    node.problem = problem;
    return node;
}

[[nodiscard]] std::optional<std::vector<WindowsDevices::DevNodeRecord>> fakeDevNodes()
{
    if (g_DevicesFail)
    {
        return std::nullopt;
    }
    WindowsDevices::DevNodeRecord gpu =
        devNode(R"(PCI\VEN_10DE&DEV_2786&SUBSYS_00000000&REV_A1\4&1&0&0008)", "ACPI\\PNP0A08\\0", "NVIDIA GeForce RTX 4070");
    gpu.manufacturer = "NVIDIA";
    gpu.className = "Display adapters";
    gpu.service = "nvlddmkm";
    gpu.bus = 1;
    gpu.address = 0;
    WindowsDevices::DevNodeRecord wifi =
        devNode(R"(PCI\VEN_8086&DEV_51F0&SUBSYS_00948086&REV_01\3&2&0&A3)", "ACPI\\PNP0A08\\0", "Wireless adapter", CM_PROB_FAILED_START);
    wifi.manufacturer = "(Standard system devices)";
    wifi.bus = 0;
    wifi.address = (0x14U << 16U) | 3U;
    WindowsDevices::DevNodeRecord odd = devNode(R"(ROOT\ODD\0000)", "HTREE\\ROOT\\0", "", 99); // no name of its own
    odd.manufacturer = "Contoso";
    WindowsDevices::DevNodeRecord receiver =
        devNode(R"(USB\VID_046D&PID_C52B\5&3&0&2)", R"(USB\VID_05E3&PID_0610\6&4&0&1)", "USB Receiver");
    receiver.locationInfo = "Port_#0002.Hub_#0003";
    WindowsDevices::DevNodeRecord hub = devNode(R"(USB\VID_05E3&PID_0610\6&4&0&1)", R"(USB\ROOT_HUB30\4&6&0&0)", "Generic USB Hub");
    hub.address = 1; // no location information: the port is its address
    WindowsDevices::DevNodeRecord storage =
        devNode(R"(USB\VID_0781&PID_5583\4C530001)", R"(USB\ROOT_HUB30\4&6&0&0)", "USB Mass Storage Device");
    storage.locationInfo = "Port_#0004.Hub_#0001";
    storage.address = 9; // the location information wins
    return std::vector{
        gpu,
        wifi,
        receiver, // listed before its hub
        devNode(R"(USB\VID_046D&PID_C52B&MI_00\6&5&0&0000)", R"(USB\VID_046D&PID_C52B\5&3&0&2)", "Interface"),
        hub,
        storage,
        devNode(R"(USB\ROOT_HUB30\4&6&0&0)", R"(PCI\VEN_8086&DEV_51ED\3&0)", "USB Root Hub (USB 3.0)"),
        devNode(R"(ROOT\DISABLED\0000)", "HTREE\\ROOT\\0", "Disabled thing", CM_PROB_DISABLED),
        odd,
    };
}

[[nodiscard]] std::optional<std::vector<WindowsDevices::AudioRecord>> fakeAudio()
{
    if (g_DevicesFail)
    {
        return std::nullopt;
    }
    return std::vector<WindowsDevices::AudioRecord>{
        {.name = "Speakers (Realtek(R) Audio)", .capture = false},
        {.name = "Microphone (USB Audio)", .capture = true},
    };
}

// The fake hubs' state: whether they can be opened, and which ports of which hub were asked for.
bool g_HubsFail = false;                         // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<std::string> g_HubsAsked;            // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<std::vector<ULONG>> g_HubPortsAsked; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// A hub's answer for a port: the EX speed, the EX_V2 flags and the SuperSpeedPlus lane speed and count.
[[nodiscard]] WindowsDevices::UsbPortRecord portAnswer(std::optional<UCHAR> speed,
                                                       bool superSpeed = false,
                                                       bool superSpeedPlus = false,
                                                       std::optional<ULONG> laneSpeed = std::nullopt,
                                                       ULONG lanesLessOne = 0)
{
    return {
        .speed = speed,
        .superSpeed = superSpeed,
        .superSpeedPlus = superSpeedPlus,
        .superSpeedPlusLaneSpeed = laneSpeed,
        .superSpeedPlusLanes = lanesLessOne,
    };
}

// A SuperSpeedPlus lane speed as the hub gives it: mantissa in bits 16-31, exponent 3 (Gb/s) in bits 4-5.
constexpr ULONG LANE_10_GBPS = (10UL << 16U) | (3UL << 4U);

/// The root hub answers for its port 1 (EX only: High-Speed) and port 4 (a SuperSpeedPlus Gen 2x2 link);
/// the external hub answers for its port 2 at Full-Speed.
[[nodiscard]] std::map<ULONG, WindowsDevices::UsbPortRecord> fakeHubPorts(const std::string& hubInstanceId, const std::vector<ULONG>& ports)
{
    g_HubsAsked.push_back(hubInstanceId);
    g_HubPortsAsked.push_back(ports);
    std::map<ULONG, WindowsDevices::UsbPortRecord> answers;
    if (g_HubsFail)
    {
        return answers;
    }
    if (hubInstanceId == R"(USB\ROOT_HUB30\4&6&0&0)")
    {
        answers[1] = portAnswer(UsbHighSpeed);
        answers[4] = portAnswer(UsbSuperSpeed, true, true, LANE_10_GBPS, 1);
    }
    else if (hubInstanceId == R"(USB\VID_05E3&PID_0610\6&4&0&1)")
    {
        answers[2] = portAnswer(UsbFullSpeed);
    }
    return answers;
}

TEST(WindowsSystemInfoProbeTest, DevicesThroughTheFunctionTable)
{
    DevicesInfo info;
    WindowsDevices::readDevices(info, {.listDevNodes = &fakeDevNodes, .listAudioEndpoints = &fakeAudio});
    ASSERT_TRUE(info.pciRead);
    ASSERT_EQ(info.pci.size(), 2U);
    EXPECT_EQ(info.pci[0].location, "00:14.3"); // sorted by location
    EXPECT_TRUE(info.pci[0].vendor.empty());    // named: the manufacturer isn't needed
    EXPECT_EQ(info.pci[0].problem, "This device cannot start (Code 10)");
    const Device& gpu = info.pci[1];
    EXPECT_EQ(gpu.name, "NVIDIA GeForce RTX 4070");
    EXPECT_TRUE(gpu.vendor.empty());
    EXPECT_EQ(gpu.vendorId, 0x10DEU);
    EXPECT_EQ(gpu.productId, 0x2786U);
    EXPECT_EQ(gpu.className, "Display adapters");
    EXPECT_EQ(gpu.driver, "nvlddmkm");
    EXPECT_EQ(gpu.location, "01:00.0");
    EXPECT_TRUE(gpu.problem.empty());

    ASSERT_TRUE(info.problemsRead);
    ASSERT_EQ(info.problems.size(), 3U);
    EXPECT_EQ(info.problems[1].problem, "This device is disabled (Code 22)");
    EXPECT_EQ(info.problems[2].problem, "Problem code 99");
    EXPECT_EQ(info.problems[2].vendor, "Contoso"); // no name: the manufacturer is kept

    ASSERT_TRUE(info.usbRead);
    ASSERT_EQ(info.usb.size(), 3U); // the root hub and the composite device's interface left out
    EXPECT_EQ(info.usb[0].name, "Generic USB Hub");
    EXPECT_EQ(info.usb[0].depth, 1U);
    EXPECT_EQ(info.usb[1].name, "USB Receiver"); // after its hub
    EXPECT_EQ(info.usb[1].depth, 2U);
    EXPECT_EQ(info.usb[1].parent, std::optional<std::size_t>(0));
    EXPECT_EQ(info.usb[1].vendorId, 0x046DU);
    EXPECT_EQ(info.usb[1].productId, 0xC52BU);
    EXPECT_TRUE(info.usb[1].serial.empty()); // a generated instance id: no serial number
    EXPECT_EQ(info.usb[2].serial, "4C530001");
    EXPECT_EQ(info.usb[2].depth, 1U);
    for (const Device& usb : info.usb)
    {
        EXPECT_DOUBLE_EQ(usb.speedMbps, 0.0); // no hub reader: no speed
    }

    ASSERT_TRUE(info.audioRead);
    ASSERT_EQ(info.audio.size(), 2U);
    EXPECT_EQ(info.audio[0].flow, AudioFlow::Output);
    EXPECT_EQ(info.audio[1].flow, AudioFlow::Input);

    g_DevicesFail = true;
    DevicesInfo failed;
    WindowsDevices::readDevices(failed, {.listDevNodes = &fakeDevNodes, .listAudioEndpoints = &fakeAudio});
    g_DevicesFail = false;
    EXPECT_TRUE(failed.available);
    EXPECT_FALSE(failed.pciRead);
    EXPECT_FALSE(failed.problemsRead);
    EXPECT_FALSE(failed.audioRead);
}

TEST(WindowsSystemInfoProbeTest, UsbSpeedsFromTheHubs)
{
    g_HubsAsked.clear();
    g_HubPortsAsked.clear();
    DevicesInfo info;
    WindowsDevices::readDevices(info, {.listDevNodes = &fakeDevNodes, .listAudioEndpoints = &fakeAudio, .readHubPorts = &fakeHubPorts});
    ASSERT_EQ(info.usb.size(), 3U);
    EXPECT_EQ(info.usb[0].name, "Generic USB Hub");
    EXPECT_DOUBLE_EQ(info.usb[0].speedMbps, 480.0);   // root hub port 1, from its address
    EXPECT_DOUBLE_EQ(info.usb[1].speedMbps, 12.0);    // the external hub's port 2, from its location
    EXPECT_DOUBLE_EQ(info.usb[2].speedMbps, 20000.0); // root hub port 4: two 10 Gbit/s lanes
    // One call per hub, with every port wanted on it; the composite device's interface isn't asked for.
    ASSERT_EQ(g_HubsAsked.size(), 2U);
    EXPECT_EQ(g_HubsAsked[0], R"(USB\ROOT_HUB30\4&6&0&0)");
    EXPECT_EQ(g_HubPortsAsked[0], (std::vector<ULONG>{1, 4}));
    EXPECT_EQ(g_HubsAsked[1], R"(USB\VID_05E3&PID_0610\6&4&0&1)");
    EXPECT_EQ(g_HubPortsAsked[1], (std::vector<ULONG>{2}));

    // A hub that can't be opened: no speed, and nothing else lost.
    g_HubsFail = true;
    DevicesInfo failed;
    WindowsDevices::readDevices(failed, {.listDevNodes = &fakeDevNodes, .listAudioEndpoints = &fakeAudio, .readHubPorts = &fakeHubPorts});
    g_HubsFail = false;
    ASSERT_EQ(failed.usb.size(), 3U);
    for (const Device& usb : failed.usb)
    {
        EXPECT_DOUBLE_EQ(usb.speedMbps, 0.0);
        EXPECT_TRUE(usb.problem.empty()); // a failed speed read isn't a problem
    }
    EXPECT_EQ(failed.problems.size(), 3U);
    EXPECT_TRUE(failed.audioRead);
}

TEST(WindowsSystemInfoProbeTest, UsbPortAndSpeedHelpers)
{
    EXPECT_EQ(WindowsDevices::portFromLocation("Port_#0003.Hub_#0001"), std::optional<ULONG>(3));
    EXPECT_EQ(WindowsDevices::portFromLocation("Port_#0012.Hub_#0004"), std::optional<ULONG>(12));
    EXPECT_EQ(WindowsDevices::portFromLocation("Port_#0000.Hub_#0001"), std::nullopt); // ports are one based
    EXPECT_EQ(WindowsDevices::portFromLocation("Port_#.Hub_#0001"), std::nullopt);
    EXPECT_EQ(WindowsDevices::portFromLocation("0000.0014.0000.001.003.000.000.000.000"), std::nullopt);
    EXPECT_EQ(WindowsDevices::portFromLocation(""), std::nullopt);

    WindowsDevices::DevNodeRecord node;
    EXPECT_EQ(WindowsDevices::usbPort(node), std::nullopt);
    node.address = 0;
    EXPECT_EQ(WindowsDevices::usbPort(node), std::nullopt);
    node.address = 5;
    EXPECT_EQ(WindowsDevices::usbPort(node), std::optional<ULONG>(5));
    node.locationInfo = "Port_#0007.Hub_#0002";
    EXPECT_EQ(WindowsDevices::usbPort(node), std::optional<ULONG>(7));

    using WindowsDevices::usbSpeedMbps;
    EXPECT_DOUBLE_EQ(usbSpeedMbps(WindowsDevices::UsbPortRecord{}), 0.0); // nothing read
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(UsbLowSpeed)), 1.5);
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(UsbFullSpeed)), 12.0);
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(UsbHighSpeed)), 480.0);
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(UsbSuperSpeed)), 5000.0);
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(7)), 0.0);
    // EX_V2 overrides EX: a USB 3 device the older call reports as High-Speed.
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(UsbHighSpeed, true)), 5000.0);
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(std::nullopt, true)), 5000.0);
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(UsbSuperSpeed, true, true)), 10000.0); // no lane information
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(UsbSuperSpeed, true, true, LANE_10_GBPS)), 10000.0);
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(UsbSuperSpeed, true, true, LANE_10_GBPS, 1)), 20000.0);
    constexpr ULONG LANE_5_GBPS = (5UL << 16U) | (3UL << 4U);
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(std::nullopt, true, true, LANE_5_GBPS, 1)), 10000.0); // Gen 1x2
    EXPECT_DOUBLE_EQ(usbSpeedMbps(portAnswer(std::nullopt, true, true, 0)), 10000.0);              // a nonsense lane speed

    constexpr ULONG LANE_10000_MBPS = (10000UL << 16U) | (2UL << 4U);
    EXPECT_DOUBLE_EQ(WindowsDevices::superSpeedPlusMbps(LANE_10000_MBPS, 0), 10000.0);
    EXPECT_DOUBLE_EQ(WindowsDevices::superSpeedPlusMbps(LANE_10_GBPS, 1), 20000.0);
    EXPECT_DOUBLE_EQ(WindowsDevices::superSpeedPlusMbps(LANE_10_GBPS, 7), 0.0); // 80 Gbit/s: out of range
    EXPECT_DOUBLE_EQ(WindowsDevices::superSpeedPlusMbps(LANE_5_GBPS, 0), 0.0);  // 5 Gbit/s isn't SuperSpeedPlus
}

TEST(WindowsSystemInfoProbeTest, DeviceHelpers)
{
    EXPECT_EQ(WindowsDevices::problemText(CM_PROB_FAILED_INSTALL), "The drivers for this device are not installed (Code 28)");
    EXPECT_EQ(WindowsDevices::problemText(CM_PROB_UNSIGNED_DRIVER), "Windows cannot verify the digital signature of the drivers (Code 52)");
    EXPECT_EQ(WindowsDevices::problemText(1234), "Problem code 1234");
    EXPECT_EQ(WindowsDevices::idAfter("PCI\\VEN_8086&DEV_51F0", "DEV_"), 0x51F0U);
    EXPECT_EQ(WindowsDevices::idAfter("PCI\\VEN_80", "VEN_"), 0U); // too short
    EXPECT_EQ(WindowsDevices::idAfter("ACPI\\VEN_INT&DEV_33A0", "VEN_"), 0U);
    EXPECT_TRUE(WindowsDevices::isListedUsbDevice("USB\\VID_046D&PID_C52B\\5&3"));
    EXPECT_FALSE(WindowsDevices::isListedUsbDevice("USB\\ROOT_HUB30\\4&6"));
    EXPECT_FALSE(WindowsDevices::isListedUsbDevice("USB\\VID_046D&PID_C52B&MI_01\\6&5"));
}

} // namespace
} // namespace Platform
