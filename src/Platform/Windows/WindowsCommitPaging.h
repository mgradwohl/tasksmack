#pragma once

// The Commit & paging facts on Windows (#1516): the commit charge, limit and peak and the page size
// (GetPerformanceInfo); each page file's DOS path, size, use and peak
// (NtQuerySystemInformation(SystemPageFileInformationEx), falling back to SystemPageFileInformation);
// and compressed memory, the "Memory Compression" process's working set from the SystemProcessInformation
// snapshot, which needs no handle to the (protected) process and so no administrator rights. Every call
// goes through an injectable table so tests drive the failures and the buffers without the machine.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsNtQuery.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <psapi.h> // GetPerformanceInfo (K32GetPerformanceInfo, in kernel32)
// clang-format on

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

namespace Platform::WindowsCommitPaging
{

inline constexpr ULONG SYSTEM_PROCESS_INFORMATION_CLASS = 5;
inline constexpr ULONG SYSTEM_PAGE_FILE_INFORMATION_CLASS = 18;
inline constexpr ULONG SYSTEM_PAGE_FILE_INFORMATION_EX_CLASS = 144;
inline constexpr LONG STATUS_INFO_LENGTH_MISMATCH_VALUE = static_cast<LONG>(0xC0000004L);

/// The calls readCommitPaging() makes. Defaults to the real ones; tests substitute fakes.
struct Functions
{
    decltype(&GetPerformanceInfo) getPerformanceInfo = &GetPerformanceInfo;
    Windows::NtQuerySystemInformationFn ntQuerySystemInformation = Windows::ntQuerySystemInformation();
    decltype(&GetLogicalDrives) getLogicalDrives = &GetLogicalDrives;
    decltype(&QueryDosDeviceW) queryDosDevice = &QueryDosDeviceW;
};

/// A drive letter and the NT device it names ('C', "\Device\HarddiskVolume3"), from QueryDosDeviceW.
struct DriveDevice
{
    wchar_t letter = L'\0';
    std::wstring device;
};

/// A page file's NT path as a DOS path: "\??\C:\pagefile.sys" -> "C:\pagefile.sys", "\??\UNC\server\share\x"
/// -> "\\server\share\x", "\Device\HarddiskVolume3\pagefile.sys" -> "C:\pagefile.sys" through @p drives.
/// A path none of those fit is returned as given.
[[nodiscard]] inline std::wstring ntPathToDosPath(std::wstring_view ntPath, std::span<const DriveDevice> drives)
{
    for (const std::wstring_view prefix :
         {std::wstring_view{L"\\??\\"}, std::wstring_view{L"\\\\?\\"}, std::wstring_view{L"\\DosDevices\\"}})
    {
        if (ntPath.starts_with(prefix))
        {
            const std::wstring_view rest = ntPath.substr(prefix.size());
            if (rest.starts_with(L"UNC\\"))
            {
                return L"\\\\" + std::wstring(rest.substr(4));
            }
            return std::wstring(rest);
        }
    }
    for (const DriveDevice& drive : drives)
    {
        if (!drive.device.empty() && ntPath.starts_with(drive.device) &&
            (ntPath.size() == drive.device.size() || ntPath[drive.device.size()] == L'\\'))
        {
            return std::wstring{drive.letter, L':'} + std::wstring(ntPath.substr(drive.device.size()));
        }
    }
    return std::wstring(ntPath);
}

/// One SYSTEM_PAGEFILE_INFORMATION entry, sizes in pages. The _EX form appends MinimumSize and
/// MaximumSize; NextEntryOffset steps over them, so both are walked alike.
struct PageFileEntry
{
    ULONG nextEntryOffset;
    ULONG totalSize;
    ULONG totalInUse;
    ULONG peakUsage;
    UNICODE_STRING pageFileName;
};

/// A page file as the buffer gives it: the NT path and the sizes in pages.
struct RawPageFile
{
    std::wstring ntPath;
    std::uint64_t totalPages = 0;
    std::uint64_t inUsePages = 0;
    std::uint64_t peakPages = 0;
};

/// The page files in a SystemPageFileInformation(Ex) buffer. The walk stops at an entry that would run
/// past the buffer or a link that doesn't move forward; a name whose characters lie outside the buffer
/// is left empty. A malformed buffer must not make this loop or read out of bounds.
[[nodiscard]] inline std::vector<RawPageFile> parsePageFiles(std::span<const std::byte> buffer)
{
    std::vector<RawPageFile> files;
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    std::size_t offset = 0;
    while (offset + sizeof(PageFileEntry) <= buffer.size())
    {
        PageFileEntry entry{};
        std::memcpy(&entry, buffer.data() + offset, sizeof(entry));
        RawPageFile file{.ntPath = {}, .totalPages = entry.totalSize, .inUsePages = entry.totalInUse, .peakPages = entry.peakUsage};
        const auto name =
            reinterpret_cast<std::uintptr_t>(entry.pageFileName.Buffer); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        const std::size_t nameBytes = entry.pageFileName.Length;
        if (name >= base && name - base <= buffer.size() && nameBytes <= buffer.size() - (name - base))
        {
            file.ntPath.resize(nameBytes / sizeof(wchar_t));
            std::memcpy(file.ntPath.data(), buffer.data() + (name - base), file.ntPath.size() * sizeof(wchar_t));
        }
        files.push_back(std::move(file));
        if (entry.nextEntryOffset < sizeof(PageFileEntry))
        {
            break; // 0 ends the chain; anything shorter than an entry is malformed
        }
        offset += entry.nextEntryOffset;
    }
    return files;
}

/// The working set of the process named "Memory Compression" in a SystemProcessInformation buffer;
/// nullopt when there is none (compression is off) or the buffer is malformed.
[[nodiscard]] inline std::optional<std::uint64_t> memoryCompressionWorkingSet(std::span<const std::byte> buffer)
{
    constexpr std::wstring_view NAME = L"Memory Compression";
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    std::size_t offset = 0;
    while (offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= buffer.size())
    {
        SYSTEM_PROCESS_INFORMATION entry{};
        std::memcpy(&entry, buffer.data() + offset, sizeof(entry));
        const auto name = reinterpret_cast<std::uintptr_t>(entry.ImageName.Buffer); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        const std::size_t nameBytes = entry.ImageName.Length;
        if (nameBytes == NAME.size() * sizeof(wchar_t) && name >= base && name - base <= buffer.size() &&
            nameBytes <= buffer.size() - (name - base))
        {
            std::array<wchar_t, NAME.size()> text{};
            std::memcpy(text.data(), buffer.data() + (name - base), nameBytes);
            if (std::wstring_view(text.data(), text.size()) == NAME)
            {
                return static_cast<std::uint64_t>(entry.WorkingSetSize);
            }
        }
        if (entry.NextEntryOffset < sizeof(SYSTEM_PROCESS_INFORMATION))
        {
            break;
        }
        offset += entry.NextEntryOffset;
    }
    return std::nullopt;
}

/// NtQuerySystemInformation(@p infoClass) into a buffer that grows on STATUS_INFO_LENGTH_MISMATCH, trimmed
/// to the bytes written; nullopt when the call fails.
[[nodiscard]] inline std::optional<std::vector<std::byte>>
querySystemInformation(const Functions& fns, ULONG infoClass, std::size_t initialBytes)
{
    if (fns.ntQuerySystemInformation == nullptr)
    {
        return std::nullopt;
    }
    constexpr std::size_t MAX_BYTES = std::size_t{64} * 1024 * 1024;
    std::vector<std::byte> buffer(initialBytes);
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        ULONG returned = 0;
        const LONG status = fns.ntQuerySystemInformation(infoClass, buffer.data(), static_cast<ULONG>(buffer.size()), &returned);
        if (status == STATUS_INFO_LENGTH_MISMATCH_VALUE)
        {
            // The list can grow between calls: take what it asked for plus headroom, at least double.
            const std::size_t next = std::max<std::size_t>(buffer.size() * 2, static_cast<std::size_t>(returned) + (returned / 4));
            if (next > MAX_BYTES)
            {
                return std::nullopt;
            }
            buffer.resize(next);
            continue;
        }
        if (status < 0)
        {
            return std::nullopt;
        }
        buffer.resize(std::min<std::size_t>(returned, buffer.size()));
        return buffer;
    }
    return std::nullopt;
}

/// Every drive letter's NT device, for ntPathToDosPath().
[[nodiscard]] inline std::vector<DriveDevice> driveDevices(const Functions& fns)
{
    std::vector<DriveDevice> drives;
    const DWORD mask = fns.getLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter)
    {
        if ((mask & (1U << static_cast<unsigned>(letter - L'A'))) == 0)
        {
            continue;
        }
        const std::array<wchar_t, 3> name{letter, L':', L'\0'};
        std::array<wchar_t, MAX_PATH> target{};
        if (fns.queryDosDevice(name.data(), target.data(), static_cast<DWORD>(target.size())) != 0)
        {
            drives.push_back({.letter = letter, .device = std::wstring(target.data())}); // the first of the NUL-separated targets
        }
    }
    return drives;
}

/// Fills @p info's Windows facts; a call that fails leaves its facts unknown.
inline void readCommitPaging(CommitPagingInfo& info, const Functions& fns = {})
{
    info.available = true;
    info.family = OsFamily::Windows;

    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (fns.getPerformanceInfo(&performance, sizeof(performance)) != FALSE)
    {
        const auto page = static_cast<std::uint64_t>(performance.PageSize);
        info.pageSizeBytes = page;
        info.committedBytes = static_cast<std::uint64_t>(performance.CommitTotal) * page;
        info.commitLimitBytes = static_cast<std::uint64_t>(performance.CommitLimit) * page;
        info.commitPeakBytes = static_cast<std::uint64_t>(performance.CommitPeak) * page;
    }
    std::uint64_t pageSize = info.pageSizeBytes;
    if (pageSize == 0)
    {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        pageSize = system.dwPageSize;
    }

    constexpr std::size_t PAGE_FILE_BYTES = 4096;
    std::optional<std::vector<std::byte>> pageFiles = querySystemInformation(fns, SYSTEM_PAGE_FILE_INFORMATION_EX_CLASS, PAGE_FILE_BYTES);
    if (!pageFiles.has_value())
    {
        pageFiles = querySystemInformation(fns, SYSTEM_PAGE_FILE_INFORMATION_CLASS, PAGE_FILE_BYTES);
    }
    if (pageFiles.has_value())
    {
        info.pageFilesRead = true;
        std::vector<DriveDevice> drives;
        for (const RawPageFile& raw : parsePageFiles(*pageFiles))
        {
            if (drives.empty() && raw.ntPath.starts_with(L"\\Device\\"))
            {
                drives = driveDevices(fns);
            }
            info.pageFiles.push_back({
                .path = WinString::wideToUtf8(ntPathToDosPath(raw.ntPath, drives)),
                .kind = {},
                .sizeBytes = raw.totalPages * pageSize,
                .usedBytes = raw.inUsePages * pageSize,
                .peakBytes = raw.peakPages * pageSize,
                .priority = 0,
            });
        }
    }

    constexpr std::size_t PROCESS_BYTES = std::size_t{512} * 1024;
    if (const auto processes = querySystemInformation(fns, SYSTEM_PROCESS_INFORMATION_CLASS, PROCESS_BYTES); processes.has_value())
    {
        info.compressedBytes = memoryCompressionWorkingSet(*processes);
    }
}

} // namespace Platform::WindowsCommitPaging
