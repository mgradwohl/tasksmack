/// @file test_WindowsStartupProbeSeams.cpp
/// @brief WindowsStartupProbe against a fake registry and fake known folders (#1722): Run and RunOnce
/// values and their failures, the Startup folders' listing (plain files, shortcuts read, broken and
/// with a missing target, desktop.ini and subfolders skipped, non-ASCII names), StartupApproved's
/// enabled and disabled states, and folders that can't be found or read. The registry is never read
/// or written; the "Startup folders" are directories the test makes under its own temp directory.

#include "Platform/IStartupProbe.h"
#include "Platform/Windows/ComPtr.h"
#include "Platform/Windows/WinString.h"
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
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
// clang-format on

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform
{
namespace
{

namespace Math = Windows::StartupMath;

constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* RUN_ONCE_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr REGSAM VIEW_MASK = KEY_WOW64_64KEY | KEY_WOW64_32KEY;

// "Café 日本", UTF-8 and UTF-16.
constexpr const char* UNICODE_NAME = "Caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC";
constexpr const wchar_t* UNICODE_NAME_WIDE = L"Caf\u00E9 \u65E5\u672C";

// 2024-01-02T03:04:05Z, when the fake StartupApproved records say an entry was disabled.
constexpr std::uint64_t DISABLED_AT = 1704164645;

struct FakeValue
{
    std::wstring name;
    DWORD type = REG_SZ;
    std::vector<std::uint8_t> data;
};

struct FakeKey
{
    HKEY root = nullptr;
    std::wstring subkey;
    REGSAM view = 0;
    std::vector<FakeValue> values;
    LSTATUS openError = ERROR_SUCCESS;
    bool infoFails = false;
    std::vector<DWORD> enumFails; ///< Value indexes RegEnumValueW fails for (ERROR_MORE_DATA).
};

/// The fake registry and known folders. Index + 1 into keys is a fake HKEY's value. Reset per test.
struct FakeSystem
{
    std::vector<FakeKey> keys;
    std::wstring userFolder;   ///< FOLDERID_Startup; the call fails when empty
    std::wstring commonFolder; ///< FOLDERID_CommonStartup; the call fails when empty
    int opened = 0;            ///< Open minus closed.
};

FakeSystem& fake()
{
    static FakeSystem instance;
    return instance;
}

[[nodiscard]] std::vector<std::uint8_t> stringData(const std::wstring& text)
{
    std::vector<std::uint8_t> data((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(data.data(), text.c_str(), data.size());
    return data;
}

/// A 12-byte StartupApproved record: enabled (0x02, no time) or disabled at DISABLED_AT.
[[nodiscard]] std::vector<std::uint8_t> approvedData(bool enabled)
{
    std::vector<std::uint8_t> data(12, 0);
    data[0] = enabled ? 0x02 : 0x03;
    if (!enabled)
    {
        const std::uint64_t fileTime = Math::FILETIME_UNIX_EPOCH + (DISABLED_AT * Math::FILETIME_TICKS_PER_SECOND);
        std::memcpy(&data[4], &fileTime, sizeof(fileTime));
    }
    return data;
}

[[nodiscard]] FakeKey& addKey(HKEY root, const wchar_t* subkey, REGSAM view = 0)
{
    FakeKey& key = fake().keys.emplace_back();
    key.root = root;
    key.subkey = subkey;
    key.view = view;
    return key;
}

[[nodiscard]] FakeKey& keyOf(HKEY key)
{
    return fake().keys.at(reinterpret_cast<std::size_t>(key) - 1);
}

LSTATUS WINAPI fakeOpen(HKEY root, LPCWSTR subkey, DWORD /*options*/, REGSAM access, PHKEY result)
{
    EXPECT_EQ(access & ~VIEW_MASK, static_cast<REGSAM>(KEY_READ)) << "the probe only reads";
    const auto& keys = fake().keys;
    const auto it = std::ranges::find_if(
        keys, [&](const FakeKey& key) { return key.root == root && key.subkey == subkey && key.view == (access & VIEW_MASK); });
    if (it == keys.end())
    {
        return ERROR_FILE_NOT_FOUND;
    }
    if (it->openError != ERROR_SUCCESS)
    {
        return it->openError;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr): a fake handle
    *result = reinterpret_cast<HKEY>(static_cast<std::size_t>(it - keys.begin()) + 1);
    ++fake().opened;
    return ERROR_SUCCESS;
}

LSTATUS WINAPI fakeQueryInfo(HKEY key,
                             LPWSTR /*cls*/,
                             LPDWORD /*clsChars*/,
                             LPDWORD /*reserved*/,
                             LPDWORD /*subkeys*/,
                             LPDWORD /*maxSubkeyChars*/,
                             LPDWORD /*maxClassChars*/,
                             LPDWORD valueCount,
                             LPDWORD maxNameChars,
                             LPDWORD maxDataBytes,
                             LPDWORD /*security*/,
                             PFILETIME /*lastWrite*/)
{
    const FakeKey& fakeKey = keyOf(key);
    if (fakeKey.infoFails)
    {
        return ERROR_ACCESS_DENIED;
    }
    *valueCount = static_cast<DWORD>(fakeKey.values.size());
    *maxNameChars = 0;
    *maxDataBytes = 0;
    for (const FakeValue& value : fakeKey.values)
    {
        *maxNameChars = std::max(*maxNameChars, static_cast<DWORD>(value.name.size()));
        *maxDataBytes = std::max(*maxDataBytes, static_cast<DWORD>(value.data.size()));
    }
    return ERROR_SUCCESS;
}

LSTATUS WINAPI
fakeEnumValue(HKEY key, DWORD index, LPWSTR name, LPDWORD nameChars, LPDWORD /*reserved*/, LPDWORD type, LPBYTE data, LPDWORD dataBytes)
{
    const FakeKey& fakeKey = keyOf(key);
    if (index >= fakeKey.values.size())
    {
        return ERROR_NO_MORE_ITEMS;
    }
    if (std::ranges::contains(fakeKey.enumFails, index))
    {
        return ERROR_MORE_DATA;
    }
    const FakeValue& value = fakeKey.values[index];
    if (*nameChars <= value.name.size() || *dataBytes < value.data.size())
    {
        ADD_FAILURE() << "the probe's buffers are sized from RegQueryInfoKeyW";
        return ERROR_MORE_DATA;
    }
    std::memcpy(name, value.name.c_str(), (value.name.size() + 1) * sizeof(wchar_t));
    *nameChars = static_cast<DWORD>(value.name.size());
    std::memcpy(data, value.data.data(), value.data.size());
    *dataBytes = static_cast<DWORD>(value.data.size());
    *type = value.type;
    return ERROR_SUCCESS;
}

LSTATUS WINAPI fakeClose(HKEY /*key*/)
{
    --fake().opened;
    return ERROR_SUCCESS;
}

HRESULT WINAPI fakeKnownFolder(const GUID& id, DWORD /*flags*/, HANDLE /*token*/, PWSTR* path)
{
    *path = nullptr;
    const std::wstring& folder = IsEqualGUID(id, FOLDERID_Startup) != FALSE ? fake().userFolder : fake().commonFolder;
    if (folder.empty())
    {
        return E_ACCESSDENIED;
    }
    const std::size_t bytes = (folder.size() + 1) * sizeof(wchar_t);
    *path = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    std::memcpy(*path, folder.c_str(), bytes);
    return S_OK;
}

[[nodiscard]] Windows::StartupProbeFunctions fakeFunctions()
{
    return {
        .openKey = &fakeOpen,
        .queryInfoKey = &fakeQueryInfo,
        .enumValue = &fakeEnumValue,
        .closeKey = &fakeClose,
        .knownFolderPath = &fakeKnownFolder,
    };
}

[[nodiscard]] std::string lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

[[nodiscard]] std::filesystem::path systemCmd()
{
    std::wstring directory(MAX_PATH, L'\0');
    directory.resize(GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size())));
    return std::filesystem::path(directory) / L"cmd.exe";
}

[[nodiscard]] const StartupEntry* find(const std::vector<StartupEntry>& entries, const std::string& name, StartupLocation location)
{
    const auto it = std::ranges::find_if(entries, [&](const StartupEntry& e) { return e.name == name && e.location == location; });
    return it == entries.end() ? nullptr : &*it;
}

class WindowsStartupProbeSeamsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeSystem{};
        m_Root = std::filesystem::temp_directory_path() / std::format("tasksmack-1722-{}", GetCurrentProcessId());
        std::error_code ignored;
        std::filesystem::remove_all(m_Root, ignored);
        std::filesystem::create_directories(m_Root);
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_ComInitialised = SUCCEEDED(init);
    }
    void TearDown() override
    {
        EXPECT_EQ(fake().opened, 0) << "every key opened is closed";
        std::error_code ignored;
        std::filesystem::remove_all(m_Root, ignored);
        if (m_ComInitialised)
        {
            CoUninitialize();
        }
    }

    [[nodiscard]] std::filesystem::path makeFolder(const wchar_t* name) const
    {
        const std::filesystem::path folder = m_Root / name;
        std::filesystem::create_directories(folder);
        return folder;
    }

    static void writeFile(const std::filesystem::path& path, std::string_view contents = "x")
    {
        std::ofstream(path, std::ios::binary) << contents;
    }

    static void writeShortcut(const std::filesystem::path& shortcut, const std::filesystem::path& target, const wchar_t* arguments)
    {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> file;
        ASSERT_TRUE(SUCCEEDED(CoCreateInstance(
            CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.releaseAndGetAddressOf()))));
        ASSERT_TRUE(SUCCEEDED(link->SetPath(target.c_str())));
        ASSERT_TRUE(SUCCEEDED(link->SetArguments(arguments)));
        ASSERT_TRUE(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.releaseAndGetAddressOf()))));
        ASSERT_TRUE(SUCCEEDED(file->Save(shortcut.c_str(), TRUE)));
    }

    std::filesystem::path m_Root;
    bool m_ComInitialised = false;
};

TEST_F(WindowsStartupProbeSeamsTest, NothingRegisteredListsNothing)
{
    fake().userFolder = makeFolder(L"user").wstring(); // an empty folder
    // The common folder can't be found (fakeKnownFolder fails), and no key exists.
    WindowsStartupProbe probe(fakeFunctions());
    EXPECT_TRUE(probe.enumerate().empty());
}

TEST_F(WindowsStartupProbeSeamsTest, ReadsRunValuesAndSkipsWhatIsNotAnEntry)
{
    const std::filesystem::path cmd = systemCmd();
    FakeKey& run = addKey(HKEY_CURRENT_USER, RUN_KEY);
    run.values = {
        {.name = L"Alpha", .type = REG_SZ, .data = stringData(std::format(L"\"{}\" /c exit", cmd.wstring()))},
        {.name = L"Gone", .type = REG_EXPAND_SZ, .data = stringData(L"%SystemDrive%\\TaskSmack-1722-missing\\gone.exe -q")},
        {.name = L"", .type = REG_SZ, .data = stringData(L"C:\\default.exe")}, // the default value isn't an entry
        {.name = L"Number", .type = REG_DWORD, .data = {1, 0, 0, 0}},          // nor is a non-string
        {.name = L"Skipped", .type = REG_SZ, .data = stringData(L"C:\\skipped.exe")},
        {.name = UNICODE_NAME_WIDE, .type = REG_SZ, .data = stringData(L"\"C:\\TaskSmack-1722\\caf\u00E9.exe\"")},
    };
    run.enumFails = {4}; // "Skipped" grew since RegQueryInfoKeyW: skipped this pass
    addKey(HKEY_CURRENT_USER, Math::APPROVED_RUN_KEY).values = {
        {.name = L"ALPHA", .type = REG_BINARY, .data = approvedData(false)},  // matched case-insensitively
        {.name = L"Gone", .type = REG_SZ, .data = stringData(L"not binary")}, // ignored
        {.name = UNICODE_NAME_WIDE, .type = REG_BINARY, .data = {}},          // empty: no record
    };
    addKey(HKEY_LOCAL_MACHINE, RUN_KEY, KEY_WOW64_64KEY).infoFails = true;
    addKey(HKEY_LOCAL_MACHINE, RUN_KEY, KEY_WOW64_32KEY).openError = ERROR_ACCESS_DENIED;
    addKey(HKEY_CURRENT_USER, RUN_ONCE_KEY).values = {
        {.name = L"Once", .type = REG_SZ, .data = stringData(L"C:\\once.exe")},
        {.name = L"Blank", .type = REG_SZ, .data = stringData(L"")},                               // no program
        {.name = L"Bare", .type = REG_SZ, .data = stringData(L"tasksmack-1722-no-such-tool --x")}, // not on the path
    };

    WindowsStartupProbe probe(fakeFunctions());
    const std::vector<StartupEntry> entries = probe.enumerate();
    ASSERT_EQ(entries.size(), 6U);

    const StartupEntry* alpha = find(entries, "Alpha", StartupLocation::RunUser);
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->scope, StartupScope::User);
    EXPECT_EQ(lower(alpha->executablePath), lower(WinString::wideToUtf8(cmd.wstring())));
    EXPECT_EQ(alpha->target, StartupTargetState::Present);
    EXPECT_FALSE(alpha->publisher.empty()); // cmd.exe's CompanyName
    EXPECT_FALSE(alpha->enabled);
    EXPECT_EQ(alpha->disabledAtUnixSeconds, DISABLED_AT);

    const StartupEntry* gone = find(entries, "Gone", StartupLocation::RunUser);
    ASSERT_NE(gone, nullptr);
    EXPECT_EQ(gone->target, StartupTargetState::Missing);
    EXPECT_FALSE(gone->executablePath.contains('%')) << "expanded: " << gone->executablePath;
    EXPECT_TRUE(gone->enabled);
    EXPECT_TRUE(gone->publisher.empty());

    const StartupEntry* unicode = find(entries, UNICODE_NAME, StartupLocation::RunUser);
    ASSERT_NE(unicode, nullptr) << "a non-ASCII value name round-trips as UTF-8";
    EXPECT_EQ(unicode->executablePath, "C:\\TaskSmack-1722\\caf\xC3\xA9.exe");
    EXPECT_TRUE(unicode->enabled);

    const StartupEntry* once = find(entries, "Once", StartupLocation::RunOnceUser);
    ASSERT_NE(once, nullptr);
    EXPECT_EQ(once->command, "C:\\once.exe");

    const StartupEntry* blank = find(entries, "Blank", StartupLocation::RunOnceUser);
    ASSERT_NE(blank, nullptr);
    EXPECT_TRUE(blank->executablePath.empty());
    EXPECT_EQ(blank->target, StartupTargetState::Unresolved);

    const StartupEntry* bare = find(entries, "Bare", StartupLocation::RunOnceUser);
    ASSERT_NE(bare, nullptr);
    EXPECT_EQ(bare->executablePath, "tasksmack-1722-no-such-tool") << "a name the search path doesn't find is kept as written";
    EXPECT_EQ(bare->target, StartupTargetState::Missing);

    // A second pass reads the cached publisher, and a value that's gone drops its cache entry.
    EXPECT_EQ(probe.enumerate().size(), 6U);
    fake().keys.front().values.erase(fake().keys.front().values.begin());
    EXPECT_EQ(probe.enumerate().size(), 5U);
}

TEST_F(WindowsStartupProbeSeamsTest, ListsTheStartupFolders)
{
    const std::filesystem::path user = makeFolder(L"user");
    fake().userFolder = user.wstring();
    writeFile(user / L"Tool.exe");
    writeFile(user / L"Site.url", "[InternetShortcut]\r\nURL=https://example.com/\r\n");
    writeFile(user / L"desktop.ini", "[.ShellClassInfo]\r\n"); // skipped
    writeFile(user / L"Broken.lnk", "not a shortcut");         // listed, its target unread
    std::filesystem::create_directories(user / L"Subfolder");  // skipped
    writeFile(user / (std::wstring(UNICODE_NAME_WIDE) + L".exe"));
    writeShortcut(user / L"Good.lnk", systemCmd(), L"/c exit");
    writeShortcut(user / L"Dangling.lnk", m_Root / L"no-such-target.exe", L"");
    addKey(HKEY_CURRENT_USER, Math::APPROVED_FOLDER_KEY).values = {
        {.name = L"Tool.exe", .type = REG_BINARY, .data = approvedData(false)},
        {.name = L"good.LNK", .type = REG_BINARY, .data = approvedData(true)},
        {.name = std::wstring(UNICODE_NAME_WIDE) + L".exe", .type = REG_BINARY, .data = approvedData(false)},
    };

    const std::filesystem::path common = makeFolder(L"common");
    fake().commonFolder = common.wstring();
    writeFile(common / L"Shared.bat", "@exit /b 0\r\n");
    addKey(HKEY_LOCAL_MACHINE, Math::APPROVED_FOLDER_KEY).openError = ERROR_ACCESS_DENIED; // all enabled, then

    WindowsStartupProbe probe(fakeFunctions());
    const std::vector<StartupEntry> entries = probe.enumerate();
    ASSERT_EQ(entries.size(), 7U);

    const StartupEntry* tool = find(entries, "Tool.exe", StartupLocation::StartupFolderUser);
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->scope, StartupScope::User);
    EXPECT_EQ(tool->sourcePath, WinString::wideToUtf8((user / L"Tool.exe").wstring()));
    EXPECT_EQ(tool->command, std::format("\"{}\"", tool->sourcePath));
    EXPECT_EQ(tool->executablePath, tool->sourcePath);
    EXPECT_EQ(tool->target, StartupTargetState::Present);
    EXPECT_FALSE(tool->enabled);
    EXPECT_EQ(tool->disabledAtUnixSeconds, DISABLED_AT);

    const StartupEntry* site = find(entries, "Site.url", StartupLocation::StartupFolderUser);
    ASSERT_NE(site, nullptr) << "an Internet shortcut is listed as the file it is";
    EXPECT_EQ(site->target, StartupTargetState::Present);
    EXPECT_TRUE(site->enabled);

    const StartupEntry* good = find(entries, "Good", StartupLocation::StartupFolderUser);
    ASSERT_NE(good, nullptr) << "a shortcut is named without its extension";
    EXPECT_EQ(lower(good->command), lower(std::format("\"{}\" /c exit", WinString::wideToUtf8(systemCmd().wstring()))));
    EXPECT_EQ(good->target, StartupTargetState::Present);
    EXPECT_TRUE(good->enabled);
    EXPECT_FALSE(good->publisher.empty());

    const StartupEntry* dangling = find(entries, "Dangling", StartupLocation::StartupFolderUser);
    ASSERT_NE(dangling, nullptr);
    EXPECT_EQ(dangling->target, StartupTargetState::Missing);

    const StartupEntry* broken = find(entries, "Broken", StartupLocation::StartupFolderUser);
    ASSERT_NE(broken, nullptr);
    EXPECT_EQ(broken->target, StartupTargetState::Unresolved);
    EXPECT_TRUE(broken->command.empty());
    EXPECT_TRUE(broken->executablePath.empty());

    const StartupEntry* unicode = find(entries, std::string(UNICODE_NAME) + ".exe", StartupLocation::StartupFolderUser);
    ASSERT_NE(unicode, nullptr) << "a non-ASCII file name round-trips as UTF-8";
    EXPECT_EQ(unicode->target, StartupTargetState::Present);
    EXPECT_EQ(WinString::utf8ToWide(unicode->sourcePath), (user / (std::wstring(UNICODE_NAME_WIDE) + L".exe")).wstring());
    EXPECT_FALSE(unicode->enabled);

    const StartupEntry* shared = find(entries, "Shared.bat", StartupLocation::StartupFolderCommon);
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->scope, StartupScope::Machine);
    EXPECT_TRUE(shared->enabled);

    EXPECT_EQ(find(entries, "desktop.ini", StartupLocation::StartupFolderUser), nullptr);
    EXPECT_EQ(find(entries, "Subfolder", StartupLocation::StartupFolderUser), nullptr);
}

TEST_F(WindowsStartupProbeSeamsTest, AFolderThatCantBeReadListsNothing)
{
    // The user folder's path names no directory (as a folder the user can't list fails the same way).
    fake().userFolder = (m_Root / L"no-such-folder").wstring();
    const std::filesystem::path common = makeFolder(L"common");
    fake().commonFolder = common.wstring();
    writeFile(common / L"Shared.cmd");

    WindowsStartupProbe probe(fakeFunctions());
    const std::vector<StartupEntry> entries = probe.enumerate();
    ASSERT_EQ(entries.size(), 1U) << "the other folder still lists";
    EXPECT_EQ(entries.front().name, "Shared.cmd");
    EXPECT_EQ(entries.front().location, StartupLocation::StartupFolderCommon);
}

} // namespace
} // namespace Platform
