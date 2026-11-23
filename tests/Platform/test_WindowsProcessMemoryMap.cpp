/// @file test_WindowsProcessMemoryMap.cpp
/// @brief Platform::Windows::WindowsProcessMemoryMapReader (#3089): the pure decoding (page protection,
/// region kind, a region as a MemoryMapping), the NT device paths as drive paths, and the walk against
/// fake kernel32 calls: free regions left out, a mapped file named once per allocation, the limited-right
/// fallback, a denied open, a reused PID, a query that fails or finds the process gone, and the cap; and each
/// committed region's resident, shared and private pages from QueryWorkingSetEx (#4031): the counting, the
/// batches, the caps, the limited right and a refused query. Plus a read-only walk of this test process
/// through the real calls.

#include "Domain/ProcessMemoryMap.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/Windows/WindowsDevicePaths.h"
#include "Platform/Windows/WindowsProcessMemoryMap.h"
#include "Support/WindowsSandbox.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{
namespace
{

constexpr std::uint64_t START_TICKS = 0x01DB'0000'1234'5678ULL;
constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = START_TICKS};

/// The top of the fake address space: a query at or past it fails as past the last region.
constexpr std::uintptr_t ADDRESS_TOP = 0x7FFF'FFFF'0000ULL;

struct FakeRegion
{
    std::uintptr_t base = 0;
    std::uintptr_t allocationBase = 0;
    SIZE_T size = 0;
    DWORD state = MEM_FREE;
    DWORD type = 0;
    DWORD protect = 0;
};

/// The answers the fakes give, set per test.
struct FakeProcess
{
    DWORD openError = 0;          // non-zero: every OpenProcess fails with it
    bool refuseFullQuery = false; // OpenProcess refuses PROCESS_QUERY_INFORMATION (a protected process)
    std::uint64_t creationTicks = START_TICKS;
    DWORD exitCode = STILL_ACTIVE;
    DWORD queryError = 0;                         // non-zero: every VirtualQueryEx fails with it
    int exitAfterQueries = -1;                    // >= 0: the process exits after this many queries
    std::vector<FakeRegion> regions;              // in address order, contiguous from 0 to ADDRESS_TOP
    std::map<std::uintptr_t, std::wstring> files; // allocation base -> NT path
    std::map<std::uintptr_t, ULONG_PTR> pages;    // page address -> its working-set block's Flags (absent: not resident)
    DWORD workingSetError = 0;                    // non-zero: every QueryWorkingSetEx fails with it
    std::vector<std::size_t> workingSetBatches;   // the pages each QueryWorkingSetEx call asked about
    std::vector<DWORD> openAccess;
    int queries = 0;
    int nameCalls = 0;
    int opens = 0;
    int closes = 0;
};

FakeProcess& fake()
{
    static FakeProcess instance;
    return instance;
}

HANDLE fakeProcessHandle()
{
    static int token = 0;
    return &token;
}

HANDLE WINAPI fakeOpenProcess(DWORD access, BOOL /*inherit*/, DWORD pid)
{
    EXPECT_EQ(pid, static_cast<DWORD>(TARGET.pid));
    EXPECT_EQ(access & PROCESS_VM_READ, 0U); // nothing is read out of the process's memory
    fake().openAccess.push_back(access);
    if (fake().openError != 0 || (fake().refuseFullQuery && (access & PROCESS_QUERY_INFORMATION) != 0))
    {
        SetLastError(fake().openError != 0 ? fake().openError : ERROR_ACCESS_DENIED);
        return nullptr;
    }
    ++fake().opens;
    return fakeProcessHandle();
}

BOOL WINAPI fakeCloseHandle(HANDLE handle)
{
    EXPECT_EQ(handle, fakeProcessHandle());
    ++fake().closes;
    return TRUE;
}

BOOL WINAPI fakeGetProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
{
    creation->dwLowDateTime = static_cast<DWORD>(fake().creationTicks & 0xFFFFFFFFU);
    creation->dwHighDateTime = static_cast<DWORD>(fake().creationTicks >> 32U);
    return TRUE;
}

BOOL WINAPI fakeGetExitCodeProcess(HANDLE /*process*/, LPDWORD code)
{
    const bool exited = fake().exitAfterQueries >= 0 && fake().queries > fake().exitAfterQueries;
    *code = exited ? 0 : fake().exitCode;
    return TRUE;
}

SIZE_T WINAPI fakeVirtualQueryEx(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T bytes)
{
    EXPECT_EQ(process, fakeProcessHandle());
    EXPECT_EQ(bytes, sizeof(MEMORY_BASIC_INFORMATION));
    ++fake().queries;
    if (fake().queryError != 0)
    {
        SetLastError(fake().queryError);
        return 0;
    }
    if (fake().exitAfterQueries >= 0 && fake().queries > fake().exitAfterQueries)
    {
        SetLastError(ERROR_PARTIAL_COPY); // the address space is being torn down
        return 0;
    }
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    for (const FakeRegion& region : fake().regions)
    {
        if (at >= region.base && at - region.base < region.size) // no overflow for a region that ends at the top
        {
            *info = {};
            info->BaseAddress = reinterpret_cast<PVOID>(region.base);              // NOLINT(performance-no-int-to-ptr) - a fake address
            info->AllocationBase = reinterpret_cast<PVOID>(region.allocationBase); // NOLINT(performance-no-int-to-ptr) - a fake address
            info->RegionSize = region.size;
            info->State = region.state;
            info->Type = region.type;
            info->Protect = region.protect;
            return sizeof(MEMORY_BASIC_INFORMATION);
        }
    }
    SetLastError(ERROR_INVALID_PARAMETER); // past the top of the address space
    return 0;
}

DWORD WINAPI fakeGetMappedFileName(HANDLE /*process*/, LPVOID address, LPWSTR buffer, DWORD size)
{
    ++fake().nameCalls;
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    for (const FakeRegion& region : fake().regions)
    {
        if (at >= region.base && at - region.base < region.size) // no overflow for a region that ends at the top
        {
            const auto file = fake().files.find(region.allocationBase);
            if (file == fake().files.end() || file->second.size() >= size)
            {
                SetLastError(ERROR_FILE_INVALID);
                return 0;
            }
            std::ranges::copy(file->second, buffer);
            buffer[file->second.size()] = L'\0';
            return static_cast<DWORD>(file->second.size());
        }
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
}

DWORD WINAPI fakeGetLogicalDrives()
{
    return (1U << 2U) | (1U << 3U); // C: and D:
}

DWORD WINAPI fakeQueryDosDevice(LPCWSTR name, LPWSTR target, DWORD size)
{
    const std::wstring_view device = (name[0] == L'C') ? L"\\Device\\HarddiskVolume3" : L"\\Device\\HarddiskVolume1";
    if (device.size() + 2 > size)
    {
        return 0;
    }
    std::ranges::copy(device, target);
    target[device.size()] = L'\0';
    target[device.size() + 1] = L'\0';
    return static_cast<DWORD>(device.size() + 2);
}

BOOL WINAPI fakeQueryWorkingSetEx(HANDLE process, PVOID buffer, DWORD bytes)
{
    EXPECT_EQ(process, fakeProcessHandle());
    EXPECT_EQ(bytes % sizeof(PSAPI_WORKING_SET_EX_INFORMATION), 0U);
    const std::size_t count = bytes / sizeof(PSAPI_WORKING_SET_EX_INFORMATION);
    fake().workingSetBatches.push_back(count);
    if (fake().workingSetError != 0)
    {
        SetLastError(fake().workingSetError);
        return FALSE;
    }
    for (PSAPI_WORKING_SET_EX_INFORMATION& entry : std::span(static_cast<PSAPI_WORKING_SET_EX_INFORMATION*>(buffer), count))
    {
        const auto page = fake().pages.find(reinterpret_cast<std::uintptr_t>(entry.VirtualAddress));
        entry.VirtualAttributes.Flags = page != fake().pages.end() ? page->second : 0;
    }
    return TRUE;
}

/// A working-set block: a resident page (or not), its share count, protection and whether it's shareable.
PSAPI_WORKING_SET_EX_BLOCK pageBlock(bool valid, unsigned shareCount, DWORD protection, bool shareable)
{
    PSAPI_WORKING_SET_EX_BLOCK block{};
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - the valid layout of the block's union
    block.Valid = valid ? 1U : 0U;
    block.ShareCount = shareCount;
    block.Win32Protection = protection;
    block.Shared = shareable ? 1U : 0U;
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    return block;
}

/// pageBlock()'s Flags, as the fake answers them.
ULONG_PTR pageFlags(bool valid, unsigned shareCount, DWORD protection, bool shareable)
{
    return pageBlock(valid, shareCount, protection, shareable).Flags;
}

/// Marks @p count pages from @p first resident, with @p flags.
void residentPages(std::uintptr_t first, std::size_t count, ULONG_PTR flags)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        fake().pages[first + (i * WORKING_SET_PAGE_BYTES)] = flags;
    }
}

ProcessMemoryMapFunctions fakeApi()
{
    return {
        .openProcess = &fakeOpenProcess,
        .closeHandle = &fakeCloseHandle,
        .getProcessTimes = &fakeGetProcessTimes,
        .getExitCodeProcess = &fakeGetExitCodeProcess,
        .virtualQueryEx = &fakeVirtualQueryEx,
        .getMappedFileName = &fakeGetMappedFileName,
        .getLogicalDrives = &fakeGetLogicalDrives,
        .queryDosDevice = &fakeQueryDosDevice,
        .queryWorkingSetEx = &fakeQueryWorkingSetEx,
    };
}

constexpr std::uintptr_t HEAP = 0x0000'0200'0000ULL;
constexpr std::uintptr_t VIEW = 0x0000'0300'0000ULL;
constexpr std::uintptr_t SECTION = 0x0000'0400'0000ULL;
constexpr std::uintptr_t IMAGE = 0x7FF8'A1B2'0000ULL;

/// A small address space: free, a private heap (committed then reserved), a mapped file view, a
/// pagefile-backed section, free, an image's header, code and data, free to the top.
std::vector<FakeRegion> sampleRegions()
{
    return {
        {.base = 0, .allocationBase = 0, .size = HEAP, .state = MEM_FREE},
        {.base = HEAP, .allocationBase = HEAP, .size = 0x10000, .state = MEM_COMMIT, .type = MEM_PRIVATE, .protect = PAGE_READWRITE},
        {.base = HEAP + 0x10000, .allocationBase = HEAP, .size = 0xF0000, .state = MEM_RESERVE, .type = MEM_PRIVATE, .protect = 0},
        {.base = HEAP + 0x100000, .allocationBase = 0, .size = VIEW - HEAP - 0x100000, .state = MEM_FREE},
        {.base = VIEW, .allocationBase = VIEW, .size = 0x4000, .state = MEM_COMMIT, .type = MEM_MAPPED, .protect = PAGE_READONLY},
        {.base = VIEW + 0x4000, .allocationBase = 0, .size = SECTION - VIEW - 0x4000, .state = MEM_FREE},
        {.base = SECTION, .allocationBase = SECTION, .size = 0x2000, .state = MEM_COMMIT, .type = MEM_MAPPED, .protect = PAGE_READWRITE},
        {.base = SECTION + 0x2000, .allocationBase = 0, .size = IMAGE - SECTION - 0x2000, .state = MEM_FREE},
        {.base = IMAGE, .allocationBase = IMAGE, .size = 0x1000, .state = MEM_COMMIT, .type = MEM_IMAGE, .protect = PAGE_READONLY},
        {
            .base = IMAGE + 0x1000,
            .allocationBase = IMAGE,
            .size = 0x11000,
            .state = MEM_COMMIT,
            .type = MEM_IMAGE,
            .protect = PAGE_EXECUTE_READ,
        },
        {
            .base = IMAGE + 0x12000,
            .allocationBase = IMAGE,
            .size = 0x2000,
            .state = MEM_COMMIT,
            .type = MEM_IMAGE,
            .protect = PAGE_WRITECOPY,
        },
        {.base = IMAGE + 0x14000, .allocationBase = 0, .size = ADDRESS_TOP - IMAGE - 0x14000, .state = MEM_FREE},
    };
}

class WindowsProcessMemoryMapTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = {};
        fake().regions = sampleRegions();
        fake().files = {
            {VIEW, L"\\Device\\HarddiskVolume1\\Data\\cache.bin"},
            {IMAGE, L"\\Device\\HarddiskVolume3\\Windows\\System32\\ntdll.dll"},
        };
    }

    void TearDown() override
    {
        EXPECT_EQ(fake().opens, fake().closes); // every handle opened was closed
    }
};

// --- The pure decoding ---

TEST(WindowsMemoryMapDecodingTest, DecodesEveryPageProtection)
{
    EXPECT_EQ(decodeProtection(0), RegionProtection{});
    EXPECT_EQ(decodeProtection(PAGE_NOACCESS), RegionProtection{});
    EXPECT_EQ(decodeProtection(PAGE_READONLY), (RegionProtection{.readable = true}));
    EXPECT_EQ(decodeProtection(PAGE_READWRITE), (RegionProtection{.readable = true, .writable = true}));
    EXPECT_EQ(decodeProtection(PAGE_WRITECOPY), (RegionProtection{.readable = true, .writable = true, .copyOnWrite = true}));
    EXPECT_EQ(decodeProtection(PAGE_EXECUTE), (RegionProtection{.executable = true}));
    EXPECT_EQ(decodeProtection(PAGE_EXECUTE_READ), (RegionProtection{.readable = true, .executable = true}));
    EXPECT_EQ(decodeProtection(PAGE_EXECUTE_READWRITE), (RegionProtection{.readable = true, .writable = true, .executable = true}));
    EXPECT_EQ(decodeProtection(PAGE_EXECUTE_WRITECOPY),
              (RegionProtection{.readable = true, .writable = true, .executable = true, .copyOnWrite = true}));
    // The modifiers: a guard page is marked; caching and CFG target bits change nothing shown.
    EXPECT_EQ(decodeProtection(PAGE_READWRITE | PAGE_GUARD), (RegionProtection{.readable = true, .writable = true, .guard = true}));
    EXPECT_EQ(decodeProtection(PAGE_READWRITE | PAGE_NOCACHE), (RegionProtection{.readable = true, .writable = true}));
    EXPECT_EQ(decodeProtection(PAGE_EXECUTE_READ | PAGE_TARGETS_INVALID), (RegionProtection{.readable = true, .executable = true}));
}

TEST(WindowsMemoryMapDecodingTest, MapsEachTypeToAKind)
{
    EXPECT_EQ(regionKind(MEM_IMAGE, true), MemoryMappingKind::Image);
    EXPECT_EQ(regionKind(MEM_IMAGE, false), MemoryMappingKind::Image);
    EXPECT_EQ(regionKind(MEM_MAPPED, true), MemoryMappingKind::File);
    EXPECT_EQ(regionKind(MEM_MAPPED, false), MemoryMappingKind::Other); // a pagefile-backed section
    EXPECT_EQ(regionKind(MEM_PRIVATE, false), MemoryMappingKind::Anonymous);
    EXPECT_EQ(regionKind(0x80000, false), MemoryMappingKind::Anonymous); // a type Windows adds later
    EXPECT_TRUE(regionMayHaveFile(MEM_IMAGE));
    EXPECT_TRUE(regionMayHaveFile(MEM_MAPPED));
    EXPECT_FALSE(regionMayHaveFile(MEM_PRIVATE));
}

TEST(WindowsMemoryMapDecodingTest, ARegionAsAMapping)
{
    MEMORY_BASIC_INFORMATION info{};
    info.BaseAddress = reinterpret_cast<PVOID>(IMAGE + 0x1000); // NOLINT(performance-no-int-to-ptr) - a fake address
    info.RegionSize = 0x11000;
    info.State = MEM_COMMIT;
    info.Type = MEM_IMAGE;
    info.Protect = PAGE_EXECUTE_READ;
    const MemoryMapping code = toMapping(info, R"(C:\Windows\System32\ntdll.dll)");
    EXPECT_EQ(code.start, IMAGE + 0x1000);
    EXPECT_EQ(code.end, IMAGE + 0x12000);
    EXPECT_EQ(code.sizeBytes(), 0x11000U);
    EXPECT_TRUE(code.readable);
    EXPECT_FALSE(code.writable);
    EXPECT_TRUE(code.executable);
    EXPECT_TRUE(code.shared); // section-backed
    EXPECT_FALSE(code.reserved);
    EXPECT_EQ(code.kind, MemoryMappingKind::Image);
    EXPECT_EQ(code.path, "C:\\Windows\\System32\\ntdll.dll");
    EXPECT_EQ(code.fileOffset, 0U);
    EXPECT_FALSE(code.residentBytes.has_value()); // the read adds the working-set sizes, not the decoding
    EXPECT_FALSE(code.pssBytes.has_value());

    info.Protect = PAGE_WRITECOPY;
    const MemoryMapping data = toMapping(info, R"(C:\Windows\System32\ntdll.dll)");
    EXPECT_TRUE(data.writable);
    EXPECT_FALSE(data.shared); // copy-on-write: a write makes it private

    // Reserved: address space only, whatever it was reserved with.
    info.State = MEM_RESERVE;
    info.Type = MEM_PRIVATE;
    info.Protect = PAGE_READWRITE;
    const MemoryMapping reserved = toMapping(info, {});
    EXPECT_TRUE(reserved.reserved);
    EXPECT_FALSE(reserved.readable || reserved.writable || reserved.executable || reserved.shared);
    EXPECT_EQ(reserved.kind, MemoryMappingKind::Anonymous);

    // A guard page, as at the edge of a thread's stack.
    info.State = MEM_COMMIT;
    info.Protect = PAGE_READWRITE | PAGE_GUARD;
    EXPECT_TRUE(toMapping(info, {}).guard);

    // A view with no file keeps no path, even one passed in.
    info.Type = MEM_MAPPED;
    info.Protect = PAGE_READWRITE;
    EXPECT_EQ(toMapping(info, {}).kind, MemoryMappingKind::Other);
    EXPECT_TRUE(toMapping(info, {}).shared);
}

TEST(WindowsMemoryMapDecodingTest, CountsResidentPagesBySharingAndWriting)
{
    WorkingSetPages pages;
    countPage(pages, pageBlock(false, 0, 0, false)); // not resident: nothing
    countPage(pages, pageBlock(false, 0, 0, true));
    EXPECT_EQ(pages, WorkingSetPages{});

    countPage(pages, pageBlock(true, 0, PAGE_READWRITE, false));         // private memory, writable
    countPage(pages, pageBlock(true, 0, PAGE_EXECUTE_READWRITE, false)); // JIT code
    countPage(pages, pageBlock(true, 1, PAGE_EXECUTE_READ, true));       // a DLL's code only this process maps
    countPage(pages, pageBlock(true, 1, PAGE_WRITECOPY, true));          // its copy-on-write data, not yet written
    countPage(pages, pageBlock(true, 4, PAGE_EXECUTE_READ, true));       // ntdll's code, in every process
    countPage(pages, pageBlock(true, 7, PAGE_READONLY | PAGE_GUARD, true));
    countPage(pages, pageBlock(true, 2, PAGE_READWRITE, true));  // a shared section, writable
    countPage(pages, pageBlock(true, 3, PAGE_READWRITE, false)); // a share count without Shared: private
    EXPECT_EQ(pages, (WorkingSetPages{.sharedClean = 2, .sharedDirty = 1, .privateClean = 2, .privateDirty = 3}));
    EXPECT_EQ(pages.resident(), 8U);

    MemoryMapping mapping;
    setWorkingSetSizes(mapping, pages);
    EXPECT_EQ(mapping.residentBytes, 8 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(mapping.sharedCleanBytes, 2 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(mapping.sharedDirtyBytes, 1 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(mapping.privateCleanBytes, 2 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(mapping.privateDirtyBytes, 3 * WORKING_SET_PAGE_BYTES);
    EXPECT_FALSE(mapping.pssBytes.has_value()); // Windows has no proportional-set or per-region swap figure
    EXPECT_FALSE(mapping.swapBytes.has_value());
    EXPECT_FALSE(mapping.swapPssBytes.has_value());
}

TEST(WindowsMemoryMapDecodingTest, ProtectionInWindowsWordsAndMapsLetters)
{
    MEMORY_BASIC_INFORMATION info{};
    info.RegionSize = 0x1000;
    info.State = MEM_COMMIT;
    info.Type = MEM_IMAGE;
    const auto words = [&info](DWORD protect)
    {
        info.Protect = protect;
        return Domain::MemoryMap::protectionWords(toMapping(info, "C:\\x.dll"));
    };
    EXPECT_EQ(words(PAGE_READONLY), "Read");
    EXPECT_EQ(words(PAGE_EXECUTE_READ), "Execute/Read");
    EXPECT_EQ(words(PAGE_WRITECOPY), "Copy on write");
    EXPECT_EQ(words(PAGE_EXECUTE_WRITECOPY), "Execute/Copy on write");
    EXPECT_EQ(words(PAGE_NOACCESS), "No access");
    info.Type = MEM_PRIVATE;
    EXPECT_EQ(words(PAGE_READWRITE), "Read/Write");
    EXPECT_EQ(words(PAGE_EXECUTE_READWRITE), "Execute/Read/Write");
    EXPECT_EQ(words(PAGE_READWRITE | PAGE_GUARD), "Read/Write + Guard");
    info.State = MEM_RESERVE;
    EXPECT_EQ(words(0), "Reserved");

    info.State = MEM_COMMIT;
    info.Type = MEM_IMAGE;
    info.Protect = PAGE_EXECUTE_READ;
    EXPECT_EQ(Domain::MemoryMap::protectionText(toMapping(info, "C:\\x.dll")), "r-xs");
    info.Type = MEM_PRIVATE;
    info.Protect = PAGE_READWRITE | PAGE_GUARD;
    EXPECT_EQ(Domain::MemoryMap::protectionText(toMapping(info, {})), "rw-pg");
}

TEST(WindowsDevicePathsTest, DevicePathsBecomeDrivePaths)
{
    using WindowsDevicePaths::ntPathToDosPath;
    const std::vector<WindowsDevicePaths::DriveDevice> drives{
        {.letter = L'C', .device = L"\\Device\\HarddiskVolume3"},
        {.letter = L'D', .device = L"\\Device\\HarddiskVolume1"},
    };
    EXPECT_EQ(ntPathToDosPath(L"\\Device\\HarddiskVolume3\\Windows\\System32\\ntdll.dll", drives), L"C:\\Windows\\System32\\ntdll.dll");
    EXPECT_EQ(ntPathToDosPath(L"\\Device\\HarddiskVolume1\\Data\\cache.bin", drives), L"D:\\Data\\cache.bin");
    // HarddiskVolume3 is not a prefix of HarddiskVolume31: a whole component must match.
    EXPECT_EQ(ntPathToDosPath(L"\\Device\\HarddiskVolume31\\x.dll", drives), L"\\Device\\HarddiskVolume31\\x.dll");
    EXPECT_EQ(ntPathToDosPath(L"\\Device\\Mup\\server\\share\\app.dll", drives), L"\\\\server\\share\\app.dll");
    EXPECT_EQ(ntPathToDosPath(L"\\??\\C:\\x.dll", drives), L"C:\\x.dll");
}

TEST(WindowsDevicePathsTest, ReadsEachDriveLettersDevice)
{
    const std::vector<WindowsDevicePaths::DriveDevice> drives =
        WindowsDevicePaths::driveDevices(&fakeGetLogicalDrives, &fakeQueryDosDevice);
    ASSERT_EQ(drives.size(), 2U);
    EXPECT_EQ(drives[0].letter, L'C');
    EXPECT_EQ(drives[0].device, L"\\Device\\HarddiskVolume3"); // the first of the NUL-separated targets
    EXPECT_EQ(drives[1].letter, L'D');
}

// --- The walk, through fakes ---

TEST_F(WindowsProcessMemoryMapTest, ListsEveryRegionButTheFreeOnes)
{
    WindowsProcessMemoryMapReader reader(fakeApi());
    EXPECT_TRUE(reader.hasMemoryMap());
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    EXPECT_FALSE(result.truncated);
    ASSERT_EQ(result.mappings.size(), 7U);
    EXPECT_TRUE(std::ranges::is_sorted(result.mappings, {}, &MemoryMapping::start)); // in address order

    const MemoryMapping& heap = result.mappings[0];
    EXPECT_EQ(heap.start, HEAP);
    EXPECT_EQ(heap.end, HEAP + 0x10000);
    EXPECT_EQ(heap.kind, MemoryMappingKind::Anonymous);
    EXPECT_TRUE(heap.path.empty());
    EXPECT_TRUE(result.mappings[1].reserved);

    const MemoryMapping& view = result.mappings[2];
    EXPECT_EQ(view.kind, MemoryMappingKind::File);
    EXPECT_EQ(view.path, "D:\\Data\\cache.bin");
    EXPECT_TRUE(view.shared);

    EXPECT_EQ(result.mappings[3].kind, MemoryMappingKind::Other); // the pagefile-backed section
    EXPECT_TRUE(result.mappings[3].path.empty());

    for (std::size_t i = 4; i < 7; ++i)
    {
        EXPECT_EQ(result.mappings[i].kind, MemoryMappingKind::Image);
        EXPECT_EQ(result.mappings[i].path, "C:\\Windows\\System32\\ntdll.dll");
    }
    EXPECT_TRUE(result.mappings[5].executable);
    EXPECT_FALSE(result.mappings[6].shared); // its copy-on-write data
    // One name per allocation: the view, the section (which has none) and the image's three regions once.
    EXPECT_EQ(fake().nameCalls, 3);
    // Asked for the documented right first, and given it.
    ASSERT_EQ(fake().openAccess.size(), 1U);
    EXPECT_NE(fake().openAccess[0] & PROCESS_QUERY_INFORMATION, 0U);
}

TEST_F(WindowsProcessMemoryMapTest, TheTotalsByKindAndFileFromWindowsRegions)
{
    WindowsProcessMemoryMapReader reader(fakeApi());
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    const Domain::MemoryMap::Summary summary = Domain::MemoryMap::summarize(result.mappings);
    EXPECT_EQ(summary.all.mappings, 7U);
    EXPECT_EQ(summary.all.reservedBytes, 0xF0000U);
    EXPECT_EQ(summary.all.sizeBytes, 0x10000U + 0x4000U + 0x2000U + 0x14000U);
    // Measured, though none of the fakes' pages is resident: "0 B", where the limited right leaves them absent.
    EXPECT_EQ(summary.all.residentBytes, 0U);
    EXPECT_EQ(summary.all.privateBytes, 0U);
    EXPECT_FALSE(summary.all.pssBytes.has_value());
    EXPECT_FALSE(summary.all.swapBytes.has_value());
    ASSERT_EQ(summary.byKind.size(), 4U);
    EXPECT_EQ(summary.byKind[0].kind, MemoryMappingKind::Image); // largest first: 0x14000 committed
    EXPECT_EQ(summary.byKind[0].totals.sizeBytes, 0x14000U);
    EXPECT_EQ(summary.byKind[1].kind, MemoryMappingKind::Anonymous);
    EXPECT_EQ(summary.byKind[1].totals.reservedBytes, 0xF0000U);
    ASSERT_EQ(summary.byFile.size(), 2U);
    EXPECT_EQ(summary.byFile[0].path, "C:\\Windows\\System32\\ntdll.dll");
    EXPECT_EQ(summary.byFile[0].totals.mappings, 3U);
    EXPECT_EQ(summary.byFile[1].path, "D:\\Data\\cache.bin");
}

TEST_F(WindowsProcessMemoryMapTest, FallsBackToTheLimitedRight)
{
    fake().refuseFullQuery = true;
    WindowsProcessMemoryMapReader reader(fakeApi());
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    EXPECT_EQ(result.status, MemoryMapReadStatus::Ok);
    ASSERT_EQ(fake().openAccess.size(), 2U);
    EXPECT_EQ(fake().openAccess[1], static_cast<DWORD>(PROCESS_QUERY_LIMITED_INFORMATION));

    // And where the limited right isn't enough for the query, it's denied.
    fake().queryError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(reader.readMemoryMap(TARGET).status, MemoryMapReadStatus::PermissionDenied);
}

TEST_F(WindowsProcessMemoryMapTest, DeniedOpenIsPermissionDeniedAndAGonePidExited)
{
    fake().openError = ERROR_ACCESS_DENIED;
    WindowsProcessMemoryMapReader reader(fakeApi());
    EXPECT_EQ(reader.readMemoryMap(TARGET).status, MemoryMapReadStatus::PermissionDenied);
    EXPECT_EQ(fake().openAccess.size(), 2U); // the full right, then the limited one
    EXPECT_EQ(fake().queries, 0);

    fake().openError = ERROR_INVALID_PARAMETER; // no such PID
    EXPECT_EQ(reader.readMemoryMap(TARGET).status, MemoryMapReadStatus::ProcessExited);
}

TEST_F(WindowsProcessMemoryMapTest, ReusedPidAndUnknownIdentityAreNotRead)
{
    fake().creationTicks = START_TICKS + 1;
    WindowsProcessMemoryMapReader reader(fakeApi());
    EXPECT_EQ(reader.readMemoryMap(TARGET).status, MemoryMapReadStatus::ProcessExited);
    EXPECT_EQ(fake().queries, 0);

    EXPECT_EQ(reader.readMemoryMap({.pid = TARGET.pid, .startTimeTicks = 0}).status, MemoryMapReadStatus::IdentityUnknown);
    EXPECT_EQ(reader.readMemoryMap({.pid = 0, .startTimeTicks = START_TICKS}).status, MemoryMapReadStatus::ProcessExited);
}

TEST_F(WindowsProcessMemoryMapTest, AnExitedProcessIsNotReadAsItsMap)
{
    fake().exitCode = 0; // exited, its object only held open
    WindowsProcessMemoryMapReader reader(fakeApi());
    EXPECT_EQ(reader.readMemoryMap(TARGET).status, MemoryMapReadStatus::ProcessExited);

    // Exits during the walk: a query fails on its torn-down address space.
    fake().exitCode = STILL_ACTIVE;
    fake().exitAfterQueries = 3;
    const MemoryMapReadResult midWalk = reader.readMemoryMap(TARGET);
    EXPECT_EQ(midWalk.status, MemoryMapReadStatus::ProcessExited);
    EXPECT_TRUE(midWalk.mappings.empty());
}

TEST_F(WindowsProcessMemoryMapTest, AFailedQuerySaysWhy)
{
    fake().queryError = ERROR_NOT_ENOUGH_MEMORY;
    WindowsProcessMemoryMapReader reader(fakeApi());
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    EXPECT_EQ(result.status, MemoryMapReadStatus::Failed);
    EXPECT_FALSE(result.detail.empty());
    EXPECT_TRUE(result.mappings.empty());
}

TEST_F(WindowsProcessMemoryMapTest, StopsAtTheCapAndSaysSo)
{
    WindowsProcessMemoryMapReader reader(fakeApi(), 4);
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    EXPECT_TRUE(result.truncated);
    ASSERT_EQ(result.mappings.size(), 4U);
    EXPECT_EQ(result.mappings[3].start, SECTION);
    // It stopped there: the image's regions were never asked about.
    EXPECT_LT(fake().queries, static_cast<int>(sampleRegions().size()));

    WindowsProcessMemoryMapReader exact(fakeApi(), 7);
    const MemoryMapReadResult all = exact.readMemoryMap(TARGET);
    EXPECT_FALSE(all.truncated);
    EXPECT_EQ(all.mappings.size(), 7U);
}

TEST_F(WindowsProcessMemoryMapTest, CountsEachCommittedRegionsResidentSharedAndPrivatePages)
{
    residentPages(HEAP, 3, pageFlags(true, 0, PAGE_READWRITE, false));             // three of the heap's 16
    residentPages(VIEW, 1, pageFlags(true, 1, PAGE_READONLY, true));               // the view, this process's alone
    residentPages(IMAGE + 0x1000, 5, pageFlags(true, 4, PAGE_EXECUTE_READ, true)); // ntdll's code, shared
    residentPages(IMAGE + 0x12000, 1, pageFlags(true, 0, PAGE_READWRITE, false));  // its data, written: now private
    WindowsProcessMemoryMapReader reader(fakeApi());
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    ASSERT_EQ(result.mappings.size(), 7U);

    const MemoryMapping& heap = result.mappings[0];
    EXPECT_EQ(heap.residentBytes, 3 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(heap.privateDirtyBytes, 3 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(heap.privateCleanBytes, 0U);
    EXPECT_EQ(heap.sharedCleanBytes, 0U);
    EXPECT_FALSE(result.mappings[1].residentBytes.has_value()); // reserved: nothing to ask about
    EXPECT_EQ(result.mappings[2].privateCleanBytes, WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(result.mappings[3].residentBytes, 0U); // the section: asked, none resident
    EXPECT_EQ(result.mappings[5].sharedCleanBytes, 5 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(result.mappings[5].residentBytes, 5 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(result.mappings[6].privateDirtyBytes, WORKING_SET_PAGE_BYTES);
    for (const MemoryMapping& mapping : result.mappings)
    {
        EXPECT_FALSE(mapping.pssBytes.has_value());
        EXPECT_FALSE(mapping.swapBytes.has_value());
    }

    const Domain::MemoryMap::Summary summary = Domain::MemoryMap::summarize(result.mappings);
    EXPECT_EQ(summary.all.residentBytes, 10 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(summary.all.privateBytes, 5 * WORKING_SET_PAGE_BYTES);
    EXPECT_EQ(summary.all.sharedBytes, 5 * WORKING_SET_PAGE_BYTES);
    // Every committed page was asked about once: 16 + 4 + 2 + 1 + 17 + 2, in one call per region.
    EXPECT_EQ(fake().workingSetBatches, (std::vector<std::size_t>{16, 4, 2, 1, 17, 2}));
}

TEST_F(WindowsProcessMemoryMapTest, AsksInBatchesOfAtMostThePagesPerQuery)
{
    residentPages(HEAP, 16, pageFlags(true, 0, PAGE_READWRITE, false));
    residentPages(IMAGE + 0x1000, 17, pageFlags(true, 2, PAGE_EXECUTE_READ, true));
    const MemoryMapReadResult whole = WindowsProcessMemoryMapReader(fakeApi()).readMemoryMap(TARGET);
    fake().workingSetBatches.clear();

    const MemoryMapReadResult batched =
        WindowsProcessMemoryMapReader(fakeApi(), MAX_MEMORY_MAP_REGIONS, {.pagesPerQuery = 4}).readMemoryMap(TARGET);
    ASSERT_EQ(batched.status, MemoryMapReadStatus::Ok);
    EXPECT_EQ(batched.mappings, whole.mappings); // the same counts, however they're asked for
    EXPECT_EQ(fake().workingSetBatches, (std::vector<std::size_t>{4, 4, 4, 4, 4, 2, 1, 4, 4, 4, 4, 1, 2}));
    EXPECT_EQ(batched.mappings[5].sharedCleanBytes, 17 * WORKING_SET_PAGE_BYTES);
}

TEST_F(WindowsProcessMemoryMapTest, ARegionPastTheCapsKeepsNoSizes)
{
    residentPages(HEAP, 2, pageFlags(true, 0, PAGE_READWRITE, false));
    // Past the region size cap: the heap's 64 KiB and the image's 68 KiB of code aren't asked about.
    const MemoryMapReadResult sized =
        WindowsProcessMemoryMapReader(fakeApi(), MAX_MEMORY_MAP_REGIONS, {.maxRegionBytes = 0x4000}).readMemoryMap(TARGET);
    ASSERT_EQ(sized.status, MemoryMapReadStatus::Ok);
    EXPECT_FALSE(sized.mappings[0].residentBytes.has_value());
    EXPECT_TRUE(sized.mappings[2].residentBytes.has_value()); // the view's 16 KiB, at the cap, is
    EXPECT_FALSE(sized.mappings[5].residentBytes.has_value());
    EXPECT_EQ(fake().workingSetBatches, (std::vector<std::size_t>{4, 2, 1, 2}));

    // Past the read's page budget: the heap takes 16 of 18, the section the last 2; the rest don't fit.
    fake().workingSetBatches.clear();
    const MemoryMapReadResult budgeted =
        WindowsProcessMemoryMapReader(fakeApi(), MAX_MEMORY_MAP_REGIONS, {.maxPagesPerRead = 18}).readMemoryMap(TARGET);
    ASSERT_EQ(budgeted.status, MemoryMapReadStatus::Ok);
    EXPECT_EQ(budgeted.mappings[0].residentBytes, 2 * WORKING_SET_PAGE_BYTES);
    EXPECT_FALSE(budgeted.mappings[2].residentBytes.has_value());
    EXPECT_EQ(budgeted.mappings[3].residentBytes, 0U);
    for (std::size_t i = 4; i < 7; ++i)
    {
        EXPECT_FALSE(budgeted.mappings[i].residentBytes.has_value()) << i;
    }
    EXPECT_EQ(fake().workingSetBatches, (std::vector<std::size_t>{16, 2}));

    // The summary leaves out what wasn't measured, rather than counting it as nothing resident.
    EXPECT_EQ(Domain::MemoryMap::summarize(budgeted.mappings).all.residentBytes, 2 * WORKING_SET_PAGE_BYTES);
}

TEST_F(WindowsProcessMemoryMapTest, TheLimitedRightLeavesTheSizesAbsent)
{
    residentPages(HEAP, 3, pageFlags(true, 0, PAGE_READWRITE, false));
    fake().refuseFullQuery = true;
    const MemoryMapReadResult result = WindowsProcessMemoryMapReader(fakeApi()).readMemoryMap(TARGET);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    EXPECT_TRUE(fake().workingSetBatches.empty()); // never asked
    EXPECT_TRUE(std::ranges::none_of(result.mappings, [](const MemoryMapping& m) { return m.residentBytes.has_value(); }));
    const Domain::MemoryMap::Summary summary = Domain::MemoryMap::summarize(result.mappings);
    EXPECT_FALSE(summary.all.residentBytes.has_value());
    EXPECT_FALSE(summary.all.privateBytes.has_value());
    EXPECT_FALSE(summary.all.sharedBytes.has_value());
}

TEST_F(WindowsProcessMemoryMapTest, AFailedWorkingSetQueryLeavesItsRegionEmpty)
{
    // Refused: the first query says so, and no other region is asked about. The map itself still reads.
    fake().workingSetError = ERROR_ACCESS_DENIED;
    WindowsProcessMemoryMapReader reader(fakeApi());
    const MemoryMapReadResult refused = reader.readMemoryMap(TARGET);
    ASSERT_EQ(refused.status, MemoryMapReadStatus::Ok);
    EXPECT_EQ(refused.mappings.size(), 7U);
    EXPECT_EQ(fake().workingSetBatches.size(), 1U);
    EXPECT_TRUE(std::ranges::none_of(refused.mappings, [](const MemoryMapping& m) { return m.residentBytes.has_value(); }));

    // Any other failure leaves only that region empty: each is still asked about.
    fake().workingSetBatches.clear();
    fake().workingSetError = ERROR_BAD_LENGTH;
    const MemoryMapReadResult failed = reader.readMemoryMap(TARGET);
    ASSERT_EQ(failed.status, MemoryMapReadStatus::Ok);
    EXPECT_EQ(fake().workingSetBatches.size(), 6U);
    EXPECT_TRUE(std::ranges::none_of(failed.mappings, [](const MemoryMapping& m) { return m.residentBytes.has_value(); }));
}

TEST_F(WindowsProcessMemoryMapTest, ARegionThatEndsAtTheTopEndsTheWalk)
{
    // The last region runs to the very top of the address space: its end wraps, and the walk stops there.
    fake().regions.back().size = static_cast<SIZE_T>(0) - static_cast<SIZE_T>(fake().regions.back().base);
    fake().regions.back().state = MEM_COMMIT;
    fake().regions.back().type = MEM_PRIVATE;
    fake().regions.back().protect = PAGE_READONLY;
    WindowsProcessMemoryMapReader reader(fakeApi());
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    ASSERT_EQ(result.mappings.size(), 8U);
    EXPECT_EQ(result.mappings.back().end, std::numeric_limits<std::uint64_t>::max()); // held at the top, not wrapped to 0
}

// --- This process, through the real calls (read-only) ---

TEST(WindowsProcessMemoryMapRealTest, ReadsThisProcess)
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    ASSERT_NE(GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime), 0);
    const ProcessTarget self{
        .pid = static_cast<std::int32_t>(GetCurrentProcessId()),
        .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
    };
    WindowsProcessMemoryMapReader reader;
    const MemoryMapReadResult result = reader.readMemoryMap(self);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok) << result.detail;
    EXPECT_FALSE(result.truncated);
    ASSERT_FALSE(result.mappings.empty());
    EXPECT_TRUE(std::ranges::is_sorted(result.mappings, {}, &MemoryMapping::start));

    // ntdll's image, with an executable region (named by its drive path: checked last).
    const auto ntdll = std::ranges::find_if(
        result.mappings,
        [](const MemoryMapping& m) { return m.kind == MemoryMappingKind::Image && m.executable && m.path.ends_with("ntdll.dll"); });
    ASSERT_NE(ntdll, result.mappings.end());

    // This function's own code is in a region that's executable, and a local in one that's writable.
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(&GetCurrentProcessId);
    const auto contains = [&result](std::uintptr_t address)
    {
        return std::ranges::find_if(result.mappings, [address](const MemoryMapping& m) { return address >= m.start && address < m.end; });
    };
    const auto code = contains(codeAddress);
    ASSERT_NE(code, result.mappings.end());
    EXPECT_TRUE(code->executable);
    const int local = 0;
    const auto stack = contains(reinterpret_cast<std::uintptr_t>(&local));
    ASSERT_NE(stack, result.mappings.end());
    EXPECT_EQ(stack->kind, MemoryMappingKind::Anonymous);
    EXPECT_TRUE(stack->writable);
    EXPECT_FALSE(stack->reserved);

    // A thread's stack reserves more than it commits: some private region is reserved.
    EXPECT_TRUE(std::ranges::any_of(result.mappings, [](const MemoryMapping& m) { return m.reserved; }));

    // The working set (#4031): the stack's page holding the local is resident and this process's alone, and
    // every measured region's resident size is its four parts', within its size; a reserved one has none.
    ASSERT_TRUE(stack->residentBytes.has_value());
    EXPECT_GT(stack->residentBytes.value_or(0), 0U);
    EXPECT_GT(stack->privateDirtyBytes.value_or(0), 0U);
    for (const MemoryMapping& m : result.mappings)
    {
        if (m.reserved)
        {
            EXPECT_FALSE(m.residentBytes.has_value());
            continue;
        }
        if (!m.residentBytes.has_value())
        {
            continue; // past a cap
        }
        EXPECT_LE(*m.residentBytes, m.sizeBytes());
        EXPECT_EQ(*m.residentBytes,
                  m.sharedCleanBytes.value_or(0) + m.sharedDirtyBytes.value_or(0) + m.privateCleanBytes.value_or(0) +
                      m.privateDirtyBytes.value_or(0));
        EXPECT_FALSE(m.pssBytes.has_value());
    }

    // ntdll is named by its drive path. Not in Windows Sandbox (#4827), which serves it from the host over VSMB, a
    // \Device\vmsmb\... name with no drive-letter form.
    if (TestSupport::isWindowsSandbox())
    {
        GTEST_SKIP() << "ntdll's drive path: Windows Sandbox serves it from the host over VSMB (" << ntdll->path << ")";
    }
    EXPECT_EQ(ntdll->path.find("\\Device\\"), std::string::npos) << ntdll->path;
    EXPECT_EQ(ntdll->path[1], ':') << ntdll->path;
}

} // namespace
} // namespace Platform::Windows
