/// @file test_WindowsStartupProbe.cpp
/// @brief The real WindowsStartupProbe (#801): it enumerates without failing (the list may be empty
/// on a clean machine) and every entry carries its location; environment-variable expansion and
/// executable resolution through the real environment; reading a shortcut; the registry key owner.

#include "Platform/IStartupProbe.h"
#include "Platform/Windows/ComPtr.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsStartupProbe.h"
#include "Platform/Windows/WindowsStartupProbeMath.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <shobjidl.h>
// clang-format on

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>

namespace Platform
{
namespace
{

[[nodiscard]] std::string lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

[[nodiscard]] std::string environmentVariable(const char* name)
{
    std::wstring value(MAX_PATH, L'\0');
    const DWORD length = GetEnvironmentVariableW(WinString::utf8ToWide(name).c_str(), value.data(), static_cast<DWORD>(value.size()));
    value.resize(length < value.size() ? length : 0);
    return WinString::wideToUtf8(value);
}
TEST(WindowsStartupProbeTest, EnumeratesWithoutFailing)
{
    WindowsStartupProbe probe;
    const StartupCapabilities capabilities = probe.capabilities();
    ASSERT_TRUE(capabilities.canEnumerate);
    EXPECT_TRUE(capabilities.unavailableReason.empty());

    const auto entries = probe.enumerate(); // may legitimately be empty
    for (const StartupEntry& entry : entries)
    {
        EXPECT_FALSE(entry.name.empty());
        EXPECT_EQ(entry.scope, Windows::StartupMath::scopeOf(entry.location)) << entry.name;
        EXPECT_LE(entry.location, StartupLocation::StartupFolderCommon) << entry.name;
        if (entry.enabled)
        {
            EXPECT_EQ(entry.disabledAtUnixSeconds, 0U) << entry.name;
        }
        if (entry.target != StartupTargetState::Unresolved)
        {
            EXPECT_FALSE(entry.executablePath.empty()) << entry.name;
        }
    }

    // A second pass (publishers now cached) lists the same number of entries.
    EXPECT_EQ(probe.enumerate().size(), entries.size());
}

TEST(WindowsStartupProbeTest, ExpandsEnvironmentVariables)
{
    const std::string systemRoot = environmentVariable("SystemRoot");
    ASSERT_FALSE(systemRoot.empty());
    EXPECT_EQ(lower(Windows::expandEnvironmentStrings(R"(%SystemRoot%\System32\cmd.exe)")), lower(systemRoot + R"(\System32\cmd.exe)"));
    EXPECT_EQ(Windows::expandEnvironmentStrings("no variables"), "no variables");
    EXPECT_EQ(Windows::expandEnvironmentStrings("%TASKSMACK_NOT_A_VARIABLE%"), "%TASKSMACK_NOT_A_VARIABLE%");
}

TEST(WindowsStartupProbeTest, ResolvesTheExecutableOfACommandLine)
{
    const std::string systemRoot = lower(environmentVariable("SystemRoot"));
    ASSERT_FALSE(systemRoot.empty());
    EXPECT_EQ(lower(Windows::resolveStartupExecutable(R"("%SystemRoot%\System32\cmd.exe" /c exit)")), systemRoot + R"(\system32\cmd.exe)");
    EXPECT_EQ(lower(Windows::resolveStartupExecutable(R"(%SystemRoot%\System32\cmd.exe /c exit)")), systemRoot + R"(\system32\cmd.exe)");
    // A bare name is found on the search path, as the shell would.
    EXPECT_EQ(lower(Windows::resolveStartupExecutable("cmd.exe /c exit")), systemRoot + R"(\system32\cmd.exe)");
    EXPECT_EQ(Windows::resolveStartupExecutable(""), "");
}

TEST(WindowsStartupProbeTest, ReadsAShortcutsTargetAndArguments)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ASSERT_TRUE(SUCCEEDED(init) || init == RPC_E_CHANGED_MODE);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("tasksmack-801-{}", GetCurrentProcessId());
    std::filesystem::create_directories(dir);
    const std::filesystem::path shortcut = dir / "Test.lnk";
    const std::string target = environmentVariable("SystemRoot") + R"(\System32\cmd.exe)";
    {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> file;
        ASSERT_TRUE(SUCCEEDED(CoCreateInstance(
            CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.releaseAndGetAddressOf()))));
        ASSERT_TRUE(SUCCEEDED(link->SetPath(std::filesystem::path(target).c_str())));
        ASSERT_TRUE(SUCCEEDED(link->SetArguments(L"/c exit")));
        ASSERT_TRUE(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.releaseAndGetAddressOf()))));
        ASSERT_TRUE(SUCCEEDED(file->Save(shortcut.c_str(), TRUE)));
    }

    EXPECT_EQ(Windows::readShortcutCommand(shortcut).transform(lower), std::optional(lower(std::format(R"("{}" /c exit)", target))));
    EXPECT_EQ(Windows::readShortcutCommand(dir / "missing.lnk"), std::nullopt);

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    if (SUCCEEDED(init))
    {
        CoUninitialize();
    }
}

TEST(WindowsStartupProbeTest, RegistryKeyOwnerClosesItsKey)
{
    Windows::UniqueRegistryKey key;
    EXPECT_FALSE(key);
    ASSERT_EQ(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_READ, key.put()), ERROR_SUCCESS);
    EXPECT_TRUE(key);
    key.reset();
    EXPECT_FALSE(key);
    // A failed open leaves no key to close.
    EXPECT_NE(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TaskSmack-no-such-key-801", 0, KEY_READ, key.put()), ERROR_SUCCESS);
    EXPECT_FALSE(key);
}

} // namespace
} // namespace Platform
