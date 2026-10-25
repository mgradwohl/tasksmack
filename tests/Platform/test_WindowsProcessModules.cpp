/// @file test_WindowsProcessModules.cpp
/// @brief Platform::Windows::WindowsProcessModulesReader (#802) against fake kernel32/version calls:
/// the modules listed with paths, bases, sizes and versions, the version read once per path, a
/// denied open, a reused PID, a list that grows between the size query and the copy, a module
/// unloaded mid-read, an enumeration that fails, and the handle closed exactly once. Plus one read of
/// this test process through the real calls.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/Windows/WindowsProcessModules.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{
namespace
{

constexpr std::uint64_t START_TICKS = 0x01DB'0000'1234'5678ULL;
constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = START_TICKS};

struct FakeModule
{
    std::uintptr_t base = 0;
    DWORD size = 0;
    std::wstring path;
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
    int enumCalls = 0;
    int versionReads = 0;
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
    EXPECT_EQ(access, static_cast<DWORD>(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ));
    EXPECT_EQ(pid, static_cast<DWORD>(TARGET.pid));
    if (fake().openError != 0)
    {
        SetLastError(fake().openError);
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
    *code = fake().exitCode;
    return TRUE;
}

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
            fake().modules.push_back(
                {.base = base, .size = 0x1000, .path = L"C:\\Late\\late" + std::to_wstring(i) + L".dll", .unloaded = false});
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

const FakeModule* findModule(HMODULE module)
{
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto it = std::ranges::find(fake().modules, base, &FakeModule::base);
    return (it == fake().modules.end() || it->unloaded) ? nullptr : &*it;
}

DWORD WINAPI fakeGetModuleFileNameEx(HANDLE /*process*/, HMODULE module, LPWSTR buffer, DWORD size)
{
    const FakeModule* found = findModule(module);
    if (found == nullptr || found->path.size() >= size)
    {
        return 0;
    }
    std::ranges::copy(found->path, buffer);
    buffer[found->path.size()] = L'\0';
    return static_cast<DWORD>(found->path.size());
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

// Only paths under C:\Windows carry a version: 10.0.26100.<path length>.
DWORD WINAPI fakeGetFileVersionInfoSize(LPCWSTR path, LPDWORD /*handle*/)
{
    return std::wstring_view(path).starts_with(L"C:\\Windows") ? static_cast<DWORD>(sizeof(VS_FIXEDFILEINFO)) : 0;
}

BOOL WINAPI fakeGetFileVersionInfo(LPCWSTR path, DWORD /*handle*/, DWORD bytes, LPVOID data)
{
    ++fake().versionReads;
    VS_FIXEDFILEINFO fixed{};
    fixed.dwSignature = 0xFEEF04BD;
    fixed.dwFileVersionMS = 10U << 16U;
    fixed.dwFileVersionLS = (26100U << 16U) | static_cast<DWORD>(std::wcslen(path));
    EXPECT_GE(bytes, sizeof(fixed));
    std::memcpy(data, &fixed, sizeof(fixed));
    return TRUE;
}

BOOL WINAPI fakeVerQueryValue(LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, PUINT length)
{
    EXPECT_STREQ(subBlock, L"\\");
    *buffer = const_cast<LPVOID>(block); // NOLINT(cppcoreguidelines-pro-type-const-cast) - VerQueryValueW's out-parameter
    *length = sizeof(VS_FIXEDFILEINFO);
    return TRUE;
}

ProcessModuleFunctions fakeApi()
{
    return {.openProcess = &fakeOpenProcess,
            .closeHandle = &fakeCloseHandle,
            .getProcessTimes = &fakeGetProcessTimes,
            .getExitCodeProcess = &fakeGetExitCodeProcess,
            .enumProcessModulesEx = &fakeEnumProcessModulesEx,
            .getModuleFileNameEx = &fakeGetModuleFileNameEx,
            .getModuleInformation = &fakeGetModuleInformation,
            .getFileVersionInfoSize = &fakeGetFileVersionInfoSize,
            .getFileVersionInfo = &fakeGetFileVersionInfo,
            .verQueryValue = &fakeVerQueryValue};
}

class WindowsProcessModulesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = {};
        fake().modules = {{.base = 0x7FF6A0000000, .size = 0x52000, .path = L"C:\\Apps\\app.exe", .unloaded = false},
                          {.base = 0x7FF8A1B20000, .size = 0x1F8000, .path = L"C:\\Windows\\System32\\ntdll.dll", .unloaded = false},
                          {.base = 0x77A00000, .size = 0x1A000, .path = L"C:\\Windows\\SysWOW64\\ntdll.dll", .unloaded = false}};
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
    EXPECT_EQ(result.modules[1].version, std::optional(ModuleVersion{.major = 10, .minor = 0, .build = 26100, .revision = 29}));
    EXPECT_EQ(result.modules[2].baseAddress, 0x77A00000U); // a WOW64 process's 32-bit modules too
}

TEST_F(WindowsProcessModulesTest, ReadsEachPathsVersionOnce)
{
    WindowsProcessModulesReader reader(fakeApi());
    static_cast<void>(reader.readModules(TARGET));
    static_cast<void>(reader.readModules(TARGET));
    EXPECT_EQ(fake().versionReads, 2); // the two C:\Windows paths, once each
    EXPECT_EQ(reader.cachedVersionCount(), 3U);
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
}

} // namespace
} // namespace Platform::Windows
