/// @file test_WindowsSystemInfoProbe.cpp
/// @brief The real WindowsSystemInfoProbe (#1512), smoke-tested: it reports an edition and a build,
/// and the session facts any signed-in test run has; the real SMBIOS table (#1513) parses; and the
/// memory modules and installed/usable memory (#1515), real and through a faked function table; and commit,
/// page files and compressed memory (#1516), real and faked, including the NT-to-DOS path conversion.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/SmbiosParser.h"
#include "Platform/Windows/WindowsCommitPaging.h"
#include "Platform/Windows/WindowsInstalledMemory.h"
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
// clang-format on

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
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

} // namespace
} // namespace Platform
