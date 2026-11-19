/// @file test_WindowsProcessModules.cpp
/// @brief Platform::Windows::WindowsProcessModulesReader (#802) against fake kernel32 calls: the modules listed with
/// paths, bases, sizes and versions, the version read once per file, a denied open, a reused PID, a list that grows
/// between the size query and the copy, a module unloaded mid-read, an enumeration that fails, and the handle closed
/// exactly once. And #4373: the path in the process's loader data is shown, never opened; the version comes from the
/// file the kernel says the image was mapped from, and only from one on a local volume, so a module mapped from a
/// network share or a device is listed, says its version wasn't read, and is never opened; and that file is opened by
/// its NT name with no reparse followed and identification-level security, and isn't read when it is a link, off a disk
/// or a redirector's. Plus reads of this test process through the real calls, and a junction in a scratch folder that
/// the real open refuses to follow.

#include "Mocks/WindowsFakeProcessIdentity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessModules.h"
#include "Support/Junction.h"
#include "Support/PeImageBuilder.h"

#include <gtest/gtest.h>

// clang-format off
#include <winioctl.h> // FILE_DEVICE_DISK and the other device types
#include <winternl.h> // OBJECT_ATTRIBUTES
// clang-format on

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform::Windows
{
namespace
{

constexpr std::uint64_t START_TICKS = 0x01DB'0000'1234'5678ULL;
constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = START_TICKS};

constexpr std::wstring_view C_DEVICE = L"\\Device\\HarddiskVolume3";
constexpr std::wstring_view D_DEVICE = L"\\Device\\CdRom0";
constexpr std::wstring_view Z_DEVICE = L"\\Device\\LanmanRedirector\\;Z:0000000000012345\\server\\share";

/// @p dosPath ("C:\x.dll") as the NT name GetMappedFileNameW gives a file on C:.
[[nodiscard]] std::wstring onC(std::wstring_view dosPath)
{
    return std::wstring(C_DEVICE) + std::wstring(dosPath.substr(2));
}

struct FakeModule
{
    std::uintptr_t base = 0;
    DWORD size = 0;
    std::wstring path;     // the loader data's FullDllName (GetModuleFileNameExW), which the process can rewrite
    std::wstring mapped;   // the file the image section was made from (GetMappedFileNameW); empty: it can't be named
    bool unloaded = false; // listed, but gone by the time it is queried
};

/// The answers the fakes give, set per test.
struct FakeProcess
{
    DWORD openError = 0; // non-zero: OpenProcess fails with it
    std::uint64_t creationTicks = START_TICKS;
    DWORD enumError = 0;
    DWORD exitCode = STILL_ACTIVE;
    int growOnFirstEnum = 0; // modules added after the first EnumProcessModulesEx call
    std::vector<FakeModule> modules;
    /// The versions of the files there are, by NT name; a name not here has none.
    std::map<std::wstring, ModuleVersion, std::less<>> files;
    std::vector<std::wstring> versionReads; // every NT name readFileVersion was given, in order
    int enumCalls = 0;
    int driveTypeCalls = 0;
    int opens = 0;
    int closes = 0;
};

FakeProcess& fake()
{
    static FakeProcess instance;
    return instance;
}

/// OpenProcess, CloseHandle, GetProcessTimes and GetExitCodeProcess: the shared identity fake (#3849).
using Identity =
    TestMocks::FakeProcessIdentity<FakeProcess, &fake, TARGET.pid, static_cast<DWORD>(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ)>;

BOOL WINAPI fakeEnumProcessModulesEx(HANDLE /*process*/, HMODULE* modules, DWORD bytes, LPDWORD needed, DWORD filter)
{
    EXPECT_EQ(filter, static_cast<DWORD>(LIST_MODULES_ALL));
    ++fake().enumCalls;
    if (fake().enumError != 0)
    {
        SetLastError(fake().enumError);
        return FALSE;
    }
    if (fake().enumCalls == 1)
    {
        for (int i = 0; i < fake().growOnFirstEnum; ++i)
        {
            const auto base = 0x7FF900000000ULL + (static_cast<std::uintptr_t>(i) * 0x100000U);
            const std::wstring path = L"C:\\Late\\late" + std::to_wstring(i) + L".dll";
            fake().modules.push_back({
                .base = base,
                .size = 0x1000,
                .path = path,
                .mapped = onC(path),
                .unloaded = false,
            });
        }
    }
    *needed = static_cast<DWORD>(fake().modules.size() * sizeof(HMODULE));
    const std::size_t fits = std::min<std::size_t>(fake().modules.size(), bytes / sizeof(HMODULE));
    for (std::size_t i = 0; i < fits; ++i)
    {
        modules[i] = reinterpret_cast<HMODULE>(fake().modules[i].base); // NOLINT(performance-no-int-to-ptr) - a module handle is its base
    }
    return TRUE;
}

const FakeModule* findModule(std::uintptr_t base)
{
    const auto it = std::ranges::find(fake().modules, base, &FakeModule::base);
    return (it == fake().modules.end() || it->unloaded) ? nullptr : &*it;
}

const FakeModule* findModule(HMODULE module)
{
    return findModule(reinterpret_cast<std::uintptr_t>(module));
}

/// Copies @p text into @p buffer as the K32 calls do: its length, or 0 when it doesn't fit.
DWORD copyOut(std::wstring_view text, LPWSTR buffer, DWORD size)
{
    if (text.empty() || text.size() >= size)
    {
        return 0;
    }
    std::ranges::copy(text, buffer);
    buffer[text.size()] = L'\0';
    return static_cast<DWORD>(text.size());
}

DWORD WINAPI fakeGetModuleFileNameEx(HANDLE /*process*/, HMODULE module, LPWSTR buffer, DWORD size)
{
    const FakeModule* found = findModule(module);
    return found == nullptr ? 0 : copyOut(found->path, buffer, size);
}

DWORD WINAPI fakeGetMappedFileName(HANDLE process, LPVOID address, LPWSTR buffer, DWORD size)
{
    EXPECT_EQ(process, Identity::handle());
    const FakeModule* found = findModule(reinterpret_cast<std::uintptr_t>(address));
    return found == nullptr ? 0 : copyOut(found->mapped, buffer, size);
}

BOOL WINAPI fakeGetModuleInformation(HANDLE /*process*/, HMODULE module, LPMODULEINFO info, DWORD /*bytes*/)
{
    const FakeModule* found = findModule(module);
    if (found == nullptr)
    {
        return FALSE;
    }
    info->lpBaseOfDll = reinterpret_cast<LPVOID>(found->base); // NOLINT(performance-no-int-to-ptr) - a fake base
    info->SizeOfImage = found->size;
    info->EntryPoint = nullptr;
    return TRUE;
}

/// C: a fixed disk, D: a CD-ROM drive, Z: a mapped network drive.
DWORD WINAPI fakeGetLogicalDrives()
{
    return (1U << 2U) | (1U << 3U) | (1U << 25U);
}

DWORD WINAPI fakeQueryDosDevice(LPCWSTR name, LPWSTR target, DWORD size)
{
    const std::wstring_view drive(name);
    std::wstring_view device;
    if (drive == L"C:")
    {
        device = C_DEVICE;
    }
    else if (drive == L"D:")
    {
        device = D_DEVICE;
    }
    else if (drive == L"Z:")
    {
        device = Z_DEVICE;
    }
    // The NUL-separated list's own terminator after the first target.
    const DWORD length = copyOut(device, target, size - 1);
    if (length != 0)
    {
        target[length + 1] = L'\0';
    }
    return length == 0 ? 0 : length + 2;
}

UINT WINAPI fakeGetDriveType(LPCWSTR root)
{
    ++fake().driveTypeCalls;
    const std::wstring_view drive(root);
    if (drive == L"C:\\")
    {
        return DRIVE_FIXED;
    }
    if (drive == L"D:\\")
    {
        return DRIVE_CDROM;
    }
    return drive == L"Z:\\" ? DRIVE_REMOTE : DRIVE_NO_ROOT_DIR;
}

std::optional<ModuleVersion> fakeReadFileVersion(const std::wstring& ntName)
{
    fake().versionReads.push_back(ntName);
    if (const auto found = fake().files.find(ntName); found != fake().files.end())
    {
        return found->second;
    }
    return std::nullopt;
}

ProcessModuleFunctions fakeApi()
{
    return {
        .openProcess = &Identity::openProcess,
        .closeHandle = &Identity::closeHandle,
        .getProcessTimes = &Identity::getProcessTimes,
        .getExitCodeProcess = &Identity::getExitCodeProcess,
        .enumProcessModulesEx = &fakeEnumProcessModulesEx,
        .getModuleFileNameEx = &fakeGetModuleFileNameEx,
        .getModuleInformation = &fakeGetModuleInformation,
        .getMappedFileName = &fakeGetMappedFileName,
        .getLogicalDrives = &fakeGetLogicalDrives,
        .queryDosDevice = &fakeQueryDosDevice,
        .getDriveType = &fakeGetDriveType,
        .readFileVersion = &fakeReadFileVersion,
    };
}

constexpr ModuleVersion NTDLL_64{.major = 10, .minor = 0, .build = 26100, .revision = 4202};
constexpr ModuleVersion NTDLL_32{.major = 10, .minor = 0, .build = 26100, .revision = 4203};

/// Whether readFileVersion was ever given a name that starts with @p prefix.
[[nodiscard]] bool wasRead(std::wstring_view prefix)
{
    return std::ranges::any_of(fake().versionReads, [prefix](const std::wstring& name) { return name.starts_with(prefix); });
}

class WindowsProcessModulesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = {};
        fake().modules = {
            {
                .base = 0x7FF6A0000000,
                .size = 0x52000,
                .path = L"C:\\Apps\\app.exe",
                .mapped = onC(L"C:\\Apps\\app.exe"),
                .unloaded = false,
            },
            {
                .base = 0x7FF8A1B20000,
                .size = 0x1F8000,
                .path = L"C:\\Windows\\System32\\ntdll.dll",
                .mapped = onC(L"C:\\Windows\\System32\\ntdll.dll"),
                .unloaded = false,
            },
            {
                .base = 0x77A00000,
                .size = 0x1A000,
                .path = L"C:\\Windows\\SysWOW64\\ntdll.dll",
                .mapped = onC(L"C:\\Windows\\SysWOW64\\ntdll.dll"),
                .unloaded = false,
            },
        };
        fake().files = {{onC(L"C:\\Windows\\System32\\ntdll.dll"), NTDLL_64}, {onC(L"C:\\Windows\\SysWOW64\\ntdll.dll"), NTDLL_32}};
    }

    void TearDown() override
    {
        EXPECT_EQ(fake().opens, fake().closes); // every handle opened was closed
    }
};

TEST_F(WindowsProcessModulesTest, ListsEveryModuleWithItsPathBaseSizeAndVersion)
{
    WindowsProcessModulesReader reader(fakeApi());
    EXPECT_TRUE(reader.hasModules());
    const ModulesReadResult result = reader.readModules(TARGET);
    ASSERT_EQ(result.status, ModulesReadStatus::Ok);
    ASSERT_EQ(result.modules.size(), 3U);
    EXPECT_EQ(result.modules[0].path, "C:\\Apps\\app.exe");
    EXPECT_EQ(result.modules[0].baseAddress, 0x7FF6A0000000U);
    EXPECT_EQ(result.modules[0].sizeBytes, 0x52000U);
    EXPECT_FALSE(result.modules[0].version.has_value());
    EXPECT_FALSE(result.modules[0].versionNotRead) << "a local file without a version was read, and has none";
    EXPECT_EQ(result.modules[1].version, std::optional(NTDLL_64));
    EXPECT_EQ(result.modules[2].baseAddress, 0x77A00000U); // a WOW64 process's 32-bit modules too
    EXPECT_EQ(result.modules[2].version, std::optional(NTDLL_32));
    EXPECT_TRUE(std::ranges::none_of(result.modules, &ProcessModule::versionNotRead));
}

TEST_F(WindowsProcessModulesTest, ReadsEachFilesVersionOnce)
{
    WindowsProcessModulesReader reader(fakeApi());
    static_cast<void>(reader.readModules(TARGET));
    static_cast<void>(reader.readModules(TARGET));
    EXPECT_EQ(fake().versionReads.size(), 3U); // each file once, the one without a version too
    EXPECT_EQ(reader.cachedVersionCount(), 3U);
}

// #4373: the loader data's FullDllName is the process's own memory, so it can point anywhere: a share on a host of its
// choosing, a named pipe it serves, another file entirely. None of those is ever opened; the version is the file the
// kernel says the image was mapped from, and the row still shows the path the process gave.
TEST_F(WindowsProcessModulesTest, TheLoaderDataPathIsShownButNeverOpened)
{
    fake().modules[1].path = L"\\\\server\\share\\a.dll";
    fake().modules[2].path = L"\\\\.\\pipe\\x";
    fake().modules.push_back({
        .base = 0x7FF8B0000000,
        .size = 0x10000,
        .path = L"\\\\?\\UNC\\attacker\\share\\b.dll",
        .mapped = onC(L"C:\\Windows\\System32\\ntdll.dll"),
        .unloaded = false,
    });
    WindowsProcessModulesReader reader(fakeApi());
    const ModulesReadResult result = reader.readModules(TARGET);
    ASSERT_EQ(result.status, ModulesReadStatus::Ok);
    ASSERT_EQ(result.modules.size(), 4U) << "every row is still listed";
    EXPECT_EQ(result.modules[1].path, "\\\\server\\share\\a.dll");
    EXPECT_EQ(result.modules[2].path, "\\\\.\\pipe\\x");
    EXPECT_EQ(result.modules[1].version, std::optional(NTDLL_64)) << "the mapped image's version, not the named file's";
    EXPECT_EQ(result.modules[2].version, std::optional(NTDLL_32));
    EXPECT_EQ(result.modules[3].version, std::optional(NTDLL_64));
    EXPECT_FALSE(wasRead(L"\\\\")) << "no loader-data path is ever opened";
    for (const std::wstring& name : fake().versionReads)
    {
        EXPECT_TRUE(name.starts_with(C_DEVICE)) << "only the kernel's names of local files are opened";
    }
}

// #4373: a decoy. The process says its module is a signed system file; the image it mapped is its own. The version is
// the mapped file's, and the decoy is never opened.
TEST_F(WindowsProcessModulesTest, ADecoyLoaderPathGetsTheMappedImagesVersion)
{
    constexpr ModuleVersion PAYLOAD{.major = 6, .minor = 6, .build = 6, .revision = 6};
    const std::wstring payload = onC(L"C:\\Users\\Public\\payload.dll");
    fake().files.emplace(payload, PAYLOAD);
    fake().files.emplace(onC(L"C:\\Windows\\System32\\kernel32.dll"),
                         ModuleVersion{.major = 10, .minor = 0, .build = 26100, .revision = 1});
    fake().modules.push_back({
        .base = 0x7FF8C0000000,
        .size = 0x8000,
        .path = L"C:\\Windows\\System32\\kernel32.dll",
        .mapped = payload,
        .unloaded = false,
    });
    WindowsProcessModulesReader reader(fakeApi());
    const ModulesReadResult result = reader.readModules(TARGET);
    ASSERT_EQ(result.modules.size(), 4U);
    EXPECT_EQ(result.modules[3].path, "C:\\Windows\\System32\\kernel32.dll");
    EXPECT_EQ(result.modules[3].version, std::optional(PAYLOAD));
    EXPECT_FALSE(wasRead(onC(L"C:\\Windows\\System32\\kernel32.dll")));
}

// #4373: a module whose image really was mapped from a network share (a UNC path, a mapped drive) or from a device
// that isn't a local volume is listed and says its version wasn't read; its file is never opened, since that would
// connect, with TaskSmack's credentials (or the elevated helper's), to wherever it is.
TEST_F(WindowsProcessModulesTest, AModuleMappedFromANetworkShareOrADeviceIsListedButNeverOpened)
{
    fake().modules.push_back({
        .base = 0x10000000,
        .size = 0x1000,
        .path = L"\\\\server\\share\\a.dll",
        .mapped = L"\\Device\\Mup\\server\\share\\a.dll",
        .unloaded = false,
    });
    fake().modules.push_back({
        .base = 0x20000000,
        .size = 0x1000,
        .path = L"Z:\\tools\\z.dll",
        .mapped = std::wstring(Z_DEVICE) + L"\\tools\\z.dll",
        .unloaded = false,
    });
    fake().modules.push_back({
        .base = 0x30000000,
        .size = 0x1000,
        .path = L"\\\\.\\pipe\\x",
        .mapped = L"\\Device\\NamedPipe\\x",
        .unloaded = false,
    });
    fake().modules.push_back({
        .base = 0x40000000,
        .size = 0x1000,
        .path = L"C:\\Windows\\System32\\x.dll",
        .mapped = L"\\Device\\HarddiskVolume3\\..\\Mup\\server\\share\\x.dll",
        .unloaded = false,
    });
    WindowsProcessModulesReader reader(fakeApi());
    const ModulesReadResult result = reader.readModules(TARGET);
    ASSERT_EQ(result.status, ModulesReadStatus::Ok);
    ASSERT_EQ(result.modules.size(), 7U);
    for (std::size_t i = 3; i < 7; ++i)
    {
        EXPECT_TRUE(result.modules[i].versionNotRead) << i;
        EXPECT_FALSE(result.modules[i].version.has_value()) << i;
    }
    EXPECT_EQ(result.modules[3].path, "\\\\server\\share\\a.dll");
    EXPECT_FALSE(wasRead(L"\\Device\\Mup"));
    EXPECT_FALSE(wasRead(Z_DEVICE));
    EXPECT_FALSE(wasRead(L"\\Device\\NamedPipe"));
    EXPECT_FALSE(wasRead(L"\\Device\\HarddiskVolume3\\.."));
    EXPECT_EQ(fake().versionReads.size(), 3U) << "only the three local files";
    EXPECT_EQ(fake().driveTypeCalls, 3) << "the drive letters are read once per read";
}

// A local optical drive's file, and a volume with no drive letter (mounted in a folder), are local.
TEST_F(WindowsProcessModulesTest, ACdRomDrivesFileAndAnUnletteredVolumesFileAreRead)
{
    const std::wstring onD = std::wstring(D_DEVICE) + L"\\setup\\d.dll";
    const std::wstring unlettered = L"\\Device\\HarddiskVolume12\\mnt\\e.dll";
    fake().files.emplace(onD, ModuleVersion{.major = 1, .minor = 2, .build = 3, .revision = 4});
    fake().files.emplace(unlettered, ModuleVersion{.major = 5, .minor = 6, .build = 7, .revision = 8});
    fake().modules.push_back({
        .base = 0x10000000,
        .size = 0x1000,
        .path = L"D:\\setup\\d.dll",
        .mapped = onD,
        .unloaded = false,
    });
    fake().modules.push_back({
        .base = 0x20000000,
        .size = 0x1000,
        .path = L"C:\\mnt\\e.dll",
        .mapped = unlettered,
        .unloaded = false,
    });
    WindowsProcessModulesReader reader(fakeApi());
    const ModulesReadResult result = reader.readModules(TARGET);
    ASSERT_EQ(result.modules.size(), 5U);
    EXPECT_EQ(result.modules[3].version, std::optional(ModuleVersion{.major = 1, .minor = 2, .build = 3, .revision = 4}));
    EXPECT_EQ(result.modules[4].version, std::optional(ModuleVersion{.major = 5, .minor = 6, .build = 7, .revision = 8}));
    EXPECT_FALSE(result.modules[3].versionNotRead);
    EXPECT_FALSE(result.modules[4].versionNotRead);
}

// A module whose mapping can't be named (unmapped since the listing) keeps its row, with no version, and nothing is
// opened for it: not by the loader data's path either.
TEST_F(WindowsProcessModulesTest, AModuleWhoseMappingCantBeNamedHasNoVersion)
{
    fake().modules[1].mapped.clear();
    WindowsProcessModulesReader reader(fakeApi());
    const ModulesReadResult result = reader.readModules(TARGET);
    ASSERT_EQ(result.modules.size(), 3U);
    EXPECT_FALSE(result.modules[1].version.has_value());
    EXPECT_FALSE(result.modules[1].versionNotRead);
    EXPECT_FALSE(wasRead(onC(L"C:\\Windows\\System32\\ntdll.dll")));
}

TEST_F(WindowsProcessModulesTest, DeniedOpenIsPermissionDenied)
{
    fake().openError = ERROR_ACCESS_DENIED;
    WindowsProcessModulesReader reader(fakeApi());
    EXPECT_EQ(reader.readModules(TARGET).status, ModulesReadStatus::PermissionDenied);

    fake().openError = ERROR_INVALID_PARAMETER; // no such PID
    EXPECT_EQ(reader.readModules(TARGET).status, ModulesReadStatus::ProcessExited);
}

TEST_F(WindowsProcessModulesTest, ReusedPidAndUnknownIdentityAreNotRead)
{
    fake().creationTicks = START_TICKS + 1;
    WindowsProcessModulesReader reader(fakeApi());
    EXPECT_EQ(reader.readModules(TARGET).status, ModulesReadStatus::ProcessExited);
    EXPECT_EQ(fake().enumCalls, 0);

    EXPECT_EQ(reader.readModules({.pid = TARGET.pid, .startTimeTicks = 0}).status, ModulesReadStatus::IdentityUnknown);
    EXPECT_EQ(reader.readModules({.pid = 0, .startTimeTicks = START_TICKS}).status, ModulesReadStatus::ProcessExited);
}

TEST_F(WindowsProcessModulesTest, GrowsTheListWhenModulesLoadMidRead)
{
    fake().growOnFirstEnum = 300; // past the first 256-handle buffer
    WindowsProcessModulesReader reader(fakeApi());
    const ModulesReadResult result = reader.readModules(TARGET);
    ASSERT_EQ(result.status, ModulesReadStatus::Ok);
    EXPECT_EQ(result.modules.size(), 303U);
    EXPECT_EQ(fake().enumCalls, 2);
}

TEST_F(WindowsProcessModulesTest, SkipsAModuleUnloadedMidRead)
{
    fake().modules[1].unloaded = true;
    WindowsProcessModulesReader reader(fakeApi());
    const ModulesReadResult result = reader.readModules(TARGET);
    ASSERT_EQ(result.status, ModulesReadStatus::Ok);
    EXPECT_EQ(result.modules.size(), 2U);
}

TEST_F(WindowsProcessModulesTest, FailedEnumerationSaysWhyOrThatTheProcessExited)
{
    fake().enumError = ERROR_PARTIAL_COPY;
    WindowsProcessModulesReader reader(fakeApi());
    const ModulesReadResult failed = reader.readModules(TARGET);
    EXPECT_EQ(failed.status, ModulesReadStatus::Failed);
    EXPECT_FALSE(failed.detail.empty());

    fake().enumError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(reader.readModules(TARGET).status, ModulesReadStatus::PermissionDenied);

    fake().exitCode = 0;
    EXPECT_EQ(reader.readModules(TARGET).status, ModulesReadStatus::ProcessExited);
}

// The real calls on this test process: ntdll's version is read from the file its image was mapped from (named by
// GetMappedFileNameW, opened through \\?\GLOBALROOT and read through the handle), and it is a local file.
TEST(WindowsProcessModulesRealTest, ReadsThisProcess)
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    ASSERT_NE(GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime), 0);
    const ProcessTarget self{.pid = static_cast<std::int32_t>(GetCurrentProcessId()),
                             .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime};
    WindowsProcessModulesReader reader;
    const ModulesReadResult result = reader.readModules(self);
    ASSERT_EQ(result.status, ModulesReadStatus::Ok);
    const auto ntdll = std::ranges::find_if(result.modules, [](const ProcessModule& m) { return m.path.ends_with("ntdll.dll"); });
    ASSERT_NE(ntdll, result.modules.end());
    EXPECT_GT(ntdll->sizeBytes, 0U);
    EXPECT_TRUE(ntdll->version.has_value());
    EXPECT_FALSE(ntdll->versionNotRead);
}

// readLocalFileVersion() on the real system: ntdll's NT name, as GetMappedFileNameW gives it, reads as the build and
// revision GetFileVersionInfoW reads by its path; a name that doesn't exist reads as none. Not the major and minor:
// GetFileVersionInfoW reports a system DLL's as 6.2 to a program whose manifest doesn't name Windows 10 (the version
// lie), as this test program's doesn't. The file's own bytes say 10.0, which is what the Modules section always showed
// TaskSmack, whose manifest does.
TEST(WindowsProcessModulesRealTest, ReadsAFilesVersionThroughItsNtName)
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    ASSERT_NE(ntdll, nullptr);
    std::wstring ntName(32768, L'\0');
    const DWORD length = GetMappedFileNameW(GetCurrentProcess(), ntdll, ntName.data(), static_cast<DWORD>(ntName.size()));
    ASSERT_GT(length, 0U) << GetLastError();
    ntName.resize(length);
    const std::optional<ModuleVersion> read = readLocalFileVersion(ntName);
    ASSERT_TRUE(read.has_value());
    const ModuleVersion version = read.value_or(ModuleVersion{});

    std::wstring dosPath(32768, L'\0');
    const DWORD dosLength = GetModuleFileNameW(ntdll, dosPath.data(), static_cast<DWORD>(dosPath.size()));
    ASSERT_GT(dosLength, 0U);
    dosPath.resize(dosLength);
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(dosPath.c_str(), &ignored);
    ASSERT_GT(size, 0U);
    std::vector<std::byte> block(size);
    ASSERT_NE(GetFileVersionInfoW(dosPath.c_str(), 0, size, block.data()), 0);
    void* data = nullptr;
    UINT bytes = 0;
    ASSERT_NE(VerQueryValueW(block.data(), L"\\", &data, &bytes), 0);
    ASSERT_GE(bytes, sizeof(VS_FIXEDFILEINFO));
    const auto* fixed = static_cast<const VS_FIXEDFILEINFO*>(data);
    EXPECT_GE(version.major, HIWORD(fixed->dwFileVersionMS));
    EXPECT_GE(version.major, 10U);
    EXPECT_EQ(version.build, HIWORD(fixed->dwFileVersionLS));
    EXPECT_EQ(version.revision, LOWORD(fixed->dwFileVersionLS));

    EXPECT_FALSE(readLocalFileVersion(ntName + L".no-such-file").has_value());
}

// #4373: how readLocalFileVersion() opens a name, through fakes. The kernel's NT name itself (no \\?\GLOBALROOT, a
// symbolic link), with OBJ_DONT_REPARSE so a junction or symlink on the way fails the open rather than being followed,
// FILE_OPEN_REPARSE_POINT so one at the name is opened as itself, identification-level security, and no write sharing.
// Then a link, a handle off a disk, or a redirector's file isn't read.
struct FakeLocalFile
{
    NTSTATUS openStatus = 0;
    DWORD attributes = FILE_ATTRIBUTE_ARCHIVE;
    DWORD reparseTag = 0;
    DWORD deviceType = FILE_DEVICE_DISK;
    ULONG characteristics = 0;
    bool remote = false;
    // What the open was asked.
    std::wstring name;
    ULONG objectAttributes = 0;
    bool rooted = false;
    SECURITY_IMPERSONATION_LEVEL impersonation = SecurityDelegation;
    ACCESS_MASK access = 0;
    ULONG share = 0;
    ULONG disposition = 0;
    ULONG options = 0;
    int opens = 0;
    int versionReads = 0;
};

FakeLocalFile& localFile()
{
    static FakeLocalFile instance;
    return instance;
}

NTSTATUS NTAPI fakeNtCreateFile(PHANDLE handle,
                                ACCESS_MASK access,
                                POBJECT_ATTRIBUTES attributes,
                                PIO_STATUS_BLOCK /*status*/,
                                PLARGE_INTEGER /*allocationSize*/,
                                ULONG /*fileAttributes*/,
                                ULONG share,
                                ULONG disposition,
                                ULONG options,
                                PVOID /*eaBuffer*/,
                                ULONG /*eaLength*/)
{
    FakeLocalFile& file = localFile();
    ++file.opens;
    file.name.assign(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
    file.objectAttributes = attributes->Attributes;
    file.rooted = attributes->RootDirectory != nullptr;
    if (attributes->SecurityQualityOfService != nullptr)
    {
        file.impersonation = static_cast<const SECURITY_QUALITY_OF_SERVICE*>(attributes->SecurityQualityOfService)->ImpersonationLevel;
    }
    file.access = access;
    file.share = share;
    file.disposition = disposition;
    file.options = options;
    if (file.openStatus < 0)
    {
        return file.openStatus;
    }
    *handle = CreateEventW(nullptr, TRUE, FALSE, nullptr); // a real handle, so its owner closes something real
    return 0;
}

NTSTATUS NTAPI fakeNtQueryVolumeInformationFile(HANDLE /*file*/, PIO_STATUS_BLOCK /*status*/, PVOID information, ULONG length, ULONG what)
{
    EXPECT_EQ(what, 4U); // FileFsDeviceInformation
    EXPECT_GE(length, 2 * sizeof(ULONG));
    const std::array<ULONG, 2> device{localFile().deviceType, localFile().characteristics};
    std::memcpy(information, device.data(), sizeof(device));
    return 0;
}

BOOL WINAPI fakeGetFileInformationByHandleEx(HANDLE /*file*/, FILE_INFO_BY_HANDLE_CLASS what, LPVOID information, DWORD /*bytes*/)
{
    if (what == FileAttributeTagInfo)
    {
        auto* tag = static_cast<FILE_ATTRIBUTE_TAG_INFO*>(information);
        tag->FileAttributes = localFile().attributes;
        tag->ReparseTag = localFile().reparseTag;
        return TRUE;
    }
    EXPECT_EQ(what, FileRemoteProtocolInfo);
    return localFile().remote ? TRUE : FALSE;
}

std::optional<ModuleVersion> fakeReadHeldVersion(HANDLE /*file*/)
{
    ++localFile().versionReads;
    return ModuleVersion{.major = 1, .minor = 2, .build = 3, .revision = 4};
}

constexpr LocalFileFunctions FAKE_LOCAL_FILE{
    .ntCreateFile = &fakeNtCreateFile,
    .ntQueryVolumeInformationFile = &fakeNtQueryVolumeInformationFile,
    .getFileInformationByHandleEx = &fakeGetFileInformationByHandleEx,
    .readHeldVersion = &fakeReadHeldVersion,
};

constexpr std::wstring_view LOCAL_NAME = L"\\Device\\HarddiskVolume3\\Windows\\System32\\ntdll.dll";

class WindowsLocalFileVersionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        localFile() = {};
    }
};

TEST_F(WindowsLocalFileVersionTest, OpensTheKernelsNameWithoutFollowingAnyReparseAndForIdentificationOnly)
{
    EXPECT_EQ(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE),
              std::optional(ModuleVersion{.major = 1, .minor = 2, .build = 3, .revision = 4}));
    const FakeLocalFile& file = localFile();
    EXPECT_EQ(file.name, LOCAL_NAME) << R"(the NT name itself, not through \\?\GLOBALROOT)";
    EXPECT_FALSE(file.rooted);
    EXPECT_NE(file.objectAttributes & 0x00001000U, 0U) << "OBJ_DONT_REPARSE";
    EXPECT_NE(file.objectAttributes & static_cast<ULONG>(OBJ_CASE_INSENSITIVE), 0U);
    EXPECT_NE(file.options & static_cast<ULONG>(FILE_OPEN_REPARSE_POINT), 0U);
    EXPECT_NE(file.options & static_cast<ULONG>(FILE_NON_DIRECTORY_FILE), 0U);
    EXPECT_NE(file.options & static_cast<ULONG>(FILE_SYNCHRONOUS_IO_NONALERT), 0U);
    EXPECT_EQ(file.impersonation, SecurityIdentification);
    EXPECT_EQ(file.disposition, static_cast<ULONG>(FILE_OPEN));
    EXPECT_EQ(file.share, static_cast<ULONG>(FILE_SHARE_READ | FILE_SHARE_DELETE)) << "never shared for write";
    EXPECT_EQ(file.access & (GENERIC_WRITE | FILE_WRITE_DATA | DELETE), 0U);
    EXPECT_EQ(file.versionReads, 1);
}

TEST_F(WindowsLocalFileVersionTest, AFailedOpenIsNoVersion)
{
    localFile().openStatus = static_cast<NTSTATUS>(0xC000050BL); // STATUS_REPARSE_POINT_ENCOUNTERED
    EXPECT_FALSE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value());
    EXPECT_EQ(localFile().versionReads, 0);
}

TEST_F(WindowsLocalFileVersionTest, ALinkAHandleOffADiskOrARedirectorsFileIsNotRead)
{
    localFile().attributes = FILE_ATTRIBUTE_REPARSE_POINT;
    localFile().reparseTag = IO_REPARSE_TAG_SYMLINK;
    EXPECT_FALSE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value()) << "a symlink, opened as itself";
    localFile().reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    EXPECT_FALSE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value()) << "a junction";

    localFile() = {};
    localFile().deviceType = FILE_DEVICE_NETWORK_FILE_SYSTEM;
    EXPECT_FALSE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value());
    localFile().deviceType = FILE_DEVICE_NAMED_PIPE;
    EXPECT_FALSE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value());
    localFile().deviceType = FILE_DEVICE_DISK;
    localFile().characteristics = 0x00000010; // FILE_REMOTE_DEVICE
    EXPECT_FALSE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value());

    localFile() = {};
    localFile().remote = true;
    EXPECT_FALSE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value());
    EXPECT_EQ(localFile().versionReads, 0);
}

// A reparse point that isn't a name surrogate is the file itself (a CompactOS system file's WOF tag), so it is read;
// so are a CD-ROM's and a virtual disk's files.
TEST_F(WindowsLocalFileVersionTest, ACompressedFileACdRomAndAVirtualDiskAreRead)
{
    localFile().attributes = FILE_ATTRIBUTE_REPARSE_POINT;
    localFile().reparseTag = IO_REPARSE_TAG_WOF;
    EXPECT_TRUE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value());
    localFile() = {};
    localFile().deviceType = FILE_DEVICE_CD_ROM;
    EXPECT_TRUE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value());
    localFile().deviceType = FILE_DEVICE_VIRTUAL_DISK;
    EXPECT_TRUE(readLocalFileVersion(std::wstring(LOCAL_NAME), FAKE_LOCAL_FILE).has_value());
}

TEST_F(WindowsLocalFileVersionTest, NoCallsOrAnEmptyNameIsNoVersion)
{
    EXPECT_FALSE(readLocalFileVersion(std::wstring(LOCAL_NAME), LocalFileFunctions{}).has_value());
    EXPECT_FALSE(readLocalFileVersion(std::wstring{}, FAKE_LOCAL_FILE).has_value());
    EXPECT_EQ(localFile().opens, 0);
}

/// A folder of this test's own under the temp folder, removed with everything in it at the end.
class ScratchFolder
{
  public:
    ScratchFolder()
        : m_Path(std::filesystem::temp_directory_path() / std::format(L"tasksmack-4373-{}-{}", GetCurrentProcessId(), GetTickCount64()))
    {
        std::filesystem::create_directories(m_Path);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    ScratchFolder(ScratchFolder&&) = delete;
    ScratchFolder& operator=(ScratchFolder&&) = delete;
    ~ScratchFolder()
    {
        try
        {
            std::error_code ignored;
            std::filesystem::remove_all(m_Path, ignored);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a destructor; a scratch folder left behind is only clutter
        {}
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

/// The NT name of @p path ("\Device\HarddiskVolume3\..."), as GetMappedFileNameW would give it; empty when unreadable.
[[nodiscard]] std::wstring ntNameOf(const std::filesystem::path& path)
{
    const UniqueHandle handle(CreateFileW(path.c_str(),
                                          FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS,
                                          nullptr));
    if (!handle)
    {
        return {};
    }
    std::wstring name(32768, L'\0');
    const DWORD length =
        GetFinalPathNameByHandleW(handle.get(), name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    if (length == 0 || length >= name.size())
    {
        return {};
    }
    name.resize(length);
    return name;
}

// #4373, on the real system with no network: a PE file with a version, read by its own NT name, reads as that version;
// the same file named through a junction (one the process could have planted on the way since the name was read) is
// refused by the open, never followed. The junction is removed before the folder.
TEST(WindowsProcessModulesRealTest, ANameThroughAJunctionIsNotFollowed)
{
    const ScratchFolder scratch;
    const std::filesystem::path target = scratch.path() / L"target";
    const std::filesystem::path link = scratch.path() / L"link";
    std::filesystem::create_directory(target);
    std::filesystem::create_directory(link);
    const TestSupport::PeImage image =
        TestSupport::buildPeImage({{0x0409U, TestSupport::versionInfoBlock(u"Co", (7U << 16U) | 8U, (9U << 16U) | 10U)}});
    {
        std::ofstream out(target / L"v.dll", std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.bytes.data()), static_cast<std::streamsize>(image.bytes.size()));
    }
    const std::optional<std::string> notMade = TestSupport::makeJunction(link, target);
    ASSERT_FALSE(notMade.has_value()) << notMade.value_or(std::string{});

    const std::wstring folder = ntNameOf(scratch.path());
    ASSERT_FALSE(folder.empty()) << GetLastError();
    EXPECT_EQ(readLocalFileVersion(folder + L"\\target\\v.dll"),
              std::optional(ModuleVersion{.major = 7, .minor = 8, .build = 9, .revision = 10}));
    EXPECT_FALSE(readLocalFileVersion(folder + L"\\link\\v.dll").has_value()) << "the junction on the way is never followed";
    EXPECT_TRUE(std::filesystem::exists(link / L"v.dll")) << "a path through it that a by-name open would have followed";

    ASSERT_TRUE(TestSupport::removeJunction(link)) << "an empty folder again, so removing the scratch folder removes nothing behind it";
}

} // namespace
} // namespace Platform::Windows
