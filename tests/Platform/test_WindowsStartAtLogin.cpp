/// @file test_WindowsStartAtLogin.cpp
/// @brief Start at sign-in on Windows (#3093, part 2): WindowsStartAtLogin against a fake registry reads the
/// Run key's TaskSmack value (none, text, not text), writes it as REG_SZ (creating the key), deletes it,
/// touches no other value, and reports each failure; this copy's path and whether it is under Program Files
/// come from the fakes too. One test runs against the real registry, on a scratch key under
/// HKCU\Software\TaskSmack.Tests that it deletes afterwards. The real Run key is never touched.

#ifdef _WIN32

#include "Domain/StartAtLogin.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/Windows/WindowsStartAtLogin.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <knownfolders.h>
#include <objbase.h>

namespace Platform
{
namespace
{

constexpr const wchar_t* SCRATCH_RUN_KEY = L"Software\\TaskSmack.Tests\\FakeRun";
constexpr const wchar_t* INSTALLED_EXE = L"C:\\Program Files\\TaskSmack\\TaskSmack.exe";

/// The fake registry: one key's values by name, under HKCU. Reset per test.
struct FakeRegistry
{
    std::map<std::wstring, std::pair<DWORD, std::vector<std::uint8_t>>> values;
    std::vector<std::wstring> touchedNames; ///< Every value name queried, set or deleted
    std::wstring module = INSTALLED_EXE;    ///< GetModuleFileNameW's answer; empty: it fails
    std::wstring programFiles = L"C:\\Program Files";
    LSTATUS openError = ERROR_SUCCESS;   ///< RegOpenKeyExW fails with this, when set
    LSTATUS createError = ERROR_SUCCESS; ///< RegCreateKeyExW fails with this, when set
    LSTATUS queryError = ERROR_SUCCESS;  ///< RegQueryValueExW fails with this, when set
    LSTATUS setError = ERROR_SUCCESS;    ///< RegSetValueExW fails with this, when set
    LSTATUS deleteError = ERROR_SUCCESS; ///< RegDeleteValueW fails with this, when set
    int opened = 0;                      ///< Open minus closed
    int growOnce = 0;                    ///< The value grows this many times between the size and the read
    bool keyExists = true;
};

FakeRegistry& fake()
{
    static FakeRegistry instance;
    return instance;
}

/// The fake key's handle.
[[nodiscard]] HKEY fakeKey() noexcept
{
    return reinterpret_cast<HKEY>(std::uintptr_t{0x5151}); // NOLINT(performance-no-int-to-ptr): a fake handle
}

LSTATUS WINAPI fakeOpen(HKEY root, LPCWSTR subkey, DWORD /*options*/, REGSAM /*access*/, PHKEY result)
{
    EXPECT_EQ(root, HKEY_CURRENT_USER) << "per-user only";
    EXPECT_EQ(std::wstring(subkey), SCRATCH_RUN_KEY);
    if (fake().openError != ERROR_SUCCESS)
    {
        return fake().openError;
    }
    if (!fake().keyExists)
    {
        return ERROR_FILE_NOT_FOUND;
    }
    *result = fakeKey();
    ++fake().opened;
    return ERROR_SUCCESS;
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
    EXPECT_EQ(root, HKEY_CURRENT_USER) << "per-user only";
    EXPECT_EQ(std::wstring(subkey), SCRATCH_RUN_KEY);
    EXPECT_EQ(access, static_cast<REGSAM>(KEY_SET_VALUE));
    if (fake().createError != ERROR_SUCCESS)
    {
        return fake().createError;
    }
    fake().keyExists = true;
    *result = fakeKey();
    ++fake().opened;
    return ERROR_SUCCESS;
}

LSTATUS WINAPI fakeQuery(HKEY key, LPCWSTR name, LPDWORD /*reserved*/, LPDWORD type, LPBYTE data, LPDWORD size)
{
    EXPECT_EQ(key, fakeKey());
    fake().touchedNames.emplace_back(name);
    if (fake().queryError != ERROR_SUCCESS)
    {
        return fake().queryError;
    }
    const auto it = fake().values.find(name);
    if (it == fake().values.end())
    {
        return ERROR_FILE_NOT_FOUND;
    }
    *type = it->second.first;
    auto& bytes = it->second.second;
    if (data != nullptr)
    {
        if (fake().growOnce > 0)
        {
            --fake().growOnce;
            bytes.insert(bytes.end() - 2, {'x', 0}); // one more character before the null
        }
        if (*size < bytes.size())
        {
            *size = static_cast<DWORD>(bytes.size());
            return ERROR_MORE_DATA;
        }
        std::memcpy(data, bytes.data(), bytes.size());
    }
    *size = static_cast<DWORD>(bytes.size());
    return ERROR_SUCCESS;
}

LSTATUS WINAPI fakeSet(HKEY key, LPCWSTR name, DWORD /*reserved*/, DWORD type, const BYTE* data, DWORD size)
{
    EXPECT_EQ(key, fakeKey());
    fake().touchedNames.emplace_back(name);
    if (fake().setError != ERROR_SUCCESS)
    {
        return fake().setError;
    }
    fake().values[name] = {type, std::vector<std::uint8_t>(data, data + size)};
    return ERROR_SUCCESS;
}

LSTATUS WINAPI fakeDelete(HKEY key, LPCWSTR name)
{
    EXPECT_EQ(key, fakeKey());
    fake().touchedNames.emplace_back(name);
    if (fake().deleteError != ERROR_SUCCESS)
    {
        return fake().deleteError;
    }
    return fake().values.erase(name) == 1 ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

LSTATUS WINAPI fakeClose(HKEY key)
{
    EXPECT_EQ(key, fakeKey());
    --fake().opened;
    return ERROR_SUCCESS;
}

DWORD WINAPI fakeModule(HMODULE /*module*/, LPWSTR path, DWORD size)
{
    const std::wstring& module = fake().module;
    if (module.empty())
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    if (module.size() >= size)
    {
        std::memcpy(path, module.data(), (size - 1) * sizeof(wchar_t));
        path[size - 1] = L'\0'; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): the caller's buffer
        return size;            // truncated, as GetModuleFileNameW reports it
    }
    std::memcpy(path, module.c_str(), (module.size() + 1) * sizeof(wchar_t));
    return static_cast<DWORD>(module.size());
}

HRESULT WINAPI fakeKnownFolder(REFKNOWNFOLDERID id, DWORD /*flags*/, HANDLE /*token*/, PWSTR* path)
{
    if (IsEqualGUID(id, FOLDERID_ProgramFiles) == FALSE || fake().programFiles.empty())
    {
        *path = nullptr;
        return E_FAIL;
    }
    const std::size_t bytes = (fake().programFiles.size() + 1) * sizeof(wchar_t);
    *path = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    std::memcpy(*path, fake().programFiles.c_str(), bytes);
    return S_OK;
}

/// A REG_SZ value's bytes for @p text, with its terminating null.
[[nodiscard]] std::vector<std::uint8_t> stringBytes(const std::wstring& text)
{
    std::vector<std::uint8_t> bytes((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(bytes.data(), text.c_str(), bytes.size());
    return bytes;
}

class WindowsStartAtLoginTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeRegistry{};
    }

    void TearDown() override
    {
        EXPECT_EQ(fake().opened, 0) << "every key opened is closed";
        for (const std::wstring& name : fake().touchedNames)
        {
            EXPECT_EQ(name, L"TaskSmack") << "no other value is touched";
        }
    }

    [[nodiscard]] static WindowsStartAtLogin entry()
    {
        return WindowsStartAtLogin(SCRATCH_RUN_KEY,
                                   L"TaskSmack",
                                   Windows::StartAtLoginFunctions{.openKey = &fakeOpen,
                                                                  .createKey = &fakeCreate,
                                                                  .queryValue = &fakeQuery,
                                                                  .setValue = &fakeSet,
                                                                  .deleteValue = &fakeDelete,
                                                                  .closeKey = &fakeClose,
                                                                  .moduleFileName = &fakeModule,
                                                                  .knownFolderPath = &fakeKnownFolder});
    }
};

TEST_F(WindowsStartAtLoginTest, TheDefaultsAreTheRunKeysTaskSmackValue)
{
    EXPECT_EQ(std::wstring(Windows::RUN_KEY), L"Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    EXPECT_EQ(std::wstring(Windows::RUN_VALUE_NAME), L"TaskSmack");
}

TEST_F(WindowsStartAtLoginTest, NoValueReadsAsNoEntryWithThisCopysPath)
{
    auto read = entry().read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->command, std::nullopt);
    EXPECT_EQ(read->executable, R"(C:\Program Files\TaskSmack\TaskSmack.exe)");
    EXPECT_TRUE(read->installed);
    EXPECT_TRUE(read->caseInsensitivePaths);
}

TEST_F(WindowsStartAtLoginTest, NoRunKeyReadsAsNoEntry)
{
    fake().keyExists = false;
    auto read = entry().read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->command, std::nullopt);
}

TEST_F(WindowsStartAtLoginTest, ACopyOutsideProgramFilesIsNotInstalled)
{
    fake().module = L"D:\\Tools\\TaskSmack\\TaskSmack.exe";
    auto read = entry().read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_FALSE(read->installed);

    fake().module = INSTALLED_EXE;
    fake().programFiles.clear(); // the known folder can't be read
    read = entry().read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_FALSE(read->installed);
}

TEST_F(WindowsStartAtLoginTest, ALongPathIsReadWhole)
{
    fake().module = L"C:\\" + std::wstring(400, L'a') + L"\\TaskSmack.exe";
    auto read = entry().read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->executable.size(), 3 + 400 + 14);
}

TEST_F(WindowsStartAtLoginTest, WriteThenReadRoundTripsAQuotedPathWithSpaces)
{
    fake().keyExists = false; // a new profile with no Run key yet: it is created
    const std::string command = Domain::StartAtLogin::runCommand(R"(C:\Program Files\TaskSmack\TaskSmack.exe)");
    ASSERT_TRUE(entry().write(command).has_value());
    const auto& stored = fake().values.at(L"TaskSmack");
    EXPECT_EQ(stored.first, static_cast<DWORD>(REG_SZ));
    EXPECT_EQ(stored.second, stringBytes(L"\"C:\\Program Files\\TaskSmack\\TaskSmack.exe\" --minimized"));

    auto read = entry().read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->command, std::optional<std::string>(command));
    EXPECT_EQ(Domain::StartAtLogin::classify(read->command, read->executable, read->installed, read->caseInsensitivePaths).state,
              Domain::StartAtLogin::State::On);
}

TEST_F(WindowsStartAtLoginTest, AnExpandStringIsReadAndAValueThatIsntTextReadsAsEmpty)
{
    fake().values[L"TaskSmack"] = {REG_EXPAND_SZ, stringBytes(L"%LOCALAPPDATA%\\TaskSmack.exe")};
    auto read = entry().read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->command, std::optional<std::string>(R"(%LOCALAPPDATA%\TaskSmack.exe)"));

    fake().values[L"TaskSmack"] = {REG_DWORD, {1, 0, 0, 0}};
    read = entry().read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->command, std::optional<std::string>(std::string{}));
}

TEST_F(WindowsStartAtLoginTest, AValueThatGrowsWhileReadIsReadAgain)
{
    fake().values[L"TaskSmack"] = {REG_SZ, stringBytes(L"\"C:\\a.exe\"")};
    fake().growOnce = 1;
    auto read = entry().read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->command, std::optional<std::string>(R"("C:\a.exe"x)"));

    fake().growOnce = 5; // it never settles
    read = entry().read();
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), "couldn't read the Run value: it kept changing");
}

TEST_F(WindowsStartAtLoginTest, RemoveDeletesOnlyTheTaskSmackValue)
{
    fake().values[L"TaskSmack"] = {REG_SZ, stringBytes(L"\"C:\\x\\TaskSmack.exe\" --minimized")};
    fake().values[L"OneDrive"] = {REG_SZ, stringBytes(L"\"C:\\OneDrive.exe\" /background")};
    ASSERT_TRUE(entry().remove().has_value());
    EXPECT_FALSE(fake().values.contains(L"TaskSmack"));
    EXPECT_TRUE(fake().values.contains(L"OneDrive")) << "another program's value is left";

    // Nothing there to remove, or no Run key at all: done.
    EXPECT_TRUE(entry().remove().has_value());
    fake().keyExists = false;
    EXPECT_TRUE(entry().remove().has_value());
}

TEST_F(WindowsStartAtLoginTest, EachFailureSaysWhatFailed)
{
    fake().module.clear();
    auto read = entry().read();
    ASSERT_FALSE(read.has_value());
    EXPECT_TRUE(read.error().starts_with("couldn't read TaskSmack's own path: ")) << read.error();
    fake().module = INSTALLED_EXE;

    fake().openError = ERROR_ACCESS_DENIED;
    read = entry().read();
    ASSERT_FALSE(read.has_value());
    EXPECT_TRUE(read.error().starts_with("couldn't open the Run key: ")) << read.error();
    auto removed = entry().remove();
    ASSERT_FALSE(removed.has_value());
    EXPECT_TRUE(removed.error().starts_with("couldn't open the Run key: ")) << removed.error();
    fake().openError = ERROR_SUCCESS;

    fake().queryError = ERROR_ACCESS_DENIED;
    read = entry().read();
    ASSERT_FALSE(read.has_value());
    EXPECT_TRUE(read.error().starts_with("couldn't read the Run value: ")) << read.error();
    fake().queryError = ERROR_SUCCESS;

    fake().createError = ERROR_ACCESS_DENIED;
    auto written = entry().write("x");
    ASSERT_FALSE(written.has_value());
    EXPECT_TRUE(written.error().starts_with("couldn't open the Run key: ")) << written.error();
    fake().createError = ERROR_SUCCESS;

    fake().setError = ERROR_ACCESS_DENIED;
    written = entry().write("x");
    ASSERT_FALSE(written.has_value());
    EXPECT_TRUE(written.error().starts_with("couldn't write the Run value: ")) << written.error();
    EXPECT_FALSE(fake().values.contains(L"TaskSmack"));
    fake().setError = ERROR_SUCCESS;

    fake().values[L"TaskSmack"] = {REG_SZ, stringBytes(L"x")};
    fake().deleteError = ERROR_ACCESS_DENIED;
    removed = entry().remove();
    ASSERT_FALSE(removed.has_value());
    EXPECT_TRUE(removed.error().starts_with("couldn't delete the Run value: ")) << removed.error();
    EXPECT_TRUE(fake().values.contains(L"TaskSmack"));
}

// The real registry, on a scratch key of this test's own under HKCU\Software\TaskSmack.Tests: never the
// Run key. Deleted afterwards, whatever the test found.
TEST(WindowsStartAtLoginRealTest, AScratchKeyRoundTrips)
{
    const std::wstring parent = L"Software\\TaskSmack.Tests";
    const std::wstring scratch = std::format(L"{}\\StartAtLogin-{}", parent, GetCurrentProcessId());
    ASSERT_NE(scratch, std::wstring(Windows::RUN_KEY));
    static_cast<void>(RegDeleteTreeW(HKEY_CURRENT_USER, scratch.c_str())); // a crashed run's

    WindowsStartAtLogin real(scratch, Windows::RUN_VALUE_NAME);
    auto read = real.read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->command, std::nullopt) << "the key doesn't exist yet";
    EXPECT_FALSE(read->executable.empty());

    const std::string command = Domain::StartAtLogin::runCommand(read->executable);
    const auto written = real.write(command);
    EXPECT_TRUE(written.has_value()) << written.error();
    read = real.read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->command, std::optional<std::string>(command));
    EXPECT_EQ(Domain::StartAtLogin::classify(read->command, read->executable, read->installed, read->caseInsensitivePaths).state,
              Domain::StartAtLogin::State::On);

    const auto removed = real.remove();
    EXPECT_TRUE(removed.has_value()) << removed.error();
    read = real.read();
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->command, std::nullopt);
    EXPECT_TRUE(real.remove().has_value()) << "nothing left to remove";

    EXPECT_EQ(RegDeleteTreeW(HKEY_CURRENT_USER, scratch.c_str()), ERROR_SUCCESS);
    static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, parent.c_str())); // only when no other run's key is left
}

} // namespace
} // namespace Platform

#endif // _WIN32
