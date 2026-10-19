/// @file test_WindowsStartupActions.cpp
/// @brief WindowsStartupActions (#801, phase 2) against a fake registry: disabling and enabling write
/// the StartupApproved value under the key the entry's location and scope choose, keep unknown bytes,
/// map a denied HKLM write to "Requires administrator", and refuse RunOnce entries without touching
/// the registry; a packaged app's startup task gets its State and LastDisabledTime (#1873), never when a
/// policy sets it or its key is gone. The real registry is never written.

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Windows/WindowsConversions.h"
#include "Platform/Windows/WindowsStartupActions.h"
#include "Platform/Windows/WindowsStartupProbeMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

namespace Math = Windows::StartupMath;

// 2024-01-02T03:04:05Z, the fake clock's time.
constexpr std::uint64_t UNIX_SECONDS = 1704164645;
constexpr std::uint64_t NOW = Windows::FILETIME_UNIX_EPOCH + (UNIX_SECONDS * Windows::FILETIME_TICKS_PER_SECOND);

/// The fake registry: values by (key, name), the key being "HKCU\..." or "HKLM\...". Reset per test.
struct FakeRegistry
{
    LSTATUS createError = ERROR_SUCCESS; ///< RegCreateKeyExW fails with this for HKLM, when set.
    std::map<std::pair<std::wstring, std::wstring>, std::pair<DWORD, std::vector<std::uint8_t>>> values;
    std::vector<std::wstring> keys;     ///< Index + 1 is a fake HKEY's value.
    std::vector<std::wstring> existing; ///< Keys RegOpenKeyExW finds (#1873); every other open fails.
    int opened = 0;                     ///< Open minus closed.
    int creates = 0;
    int writes = 0;
};

FakeRegistry& fake()
{
    static FakeRegistry instance;
    return instance;
}

LSTATUS WINAPI fakeCreate(HKEY root,
                          LPCWSTR subkey,
                          DWORD /*reserved*/,
                          LPWSTR /*cls*/,
                          DWORD /*options*/,
                          REGSAM access,
                          LPSECURITY_ATTRIBUTES /*security*/,
                          PHKEY result,
                          LPDWORD /*disposition*/)
{
    ++fake().creates;
    EXPECT_EQ(access, static_cast<REGSAM>(KEY_QUERY_VALUE | KEY_SET_VALUE));
    const bool machine = root == HKEY_LOCAL_MACHINE;
    if (machine && fake().createError != ERROR_SUCCESS)
    {
        return fake().createError;
    }
    fake().keys.push_back(std::wstring(machine ? L"HKLM\\" : L"HKCU\\") + subkey);
    *result = reinterpret_cast<HKEY>(fake().keys.size()); // NOLINT(performance-no-int-to-ptr): a fake handle
    ++fake().opened;
    return ERROR_SUCCESS;
}

LSTATUS WINAPI fakeOpen(HKEY root, LPCWSTR subkey, DWORD /*options*/, REGSAM access, PHKEY result)
{
    EXPECT_EQ(access, static_cast<REGSAM>(KEY_QUERY_VALUE | KEY_SET_VALUE));
    std::wstring name = std::wstring(root == HKEY_LOCAL_MACHINE ? L"HKLM\\" : L"HKCU\\") + subkey;
    if (std::ranges::find(fake().existing, name) == fake().existing.end())
    {
        return ERROR_FILE_NOT_FOUND;
    }
    fake().keys.push_back(std::move(name));
    *result = reinterpret_cast<HKEY>(fake().keys.size()); // NOLINT(performance-no-int-to-ptr): a fake handle
    ++fake().opened;
    return ERROR_SUCCESS;
}

[[nodiscard]] const std::wstring& keyOf(HKEY key)
{
    return fake().keys.at(reinterpret_cast<std::size_t>(key) - 1);
}

LSTATUS WINAPI fakeQuery(HKEY key, LPCWSTR name, LPDWORD /*reserved*/, LPDWORD type, LPBYTE data, LPDWORD size)
{
    const auto it = fake().values.find({keyOf(key), name});
    if (it == fake().values.end())
    {
        return ERROR_FILE_NOT_FOUND;
    }
    *type = it->second.first;
    const auto& bytes = it->second.second;
    if (data != nullptr)
    {
        if (*size < bytes.size())
        {
            return ERROR_MORE_DATA;
        }
        std::memcpy(data, bytes.data(), bytes.size());
    }
    *size = static_cast<DWORD>(bytes.size());
    return ERROR_SUCCESS;
}

LSTATUS WINAPI fakeSet(HKEY key, LPCWSTR name, DWORD /*reserved*/, DWORD type, const BYTE* data, DWORD size)
{
    ++fake().writes;
    fake().values[{keyOf(key), name}] = {type, std::vector<std::uint8_t>(data, data + size)};
    return ERROR_SUCCESS;
}

LSTATUS WINAPI fakeClose(HKEY /*key*/)
{
    --fake().opened;
    return ERROR_SUCCESS;
}

void WINAPI fakeNow(LPFILETIME time)
{
    time->dwLowDateTime = static_cast<DWORD>(NOW & 0xFFFFFFFFU);
    time->dwHighDateTime = static_cast<DWORD>(NOW >> 32U);
}

class WindowsStartupActionsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeRegistry{};
    }
    void TearDown() override
    {
        EXPECT_EQ(fake().opened, 0) << "every key opened is closed";
    }

    [[nodiscard]] static WindowsStartupActions actions(bool elevated = false)
    {
        return WindowsStartupActions(elevated,
                                     {.createKey = &fakeCreate,
                                      .openKey = &fakeOpen,
                                      .queryValue = &fakeQuery,
                                      .setValue = &fakeSet,
                                      .closeKey = &fakeClose,
                                      .now = &fakeNow});
    }

    /// An entry registered as @p name, shown by another name (its program's, #1873): the actions
    /// must record its state under the registered one.
    [[nodiscard]] static StartupEntry entry(const char* name, StartupLocation location)
    {
        StartupEntry e;
        e.name = std::string("Shown ") + name;
        e.registrationName = name;
        e.location = location;
        e.scope = Math::scopeOf(location);
        return e;
    }

    /// The value recorded for (`key`, `name`), parsed; nullopt when none was written.
    [[nodiscard]] static std::optional<Math::ApprovedState> recorded(const std::wstring& key, const std::wstring& name)
    {
        const auto it = fake().values.find({key, name});
        if (it == fake().values.end())
        {
            return std::nullopt;
        }
        EXPECT_EQ(it->second.first, static_cast<DWORD>(REG_BINARY));
        return Math::parseStartupApproved(it->second.second);
    }
};

/// The fake registry's key names: the root, then the StartupApproved subkey.
[[nodiscard]] std::wstring hkcu(const wchar_t* subkey)
{
    return std::wstring(L"HKCU\\") + subkey;
}
[[nodiscard]] std::wstring hklm(const wchar_t* subkey)
{
    return std::wstring(L"HKLM\\") + subkey;
}

TEST_F(WindowsStartupActionsTest, DisableThenEnableAUserRunEntry)
{
    auto subject = actions();
    EXPECT_TRUE(subject.capabilities().canSetEnabled);
    EXPECT_FALSE(subject.capabilities().elevated);

    const auto oneDrive = entry("OneDrive", StartupLocation::RunUser);
    EXPECT_TRUE(subject.setEnabled(oneDrive, false).ok);
    EXPECT_EQ(recorded(hkcu(Math::APPROVED_RUN_KEY), L"OneDrive"),
              (Math::ApprovedState{.enabled = false, .disabledAtUnixSeconds = UNIX_SECONDS}));
    EXPECT_EQ(fake().values.at({hkcu(Math::APPROVED_RUN_KEY), L"OneDrive"}).second.size(), 12U);

    EXPECT_TRUE(subject.setEnabled(oneDrive, true).ok);
    EXPECT_EQ(recorded(hkcu(Math::APPROVED_RUN_KEY), L"OneDrive"), (Math::ApprovedState{.enabled = true, .disabledAtUnixSeconds = 0}));
}

TEST_F(WindowsStartupActionsTest, KeepsTheBytesOfAnExistingValue)
{
    fake().values[{hkcu(Math::APPROVED_RUN_KEY), L"Tool"}] = {REG_BINARY, {0x06, 0xAA, 0xBB, 0xCC, 0, 0, 0, 0, 0, 0, 0, 0, 0xDD}};
    auto subject = actions();
    ASSERT_TRUE(subject.setEnabled(entry("Tool", StartupLocation::RunUser), false).ok);
    const auto& bytes = fake().values.at({hkcu(Math::APPROVED_RUN_KEY), L"Tool"}).second;
    ASSERT_EQ(bytes.size(), 13U);
    EXPECT_EQ(bytes[0], 0x07);
    EXPECT_EQ(bytes[1], 0xAA);
    EXPECT_EQ(bytes[12], 0xDD);
}

TEST_F(WindowsStartupActionsTest, StartupFolderShortcutIsRecordedByItsFileName)
{
    auto shortcut = entry("Tool", StartupLocation::StartupFolderUser);
    shortcut.sourcePath = R"(C:\Users\me\Startup\Tool.lnk)";
    auto subject = actions();
    ASSERT_TRUE(subject.setEnabled(shortcut, false).ok);
    EXPECT_TRUE(recorded(hkcu(Math::APPROVED_FOLDER_KEY), L"Tool.lnk").has_value());
}

TEST_F(WindowsStartupActionsTest, MachineEntryWritesHklmWhenAllowed)
{
    auto subject = actions(true);
    EXPECT_TRUE(subject.capabilities().elevated);
    ASSERT_TRUE(subject.setEnabled(entry("Helper", StartupLocation::RunMachine32), false).ok);
    EXPECT_TRUE(recorded(hklm(Math::APPROVED_RUN32_KEY), L"Helper").has_value());
}

TEST_F(WindowsStartupActionsTest, DeniedMachineEntryRequiresAdministrator)
{
    fake().createError = ERROR_ACCESS_DENIED;
    auto subject = actions();
    const StartupActionResult result = subject.setEnabled(entry("Helper", StartupLocation::RunMachine), false);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "Requires administrator");
    EXPECT_EQ(fake().writes, 0);
}

TEST_F(WindowsStartupActionsTest, RunOnceIsUnsupportedAndNeverTouchesTheRegistry)
{
    auto subject = actions(true);
    for (const StartupLocation location : {StartupLocation::RunOnceUser, StartupLocation::RunOnceMachine})
    {
        const StartupActionResult result = subject.setEnabled(entry("Setup", location), false);
        EXPECT_FALSE(result.ok);
        EXPECT_FALSE(result.message.empty());
    }
    EXPECT_EQ(fake().creates, 0);
    EXPECT_EQ(fake().writes, 0);
}

// Packaged apps' startup tasks (#1873): State and LastDisabledTime as Settings > Apps > Startup writes them.

constexpr const wchar_t* MUSIC_FAMILY = L"Contoso.Music_abc123";

[[nodiscard]] std::wstring packagedTaskKey(const wchar_t* family, const wchar_t* taskId)
{
    return hkcu(Math::PACKAGED_TASKS_KEY) + L"\\" + family + L"\\" + taskId;
}

[[nodiscard]] std::vector<std::uint8_t> dword(std::uint32_t value)
{
    std::vector<std::uint8_t> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> qword(std::uint64_t value)
{
    std::vector<std::uint8_t> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

[[nodiscard]] StartupEntry packagedEntry(const char* taskId)
{
    StartupEntry e;
    e.name = "Contoso Music";
    e.registrationName = taskId;
    e.packageFamily = "Contoso.Music_abc123";
    e.location = StartupLocation::PackagedApp;
    e.scope = StartupScope::User;
    return e;
}

TEST_F(WindowsStartupActionsTest, DisableThenEnableAPackagedStartupTask)
{
    const std::wstring key = packagedTaskKey(MUSIC_FAMILY, L"Music");
    fake().existing.push_back(key);
    fake().values[{key, L"State"}] = {REG_DWORD, dword(2)};
    fake().values[{key, L"UserEnabledStartupOnce"}] = {REG_DWORD, dword(1)};
    auto subject = actions();

    ASSERT_TRUE(subject.setEnabled(packagedEntry("Music"), false).ok);
    EXPECT_EQ(fake().values.at({key, L"State"}), (std::pair<DWORD, std::vector<std::uint8_t>>{REG_DWORD, dword(1)})) << "DisabledByUser";
    EXPECT_EQ(fake().values.at({key, L"LastDisabledTime"}), (std::pair<DWORD, std::vector<std::uint8_t>>{REG_QWORD, qword(UNIX_SECONDS)}))
        << "Unix seconds, as Settings writes it";

    ASSERT_TRUE(subject.setEnabled(packagedEntry("Music"), true).ok);
    EXPECT_EQ(fake().values.at({key, L"State"}), (std::pair<DWORD, std::vector<std::uint8_t>>{REG_DWORD, dword(2)})) << "Enabled";
    EXPECT_EQ(fake().values.at({key, L"LastDisabledTime"}).second, qword(UNIX_SECONDS)) << "kept on enable, as Settings keeps it";
    EXPECT_EQ(fake().values.at({key, L"UserEnabledStartupOnce"}).second, dword(1)) << "never written";
    EXPECT_EQ(fake().values.size(), 3U);
    EXPECT_EQ(fake().creates, 0) << "the task's key is opened, never created";
}

TEST_F(WindowsStartupActionsTest, APackagedTaskSetByPolicyIsRefused)
{
    for (const std::uint32_t state : {3U, 4U})
    {
        const std::wstring key = packagedTaskKey(MUSIC_FAMILY, L"Managed");
        fake().existing = {key};
        fake().values[{key, L"State"}] = {REG_DWORD, dword(state)};
        auto subject = actions(true);
        const StartupActionResult result = subject.setEnabled(packagedEntry("Managed"), state == 3);
        EXPECT_FALSE(result.ok);
        EXPECT_TRUE(result.message.contains("policy")) << result.message;
    }
    EXPECT_EQ(fake().writes, 0);
}

TEST_F(WindowsStartupActionsTest, APackagedTaskThatCantBeFoundOrReadIsNotWritten)
{
    auto subject = actions();
    // No key: the package was removed since the list was read.
    const StartupActionResult gone = subject.setEnabled(packagedEntry("Gone"), false);
    EXPECT_FALSE(gone.ok);
    EXPECT_EQ(gone.message, "The startup task is no longer registered");

    // A key without a DWORD State.
    const std::wstring key = packagedTaskKey(MUSIC_FAMILY, L"Odd");
    fake().existing.push_back(key);
    fake().values[{key, L"State"}] = {REG_SZ, {'2', 0}};
    EXPECT_FALSE(subject.setEnabled(packagedEntry("Odd"), false).ok);

    // No family or task id to find it by.
    auto unnamed = packagedEntry("Odd");
    unnamed.packageFamily.clear();
    EXPECT_FALSE(subject.setEnabled(unnamed, true).ok);

    EXPECT_EQ(fake().writes, 0);
    EXPECT_EQ(fake().creates, 0);
}

} // namespace
} // namespace Platform
