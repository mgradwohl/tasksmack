#pragma once

// Windows IProcessMemoryMapReader (#3089): VirtualQueryEx over the target's address space, one mapping per
// region it reports (free regions left out), with GetMappedFileNameW naming the file behind an image or a
// mapped view, as a drive path (WindowsDevicePaths.h). The process is opened with PROCESS_QUERY_INFORMATION,
// which both calls need, falling back to PROCESS_QUERY_LIMITED_INFORMATION where that is all the OS gives,
// and its creation time is checked against the target's, as every Process Details reader's is
// (WindowsProcessIdentity.h). Nothing is read out of the process's memory, so it needs no PROCESS_VM_READ.
//
// A committed region's resident, private and shared sizes come from QueryWorkingSetEx (#4031), page by page:
// resident is a page in the working set (Valid), shared one that another process maps too (Shared, with a
// ShareCount past 1, as smaps' Shared_* counts a page mapped more than once) and private any other resident
// page. Windows doesn't record whether a page was written, so a page whose protection allows writing in place
// counts as dirty and any other (read-only, executable, copy-on-write not yet written) as clean. The query
// needs PROCESS_QUERY_INFORMATION: on the limited right the sizes stay absent. It is bounded (the
// WORKING_SET_* constants): a region past the size cap, or past the read's page budget, keeps none. PSS and
// swap stay absent: Windows has no proportional-set or per-region pagefile figure. Nor does VirtualQueryEx
// tell a heap or a stack from other private memory: MEM_PRIVATE is Anonymous.
//
// The calls are a table of function pointers, the system's by default, so tests drive the walk, the
// failures and the caps (test_WindowsProcessMemoryMap.cpp). The region and page decoding is pure and inline here.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <psapi.h>   // GetMappedFileNameW, QueryWorkingSetEx (K32*, in kernel32)
// clang-format on

#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The most regions one read keeps; past it the result is truncated. A desktop process has a few thousand,
/// a browser's or a JIT's tens of thousands; Linux's read stops at 32 MiB of smaps, about as many.
inline constexpr std::size_t MAX_MEMORY_MAP_REGIONS = 65536;

/// The page QueryWorkingSetEx is asked about: 4 KiB on every Windows TaskSmack runs on (x64, ARM64). A large
/// page answers for each 4 KiB piece of it, each Valid, so the count still comes out in bytes.
inline constexpr std::uint64_t WORKING_SET_PAGE_BYTES = 4096;

/// The most pages one QueryWorkingSetEx call asks about: a 128 KiB buffer of
/// PSAPI_WORKING_SET_EX_INFORMATION, reused across calls; a larger region takes several.
inline constexpr std::size_t WORKING_SET_PAGES_PER_QUERY = 8192;

/// A committed region larger than this (1 GiB, 262,144 pages) isn't asked about, and keeps no sizes: one
/// huge reservation committed whole would otherwise take most of a read's budget by itself.
inline constexpr std::uint64_t WORKING_SET_MAX_REGION_BYTES = std::uint64_t{1} << 30U;

/// The most pages one read asks about, over all its regions (2 Mi pages, 8 GiB of committed memory, some
/// tenths of a second on the read worker at the most); a region that doesn't fit what is left keeps no sizes.
/// A desktop process commits well under it; the read refreshes every PROCESS_MEMORY_MAP_REFRESH_MS.
inline constexpr std::uint64_t WORKING_SET_MAX_PAGES_PER_READ = std::uint64_t{1} << 21U;

/// The bounds on a read's QueryWorkingSetEx calls (the WORKING_SET_* constants, or a test's).
struct WorkingSetLimits
{
    std::size_t pagesPerQuery = WORKING_SET_PAGES_PER_QUERY;
    std::uint64_t maxRegionBytes = WORKING_SET_MAX_REGION_BYTES;
    std::uint64_t maxPagesPerRead = WORKING_SET_MAX_PAGES_PER_READ;
};

/// One region's resident pages, by whether another process maps them too and whether they may have been
/// written, in pages.
struct WorkingSetPages
{
    std::uint64_t sharedClean = 0;
    std::uint64_t sharedDirty = 0;
    std::uint64_t privateClean = 0;
    std::uint64_t privateDirty = 0;

    [[nodiscard]] constexpr std::uint64_t resident() const noexcept
    {
        return sharedClean + sharedDirty + privateClean + privateDirty;
    }

    friend bool operator==(const WorkingSetPages&, const WorkingSetPages&) = default;
};

/// What a region's page protection allows (MEMORY_BASIC_INFORMATION::Protect), decoded.
struct RegionProtection
{
    bool readable = false;
    bool writable = false;
    bool executable = false;
    bool copyOnWrite = false; ///< PAGE_WRITECOPY or PAGE_EXECUTE_WRITECOPY: writable, but a write makes a private copy.
    bool guard = false;       ///< PAGE_GUARD: the first touch raises a guard-page exception (a thread stack's growth).

    friend bool operator==(const RegionProtection&, const RegionProtection&) = default;
};

/// @p protect decoded: its access in the low byte (PAGE_NOACCESS ... PAGE_EXECUTE_WRITECOPY) and PAGE_GUARD.
/// The other modifiers (PAGE_NOCACHE, PAGE_WRITECOMBINE, PAGE_TARGETS_*) change nothing shown. 0 (a reserved
/// region's, whose pages have no protection yet) and PAGE_NOACCESS allow nothing.
[[nodiscard]] constexpr RegionProtection decodeProtection(DWORD protect) noexcept
{
    RegionProtection decoded{.guard = (protect & PAGE_GUARD) != 0};
    switch (protect & 0xFFU)
    {
    case PAGE_READONLY:
        decoded.readable = true;
        break;
    case PAGE_READWRITE:
        decoded.readable = true;
        decoded.writable = true;
        break;
    case PAGE_WRITECOPY:
        decoded.readable = true;
        decoded.writable = true;
        decoded.copyOnWrite = true;
        break;
    case PAGE_EXECUTE:
        decoded.executable = true;
        break;
    case PAGE_EXECUTE_READ:
        decoded.readable = true;
        decoded.executable = true;
        break;
    case PAGE_EXECUTE_READWRITE:
        decoded.readable = true;
        decoded.writable = true;
        decoded.executable = true;
        break;
    case PAGE_EXECUTE_WRITECOPY:
        decoded.readable = true;
        decoded.writable = true;
        decoded.executable = true;
        decoded.copyOnWrite = true;
        break;
    default: // PAGE_NOACCESS, or none (reserved)
        break;
    }
    return decoded;
}

/// @p page, one QueryWorkingSetEx answer, added to @p pages: nothing unless it is resident (Valid). It is shared
/// when another process maps it too (Shared and a ShareCount past 1; a shareable page only this process maps
/// is private, as in smaps), and dirty when its protection allows writing in place (copy-on-write doesn't:
/// a written copy-on-write page becomes read/write and private).
inline void countPage(WorkingSetPages& pages, const PSAPI_WORKING_SET_EX_BLOCK& page) noexcept
{
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - the block is a union of the valid and invalid layouts
    if (page.Valid == 0)
    {
        return;
    }
    const RegionProtection protection = decodeProtection(static_cast<DWORD>(page.Win32Protection));
    const bool shared = page.Shared != 0 && page.ShareCount > 1;
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    const bool dirty = protection.writable && !protection.copyOnWrite;
    if (shared)
    {
        ++(dirty ? pages.sharedDirty : pages.sharedClean);
    }
    else
    {
        ++(dirty ? pages.privateDirty : pages.privateClean);
    }
}

/// @p mapping with the sizes of its resident @p pages, in bytes.
constexpr void setWorkingSetSizes(MemoryMapping& mapping, const WorkingSetPages& pages) noexcept
{
    mapping.residentBytes = pages.resident() * WORKING_SET_PAGE_BYTES;
    mapping.sharedCleanBytes = pages.sharedClean * WORKING_SET_PAGE_BYTES;
    mapping.sharedDirtyBytes = pages.sharedDirty * WORKING_SET_PAGE_BYTES;
    mapping.privateCleanBytes = pages.privateClean * WORKING_SET_PAGE_BYTES;
    mapping.privateDirtyBytes = pages.privateDirty * WORKING_SET_PAGE_BYTES;
}

/// A region's kind from its MEMORY_BASIC_INFORMATION::Type: MEM_IMAGE is Image, MEM_MAPPED a File when
/// GetMappedFileNameW named one (@p hasFile) and Other when it didn't (a pagefile-backed section), and
/// MEM_PRIVATE, or a type Windows adds later, Anonymous.
[[nodiscard]] constexpr MemoryMappingKind regionKind(DWORD type, bool hasFile) noexcept
{
    switch (type)
    {
    case MEM_IMAGE:
        return MemoryMappingKind::Image;
    case MEM_MAPPED:
        return hasFile ? MemoryMappingKind::File : MemoryMappingKind::Other;
    default:
        return MemoryMappingKind::Anonymous;
    }
}

/// Whether a region of @p type can be backed by a file, so GetMappedFileNameW is asked to name it.
[[nodiscard]] constexpr bool regionMayHaveFile(DWORD type) noexcept
{
    return type == MEM_IMAGE || type == MEM_MAPPED;
}

/// One non-free region as a MemoryMapping, @p path its file's drive path (UTF-8; empty when it has none).
/// A reserved region is address space only: it allows nothing, whatever AllocationProtect it was reserved
/// with. A section-backed region (an image or a view) is shared unless its protection is copy-on-write.
[[nodiscard]] inline MemoryMapping toMapping(const MEMORY_BASIC_INFORMATION& info, std::string path)
{
    const bool reserved = info.State == MEM_RESERVE;
    const RegionProtection protection = reserved ? RegionProtection{} : decodeProtection(info.Protect);
    const auto start = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info.BaseAddress));
    const auto size = static_cast<std::uint64_t>(info.RegionSize);
    const MemoryMappingKind kind = regionKind(info.Type, !path.empty());
    return {
        .start = start,
        .end = size > std::numeric_limits<std::uint64_t>::max() - start ? std::numeric_limits<std::uint64_t>::max() : start + size,
        .readable = protection.readable,
        .writable = protection.writable,
        .executable = protection.executable,
        .shared = regionMayHaveFile(info.Type) && !protection.copyOnWrite,
        .guard = protection.guard,
        .reserved = reserved,
        .kind = kind,
        .path = (kind == MemoryMappingKind::Image || kind == MemoryMappingKind::File) ? std::move(path) : std::string{},
    };
}

/// The calls WindowsProcessMemoryMapReader makes: kernel32's, or a test's fakes.
struct ProcessMemoryMapFunctions
{
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
    using GetProcessTimesFn = BOOL(WINAPI*)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    using GetExitCodeProcessFn = BOOL(WINAPI*)(HANDLE, LPDWORD);
    using VirtualQueryExFn = SIZE_T(WINAPI*)(HANDLE, LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
    using GetMappedFileNameFn = DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD);
    using GetLogicalDrivesFn = DWORD(WINAPI*)();
    using QueryDosDeviceFn = DWORD(WINAPI*)(LPCWSTR, LPWSTR, DWORD);
    using QueryWorkingSetExFn = BOOL(WINAPI*)(HANDLE, PVOID, DWORD);

    OpenProcessFn openProcess = &::OpenProcess;
    CloseHandleFn closeHandle = &::CloseHandle;
    GetProcessTimesFn getProcessTimes = &::GetProcessTimes;
    GetExitCodeProcessFn getExitCodeProcess = &::GetExitCodeProcess;
    VirtualQueryExFn virtualQueryEx = &::VirtualQueryEx;
    GetMappedFileNameFn getMappedFileName = &::GetMappedFileNameW;
    GetLogicalDrivesFn getLogicalDrives = &::GetLogicalDrives;
    QueryDosDeviceFn queryDosDevice = &::QueryDosDeviceW;
    QueryWorkingSetExFn queryWorkingSetEx = &::QueryWorkingSetEx; // K32QueryWorkingSetEx, in kernel32
};

/// Not thread-safe: one read at a time (the Memory map view runs them one at a time on a worker).
class WindowsProcessMemoryMapReader final : public IProcessMemoryMapReader
{
  public:
    WindowsProcessMemoryMapReader() = default;

    /// Test seam: make every call through @p api, keep at most @p maxRegions regions, and bound the working-set
    /// queries by @p limits.
    explicit WindowsProcessMemoryMapReader(const ProcessMemoryMapFunctions& api,
                                           std::size_t maxRegions = MAX_MEMORY_MAP_REGIONS,
                                           const WorkingSetLimits& limits = {})
        : m_Api(api), m_MaxRegions(maxRegions), m_Limits(limits)
    {}

    [[nodiscard]] bool hasMemoryMap() const override;
    [[nodiscard]] MemoryMapReadResult readMemoryMap(const ProcessTarget& target) override;

  private:
    ProcessMemoryMapFunctions m_Api;
    std::size_t m_MaxRegions = MAX_MEMORY_MAP_REGIONS;
    WorkingSetLimits m_Limits;
};

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
