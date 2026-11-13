#pragma once

// Windows IProcessMemoryMapReader (#3089): VirtualQueryEx over the target's address space, one mapping per
// region it reports (free regions left out), with GetMappedFileNameW naming the file behind an image or a
// mapped view, as a drive path (WindowsDevicePaths.h). The process is opened with PROCESS_QUERY_INFORMATION,
// which both calls need, falling back to PROCESS_QUERY_LIMITED_INFORMATION where that is all the OS gives,
// and its creation time is checked against the target's, as every Process Details reader's is
// (WindowsProcessIdentity.h). Nothing is read out of the process's memory, so it needs no PROCESS_VM_READ.
//
// Windows measures none of smaps' sizes per region: a mapping's residentBytes, pssBytes and the rest stay
// absent, and the view shows the address space, committed and reserved. Nor does VirtualQueryEx tell a heap
// or a stack from other private memory: MEM_PRIVATE is Anonymous.
//
// The calls are a table of function pointers, the system's by default, so tests drive the walk, the
// failures and the cap (test_WindowsProcessMemoryMap.cpp). The region decoding is pure and inline here.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <psapi.h>   // GetMappedFileNameW (K32GetMappedFileNameW, in kernel32)
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

    OpenProcessFn openProcess = &::OpenProcess;
    CloseHandleFn closeHandle = &::CloseHandle;
    GetProcessTimesFn getProcessTimes = &::GetProcessTimes;
    GetExitCodeProcessFn getExitCodeProcess = &::GetExitCodeProcess;
    VirtualQueryExFn virtualQueryEx = &::VirtualQueryEx;
    GetMappedFileNameFn getMappedFileName = &::GetMappedFileNameW;
    GetLogicalDrivesFn getLogicalDrives = &::GetLogicalDrives;
    QueryDosDeviceFn queryDosDevice = &::QueryDosDeviceW;
};

/// Not thread-safe: one read at a time (the Memory map view runs them one at a time on a worker).
class WindowsProcessMemoryMapReader final : public IProcessMemoryMapReader
{
  public:
    WindowsProcessMemoryMapReader() = default;

    /// Test seam: make every call through @p api, and keep at most @p maxRegions regions.
    explicit WindowsProcessMemoryMapReader(const ProcessMemoryMapFunctions& api, std::size_t maxRegions = MAX_MEMORY_MAP_REGIONS)
        : m_Api(api), m_MaxRegions(maxRegions)
    {}

    [[nodiscard]] bool hasMemoryMap() const override;
    [[nodiscard]] MemoryMapReadResult readMemoryMap(const ProcessTarget& target) override;

  private:
    ProcessMemoryMapFunctions m_Api;
    std::size_t m_MaxRegions = MAX_MEMORY_MAP_REGIONS;
};

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
