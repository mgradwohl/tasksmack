/// @file test_WindowsStartupProbeSeams.cpp
/// @brief WindowsStartupProbe against a fake registry and fake known folders (#1722): Run and RunOnce
/// values and their failures, the Startup folders' listing (plain files, shortcuts read, broken and
/// with a missing target, desktop.ini and subfolders skipped, non-ASCII names), StartupApproved's
/// enabled and disabled states, folders that can't be found or read, and packaged apps' startup tasks
/// (#1798: the manifest parser, recorded states, installed packages and their names, and packages that
/// can't be listed or read). The registry is never read
/// or written; the "Startup folders" are directories the test makes under its own temp directory.

#include "Platform/IStartupProbe.h"
#include "Platform/Windows/ComPtr.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsConversions.h"
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
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
    std::vector<DWORD> enumFails;    ///< Value indexes RegEnumValueW fails for (ERROR_MORE_DATA).
    std::vector<DWORD> enumKeyFails; ///< Subkey indexes RegEnumKeyExW fails for (ERROR_MORE_DATA, #3221).
    bool opensToNoHandle = false;    ///< RegOpenKeyExW succeeds but gives no handle (#3221).
};

/// One installed package (#1798): GetPackagesByPackageFamily lists fullName under family, and
/// GetPackagePathByFullName gives folder (fails when empty).
struct FakePackage
{
    std::wstring family;
    std::wstring fullName;
    std::wstring folder;
    bool pathFailsAfterSizing = false; ///< GetPackagePathByFullName sizes the folder, then fails.
    bool pathSizedEmpty = false;       ///< GetPackagePathByFullName sizes the folder as no characters (#3221).
    bool listedWithoutName = false;    ///< GetPackagesByPackageFamily lists it with a null name (#3221).
};

/// The fake registry, known folders and packages. Index + 1 into keys is a fake HKEY's value. Reset per test.
struct FakeSystem
{
    std::vector<FakeKey> keys;
    std::vector<FakePackage> packages;
    std::vector<std::pair<std::wstring, std::wstring>> resources; ///< SHLoadIndirectString source and text.
    int packageLookups = 0;                                       ///< GetPackagePathByFullName calls.
    std::wstring familyFailingAfterSizing;                        ///< GetPackagesByPackageFamily sizes it, then fails.
    std::wstring familySizedEmpty;                                ///< GetPackagesByPackageFamily asks for a buffer for no packages (#3221).
    bool knownFolderWithoutPath = false;                          ///< SHGetKnownFolderPath succeeds without a path (#3221).
    std::wstring userFolder;                                      ///< FOLDERID_Startup; the call fails when empty
    std::wstring commonFolder;                                    ///< FOLDERID_CommonStartup; the call fails when empty
    int opened = 0;                                               ///< Open minus closed.
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
        const std::uint64_t fileTime = Windows::FILETIME_UNIX_EPOCH + (DISABLED_AT * Windows::FILETIME_TICKS_PER_SECOND);
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
    if (it->opensToNoHandle)
    {
        *result = nullptr;
        return ERROR_SUCCESS;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr): a fake handle
    *result = reinterpret_cast<HKEY>(static_cast<std::size_t>(it - keys.begin()) + 1);
    ++fake().opened;
    return ERROR_SUCCESS;
}

/// The names of @p parent's direct subkeys: the fake keys under it in the same root and view, in the
/// order they were added.
[[nodiscard]] std::vector<std::wstring> childrenOf(const FakeKey& parent)
{
    std::vector<std::wstring> children;
    const std::wstring prefix = parent.subkey + L"\\";
    for (const FakeKey& key : fake().keys)
    {
        if (key.root == parent.root && key.view == parent.view && key.subkey.starts_with(prefix) &&
            !key.subkey.substr(prefix.size()).contains(L'\\'))
        {
            children.push_back(key.subkey.substr(prefix.size()));
        }
    }
    return children;
}

LSTATUS WINAPI fakeQueryInfo(HKEY key,
                             LPWSTR /*cls*/,
                             LPDWORD /*clsChars*/,
                             LPDWORD /*reserved*/,
                             LPDWORD subkeys,
                             LPDWORD maxSubkeyChars,
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
    if (subkeys != nullptr && maxSubkeyChars != nullptr)
    {
        const std::vector<std::wstring> children = childrenOf(fakeKey);
        *subkeys = static_cast<DWORD>(children.size());
        *maxSubkeyChars = 0;
        for (const std::wstring& child : children)
        {
            *maxSubkeyChars = std::max(*maxSubkeyChars, static_cast<DWORD>(child.size()));
        }
    }
    if (valueCount == nullptr)
    {
        return ERROR_SUCCESS;
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
    if (fake().knownFolderWithoutPath)
    {
        return S_OK;
    }
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

LSTATUS WINAPI fakeEnumKey(HKEY key,
                           DWORD index,
                           LPWSTR name,
                           LPDWORD nameChars,
                           LPDWORD /*reserved*/,
                           LPWSTR /*cls*/,
                           LPDWORD /*clsChars*/,
                           PFILETIME /*lastWrite*/)
{
    const std::vector<std::wstring> children = childrenOf(keyOf(key));
    if (index >= children.size())
    {
        return ERROR_NO_MORE_ITEMS;
    }
    if (std::ranges::contains(keyOf(key).enumKeyFails, index))
    {
        return ERROR_MORE_DATA;
    }
    const std::wstring& child = children[index];
    if (*nameChars <= child.size())
    {
        ADD_FAILURE() << "the probe's buffer is sized from RegQueryInfoKeyW";
        return ERROR_MORE_DATA;
    }
    std::memcpy(name, child.c_str(), (child.size() + 1) * sizeof(wchar_t));
    *nameChars = static_cast<DWORD>(child.size());
    return ERROR_SUCCESS;
}

/// GetPackagesByPackageFamily's contract: sizes first (ERROR_INSUFFICIENT_BUFFER), then the names,
/// each pointing into the caller's buffer.
LONG WINAPI fakePackagesByFamily(PCWSTR family, UINT32* count, PWSTR* fullNames, UINT32* bufferChars, WCHAR* buffer)
{
    std::vector<const FakePackage*> matches;
    for (const FakePackage& package : fake().packages)
    {
        if (package.family == family)
        {
            matches.push_back(&package);
        }
    }
    UINT32 needed = 0;
    for (const FakePackage* package : matches)
    {
        needed += static_cast<UINT32>(package->fullName.size() + 1);
    }
    if (fake().familySizedEmpty == family)
    {
        *count = 0;
        *bufferChars = 0;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (matches.empty())
    {
        *count = 0;
        *bufferChars = 0;
        return ERROR_SUCCESS;
    }
    if (fullNames == nullptr || buffer == nullptr || *count < matches.size() || *bufferChars < needed)
    {
        *count = static_cast<UINT32>(matches.size());
        *bufferChars = needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (fake().familyFailingAfterSizing == family)
    {
        return ERROR_ACCESS_DENIED;
    }
    WCHAR* next = buffer;
    std::size_t index = 0;
    while (index < matches.size())
    {
        const std::wstring& name = matches[index]->fullName;
        std::memcpy(next, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        fullNames[index] = matches[index]->listedWithoutName ? nullptr : next;
        next += name.size() + 1;
        ++index;
    }
    *count = static_cast<UINT32>(matches.size());
    return ERROR_SUCCESS;
}

LONG WINAPI fakePackagePath(PCWSTR fullName, UINT32* pathChars, PWSTR path)
{
    ++fake().packageLookups;
    const auto it = std::ranges::find_if(fake().packages, [&](const FakePackage& p) { return p.fullName == fullName; });
    if (it == fake().packages.end() || it->folder.empty())
    {
        return ERROR_NOT_FOUND;
    }
    if (it->pathSizedEmpty)
    {
        *pathChars = 0;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    const auto needed = static_cast<UINT32>(it->folder.size() + 1);
    if (path == nullptr || *pathChars < needed)
    {
        *pathChars = needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (it->pathFailsAfterSizing)
    {
        return ERROR_ACCESS_DENIED;
    }
    std::memcpy(path, it->folder.c_str(), needed * sizeof(wchar_t));
    *pathChars = needed;
    return ERROR_SUCCESS;
}

HRESULT WINAPI fakeLoadIndirectString(PCWSTR source, PWSTR text, UINT textChars, void** /*reserved*/)
{
    const auto it = std::ranges::find_if(fake().resources, [&](const auto& resource) { return resource.first == source; });
    if (it == fake().resources.end() || it->second.size() >= textChars)
    {
        return E_FAIL;
    }
    std::memcpy(text, it->second.c_str(), (it->second.size() + 1) * sizeof(wchar_t));
    return S_OK;
}

[[nodiscard]] Windows::StartupProbeFunctions fakeFunctions()
{
    return {
        .openKey = &fakeOpen,
        .queryInfoKey = &fakeQueryInfo,
        .enumValue = &fakeEnumValue,
        .enumKey = &fakeEnumKey,
        .closeKey = &fakeClose,
        .knownFolderPath = &fakeKnownFolder,
        .packagesByFamily = &fakePackagesByFamily,
        .packagePath = &fakePackagePath,
        .loadIndirectString = &fakeLoadIndirectString,
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

[[nodiscard]] const StartupEntry*
find(const std::vector<StartupEntry>& entries, const std::string& registrationName, StartupLocation location)
{
    const auto it = std::ranges::find_if(
        entries, [&](const StartupEntry& e) { return e.registrationName == registrationName && e.location == location; });
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
        {.name = L"Cleanup", .type = REG_SZ, .data = stringData(std::format(L"\"{}\" /c exit", cmd.wstring()))},
    };
    addKey(HKEY_LOCAL_MACHINE, RUN_ONCE_KEY, KEY_WOW64_64KEY).values = {
        {.name = L"MachineCleanup", .type = REG_SZ, .data = stringData(std::format(L"\"{}\" /c exit", cmd.wstring()))},
    };

    WindowsStartupProbe probe(fakeFunctions());
    const std::vector<StartupEntry> entries = probe.enumerate();
    ASSERT_EQ(entries.size(), 8U);

    const StartupEntry* alpha = find(entries, "Alpha", StartupLocation::RunUser);
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->scope, StartupScope::User);
    EXPECT_EQ(lower(alpha->executablePath), lower(WinString::wideToUtf8(cmd.wstring())));
    EXPECT_EQ(alpha->target, StartupTargetState::Present);
    EXPECT_FALSE(alpha->publisher.empty()); // cmd.exe's CompanyName
    // #1873: shown by cmd.exe's FileDescription ("Windows Command Processor", localised), not the value name.
    EXPECT_FALSE(alpha->name.empty());
    EXPECT_NE(alpha->name, "Alpha");
    EXPECT_NE(lower(alpha->name), "cmd.exe") << "the description, not the file name";
    EXPECT_EQ(alpha->programName, alpha->name);
    EXPECT_FALSE(alpha->enabled);
    EXPECT_EQ(alpha->disabledAtUnixSeconds, DISABLED_AT);

    const StartupEntry* gone = find(entries, "Gone", StartupLocation::RunUser);
    ASSERT_NE(gone, nullptr);
    EXPECT_EQ(gone->name, "gone.exe") << "a missing program is named by its file name";
    EXPECT_EQ(gone->target, StartupTargetState::Missing);
    EXPECT_FALSE(gone->executablePath.contains('%')) << "expanded: " << gone->executablePath;
    EXPECT_TRUE(gone->enabled);
    EXPECT_TRUE(gone->publisher.empty());

    const StartupEntry* unicode = find(entries, UNICODE_NAME, StartupLocation::RunUser);
    ASSERT_NE(unicode, nullptr) << "a non-ASCII value name round-trips as UTF-8";
    EXPECT_EQ(unicode->executablePath, "C:\\TaskSmack-1722\\caf\xC3\xA9.exe");
    EXPECT_EQ(unicode->name, "caf\xC3\xA9.exe");
    EXPECT_TRUE(unicode->enabled);

    const StartupEntry* once = find(entries, "Once", StartupLocation::RunOnceUser);
    ASSERT_NE(once, nullptr);
    EXPECT_EQ(once->command, "C:\\once.exe");
    // #1971: a RunOnce entry is shown by its value name, with its program's name kept for the tooltip.
    EXPECT_EQ(once->name, "Once");
    EXPECT_EQ(once->programName, "once.exe");

    for (const auto& [value, location] :
         {std::pair{"Cleanup", StartupLocation::RunOnceUser}, std::pair{"MachineCleanup", StartupLocation::RunOnceMachine}})
    {
        const StartupEntry* cleanup = find(entries, value, location);
        ASSERT_NE(cleanup, nullptr) << value;
        EXPECT_EQ(cleanup->name, value) << "the value name, not cmd.exe's description";
        EXPECT_EQ(cleanup->programName, alpha->name) << "cmd.exe's description, as the Run entry shows it";
    }

    const StartupEntry* blank = find(entries, "Blank", StartupLocation::RunOnceUser);
    ASSERT_NE(blank, nullptr);
    EXPECT_EQ(blank->name, "Blank") << "no program: named by its value";
    EXPECT_EQ(blank->programName, "Blank");
    EXPECT_TRUE(blank->executablePath.empty());
    EXPECT_EQ(blank->target, StartupTargetState::Unresolved);

    const StartupEntry* bare = find(entries, "Bare", StartupLocation::RunOnceUser);
    ASSERT_NE(bare, nullptr);
    EXPECT_EQ(bare->executablePath, "tasksmack-1722-no-such-tool") << "a name the search path doesn't find is kept as written";
    EXPECT_EQ(bare->name, "Bare");
    EXPECT_EQ(bare->programName, "tasksmack-1722-no-such-tool");
    for (const StartupEntry& entry : entries)
    {
        EXPECT_TRUE(entry.packageFamily.empty()) << entry.registrationName;
        EXPECT_FALSE(entry.setByPolicy) << entry.registrationName;
    }
    EXPECT_EQ(bare->target, StartupTargetState::Missing);

    // A second pass reads the cached publisher, and a value that's gone drops its cache entry.
    EXPECT_EQ(probe.enumerate().size(), 8U);
    fake().keys.front().values.erase(fake().keys.front().values.begin());
    EXPECT_EQ(probe.enumerate().size(), 7U);
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
    EXPECT_EQ(tool->name, "Tool.exe") << "no version resource: its file name";
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

    const StartupEntry* good = find(entries, "Good.lnk", StartupLocation::StartupFolderUser);
    ASSERT_NE(good, nullptr) << "a shortcut is registered by its file name, extension included";
    EXPECT_NE(good->name, "Good") << "#1873: named by its target's FileDescription";
    EXPECT_FALSE(good->name.empty());
    EXPECT_EQ(lower(good->command), lower(std::format("\"{}\" /c exit", WinString::wideToUtf8(systemCmd().wstring()))));
    EXPECT_EQ(good->target, StartupTargetState::Present);
    EXPECT_TRUE(good->enabled);
    EXPECT_FALSE(good->publisher.empty());

    const StartupEntry* dangling = find(entries, "Dangling.lnk", StartupLocation::StartupFolderUser);
    ASSERT_NE(dangling, nullptr);
    EXPECT_EQ(dangling->target, StartupTargetState::Missing);
    EXPECT_EQ(dangling->name, "no-such-target.exe");

    const StartupEntry* broken = find(entries, "Broken.lnk", StartupLocation::StartupFolderUser);
    ASSERT_NE(broken, nullptr);
    EXPECT_EQ(broken->name, "Broken") << "an unread shortcut is named without its extension";
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
    EXPECT_EQ(shared->name, "Shared.bat");
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

// #1798: packaged apps' startup tasks, as Task Manager lists them.

constexpr const wchar_t* TERMINAL_FAMILY = L"Contoso.Terminal_8wekyb3d8bbwe";
constexpr const wchar_t* TERMINAL_FULL = L"Contoso.Terminal_1.2.3.0_x64__8wekyb3d8bbwe";
constexpr const wchar_t* CHAT_FAMILY = L"Contoso.Chat_abc123";
constexpr const wchar_t* CHAT_RESOURCES = L"Contoso.Chat_1.0.0.0_neutral_split.scale-100_abc123";
constexpr const wchar_t* CHAT_FULL = L"Contoso.Chat_1.0.0.0_x64__abc123";

[[nodiscard]] std::vector<std::uint8_t> dwordData(DWORD value)
{
    std::vector<std::uint8_t> data(sizeof(value));
    std::memcpy(data.data(), &value, sizeof(value));
    return data;
}

[[nodiscard]] std::vector<std::uint8_t> qwordData(std::uint64_t value)
{
    std::vector<std::uint8_t> data(sizeof(value));
    std::memcpy(data.data(), &value, sizeof(value));
    return data;
}

/// The HKCU key @p subkey, made when missing.
FakeKey& ensureUserKey(const std::wstring& subkey)
{
    const auto it = std::ranges::find_if(fake().keys, [&](const FakeKey& key) { return key.subkey == subkey; });
    return it != fake().keys.end() ? *it : addKey(HKEY_CURRENT_USER, subkey.c_str());
}

/// The SystemAppData key of @p family (and SystemAppData itself, made when missing), and a subkey under
/// it holding @p values.
void addTaskKey(const std::wstring& family, const std::wstring& name, std::vector<FakeValue> values)
{
    (void) ensureUserKey(Math::PACKAGED_TASKS_KEY);
    const std::wstring familyKey = std::format(L"{}\\{}", Math::PACKAGED_TASKS_KEY, family);
    (void) ensureUserKey(familyKey);
    addKey(HKEY_CURRENT_USER, std::format(L"{}\\{}", familyKey, name).c_str()).values = std::move(values);
}

/// A manifest with one Application holding @p extensions, named @p displayName by @p publisher.
[[nodiscard]] std::string manifestXml(std::string_view extensions,
                                      std::string_view displayName = "Contoso App",
                                      std::string_view publisher = "Contoso Ltd.",
                                      std::string_view visualName = "Contoso Visual")
{
    return std::format(R"(<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:uap5="http://schemas.microsoft.com/appx/manifest/uap/windows10/5"
         xmlns:desktop="http://schemas.microsoft.com/appx/manifest/desktop/windows10">
  <!-- a comment -->
  <Properties>
    <DisplayName>{}</DisplayName>
    <PublisherDisplayName>{}</PublisherDisplayName>
    <Logo>Assets\Logo.png</Logo>
  </Properties>
  <Applications>
    <Application Id="App" Executable="app/Main.exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="{}" Description="x" Square150x150Logo="a.png" Square44x44Logo="b.png" BackgroundColor="transparent" />
      <Extensions>{}</Extensions>
    </Application>
  </Applications>
</Package>)",
                       displayName,
                       publisher,
                       visualName,
                       extensions);
}

TEST(WindowsPackageManifestTest, ReadsStartupTasksWhateverTheirPrefix)
{
    const auto parsed = Windows::parsePackageManifest(manifestXml(R"(
        <uap5:Extension Category="windows.startupTask" Executable="bin\Starter.exe" EntryPoint="Windows.FullTrustApplication">
          <uap5:StartupTask TaskId="Starter" Enabled="true" DisplayName="Contoso Starter" />
        </uap5:Extension>
        <desktop:Extension Category="windows.startupTask">
          <desktop:StartupTask TaskId="Quiet" Enabled="false" />
        </desktop:Extension>
        <uap:Extension Category="windows.protocol"><uap:Protocol Name="contoso" /><uap5:StartupTask TaskId="NotAStartupTask" /></uap:Extension>)",
                                                                  "Contoso &amp; Co",
                                                                  "  Contoso Ltd.  "));
    ASSERT_TRUE(parsed.has_value());
    const Windows::PackageManifest manifest = parsed.value_or(Windows::PackageManifest{});
    EXPECT_EQ(manifest.displayName, "Contoso & Co");
    EXPECT_EQ(manifest.publisherDisplayName, "Contoso Ltd.");
    ASSERT_EQ(manifest.startupTasks.size(), 2U) << "a StartupTask outside a windows.startupTask extension isn't one";

    const Windows::PackageStartupTask& starter = manifest.startupTasks[0];
    EXPECT_EQ(starter.taskId, "Starter");
    EXPECT_EQ(starter.displayName, "Contoso Starter");
    EXPECT_EQ(starter.executable, "bin\\Starter.exe") << "the extension's Executable wins";
    EXPECT_TRUE(starter.enabledByDefault);

    const Windows::PackageStartupTask& quiet = manifest.startupTasks[1];
    EXPECT_EQ(quiet.taskId, "Quiet");
    EXPECT_EQ(quiet.displayName, "Contoso Visual") << "no DisplayName: the Application's";
    EXPECT_EQ(quiet.executable, "app/Main.exe") << "no Executable: the Application's";
    EXPECT_FALSE(quiet.enabledByDefault);

    // #1800: each task's logo is its Application's Square44x44Logo; the package's own is kept too.
    EXPECT_EQ(manifest.logo, "Assets\\Logo.png");
    EXPECT_EQ(starter.logo, "b.png");
    EXPECT_EQ(quiet.logo, "b.png");
}

TEST(WindowsPackageManifestTest, ATaskWithoutAnApplicationLogoTakesThePackagesLogo)
{
    // #1800: no Square44x44Logo on the VisualElements, so the package's Store logo stands in.
    const auto parsed = Windows::parsePackageManifest(R"(<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap5="http://schemas.microsoft.com/appx/manifest/uap/windows10/5">
  <Properties><DisplayName>Plain</DisplayName><Logo> Images\StoreLogo.png </Logo></Properties>
  <Applications>
    <Application Id="App" Executable="Plain.exe">
      <VisualElements DisplayName="Plain" />
      <Extensions>
        <uap5:Extension Category="windows.startupTask"><uap5:StartupTask TaskId="Plain" Enabled="true" /></uap5:Extension>
      </Extensions>
    </Application>
  </Applications>
</Package>)");
    ASSERT_TRUE(parsed.has_value());
    const Windows::PackageManifest manifest = parsed.value_or(Windows::PackageManifest{});
    ASSERT_EQ(manifest.startupTasks.size(), 1U);
    EXPECT_EQ(manifest.logo, "Images\\StoreLogo.png") << "trimmed";
    EXPECT_EQ(manifest.startupTasks[0].logo, "Images\\StoreLogo.png");
}

TEST(WindowsPackageManifestTest, MalformedOrTasklessManifests)
{
    EXPECT_FALSE(Windows::parsePackageManifest("<Package><Properties>").has_value());
    EXPECT_FALSE(Windows::parsePackageManifest("not xml at all").has_value());
    EXPECT_FALSE(Windows::parsePackageManifest(R"(<!DOCTYPE p [<!ENTITY x "y">]><Package/>)").has_value()) << "no DTDs";
    const auto none = Windows::parsePackageManifest(manifestXml(""));
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none.value_or(Windows::PackageManifest{}).startupTasks.empty());
    const auto unnamed = Windows::parsePackageManifest(manifestXml(R"(
        <uap5:Extension Category="windows.startupTask"><uap5:StartupTask Enabled="true" /></uap5:Extension>)"));
    ASSERT_TRUE(unnamed.has_value());
    EXPECT_TRUE(unnamed.value_or(Windows::PackageManifest{}).startupTasks.empty())
        << "a task without a TaskId can't be matched to its state";
}

TEST_F(WindowsStartupProbeSeamsTest, ListsPackagedStartupTasksAsTaskManagerDoes)
{
    // Terminal: installed, disabled by default (State 0), named through ms-resource strings.
    const std::filesystem::path terminal = makeFolder(L"terminal");
    writeFile(terminal / L"AppxManifest.xml",
              manifestXml(R"(
        <uap5:Extension Category="windows.startupTask">
          <uap5:StartupTask TaskId="StartOnLogin" Enabled="false" DisplayName="ms-resource:AppName" />
        </uap5:Extension>)",
                          "ms-resource:AppStoreName",
                          "Contoso Corporation"));
    // app/Main.exe isn't in the folder: the program is reported missing.
    fake().packages.push_back({.family = TERMINAL_FAMILY, .fullName = TERMINAL_FULL, .folder = terminal.wstring()});
    fake().resources.emplace_back(std::format(L"@{{{}?ms-resource://Contoso.Terminal/Resources/AppName}}", TERMINAL_FULL), L"Terminal");
    addTaskKey(TERMINAL_FAMILY, L"Schemas", {}); // other app data: no State, not a task
    addTaskKey(TERMINAL_FAMILY, L"StartOnLogin", {{.name = L"State", .type = REG_DWORD, .data = dwordData(0)}});

    // Chat: a resource package (no folder) is listed first; the main package declares two tasks.
    // "Chat" was disabled by the user, with a QWORD time; "Policy" is enabled by policy and its
    // DisplayName can't be loaded, so the package's name is used.
    const std::filesystem::path chat = makeFolder(L"chat");
    std::filesystem::create_directories(chat / L"bin");
    writeFile(chat / L"bin" / L"Starter.exe");
    writeFile(chat / L"AppxManifest.xml",
              manifestXml(R"(
        <uap5:Extension Category="windows.startupTask" Executable="bin/Starter.exe">
          <uap5:StartupTask TaskId="Chat" Enabled="true" DisplayName="Contoso Chat" />
        </uap5:Extension>
        <uap5:Extension Category="windows.startupTask" Executable="bin/Starter.exe">
          <uap5:StartupTask TaskId="Policy" Enabled="true" DisplayName="ms-resource:Missing" />
        </uap5:Extension>)",
                          "Contoso Chat Package",
                          "Contoso Chat Ltd."));
    fake().packages.push_back({.family = CHAT_FAMILY, .fullName = CHAT_RESOURCES, .folder = L""});
    fake().packages.push_back({.family = CHAT_FAMILY, .fullName = CHAT_FULL, .folder = chat.wstring()});
    addTaskKey(CHAT_FAMILY,
               L"chat", // the registry's case needn't match the manifest's
               {{.name = L"State", .type = REG_DWORD, .data = dwordData(1)},
                {.name = L"LastDisabledTime", .type = REG_QWORD, .data = qwordData(DISABLED_AT)}});
    addTaskKey(CHAT_FAMILY,
               L"Policy",
               {{.name = L"State", .type = REG_DWORD, .data = dwordData(4)},
                {.name = L"LastDisabledTime", .type = REG_DWORD, .data = dwordData(1234)}}); // enabled: no time shown
    // A task the manifest no longer declares (Task Manager drops it too).
    addTaskKey(CHAT_FAMILY, L"OldLauncher", {{.name = L"State", .type = REG_DWORD, .data = dwordData(2)}});

    // A family whose package isn't installed any more, and one with no tasks at all.
    addTaskKey(L"Contoso.Removed_xyz", L"App", {{.name = L"State", .type = REG_DWORD, .data = dwordData(2)}});
    addTaskKey(L"Contoso.NoTasks_xyz", L"PSR", {{.name = L"Other", .type = REG_DWORD, .data = dwordData(2)}});

    WindowsStartupProbe probe(fakeFunctions());
    const std::vector<StartupEntry> entries = probe.enumerate();
    ASSERT_EQ(entries.size(), 3U);

    const StartupEntry* term = find(entries, "StartOnLogin", StartupLocation::PackagedApp);
    ASSERT_NE(term, nullptr);
    EXPECT_EQ(term->name, "Terminal") << "named from its resolved ms-resource DisplayName";
    EXPECT_EQ(term->packageFamily, "Contoso.Terminal_8wekyb3d8bbwe") << "#1873: the task is found again by family and TaskId";
    EXPECT_FALSE(term->setByPolicy);
    EXPECT_EQ(term->scope, StartupScope::User);
    EXPECT_EQ(term->publisher, "Contoso Corporation");
    EXPECT_FALSE(term->enabled);
    EXPECT_EQ(term->disabledAtUnixSeconds, 0U) << "never disabled by the user: no time";
    EXPECT_EQ(term->executablePath, WinString::wideToUtf8((terminal / L"app" / L"Main.exe").wstring()));
    EXPECT_EQ(term->command, std::format("\"{}\"", term->executablePath));
    EXPECT_EQ(term->target, StartupTargetState::Missing);
    // #1800: the Application's Square44x44Logo under the package's folder, as the manifest names it.
    EXPECT_EQ(term->iconPath, WinString::wideToUtf8((terminal / L"b.png").wstring()));

    const StartupEntry* chatEntry = find(entries, "chat", StartupLocation::PackagedApp);
    ASSERT_NE(chatEntry, nullptr) << "registered by the task key's own name, whatever the manifest's case";
    EXPECT_EQ(chatEntry->name, "Contoso Chat");
    EXPECT_EQ(chatEntry->packageFamily, "Contoso.Chat_abc123");
    EXPECT_EQ(chatEntry->publisher, "Contoso Chat Ltd.");
    EXPECT_FALSE(chatEntry->enabled);
    EXPECT_EQ(chatEntry->disabledAtUnixSeconds, DISABLED_AT);
    EXPECT_EQ(chatEntry->executablePath, WinString::wideToUtf8((chat / L"bin" / L"Starter.exe").wstring()));
    EXPECT_EQ(chatEntry->target, StartupTargetState::Present);

    const StartupEntry* policy = find(entries, "Policy", StartupLocation::PackagedApp);
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->name, "Contoso Chat Package") << "an unloadable DisplayName falls back to the package's";
    EXPECT_TRUE(policy->enabled);
    EXPECT_TRUE(policy->setByPolicy) << "EnabledByPolicy (#1873)";
    EXPECT_EQ(policy->disabledAtUnixSeconds, 0U);

    // A second pass reads the cached manifests: only the resource package, which has no folder, is
    // looked up again (once for each of Chat's three recorded tasks).
    const int lookups = fake().packageLookups;
    EXPECT_EQ(probe.enumerate().size(), 3U);
    EXPECT_EQ(fake().packageLookups, lookups + 3);
}

TEST_F(WindowsStartupProbeSeamsTest, AnUnreadableManifestListsNoTasksForItsPackage)
{
    const std::filesystem::path folder = makeFolder(L"broken");
    writeFile(folder / L"AppxManifest.xml", "<Package><Applications>");
    fake().packages.push_back({.family = TERMINAL_FAMILY, .fullName = TERMINAL_FULL, .folder = folder.wstring()});
    addTaskKey(TERMINAL_FAMILY, L"StartOnLogin", {{.name = L"State", .type = REG_DWORD, .data = dwordData(2)}});
    // The family key itself can be opened but not listed.
    ensureUserKey(std::format(L"{}\\Contoso.Denied_xyz", Math::PACKAGED_TASKS_KEY)).infoFails = true;

    WindowsStartupProbe probe(fakeFunctions());
    EXPECT_TRUE(probe.enumerate().empty());
}
TEST_F(WindowsStartupProbeSeamsTest, PackagedTasksThatCantBeResolvedAreSkipped)
{
    // Quirks: installed, but no name loads, so the task is shown by its TaskId. Its key is listed twice in
    // different case (one entry), and a third key's State isn't a number (not a task).
    const std::filesystem::path quirks = makeFolder(L"quirks");
    writeFile(quirks / L"AppxManifest.xml",
              manifestXml(R"(
        <uap5:Extension Category="windows.startupTask">
          <uap5:StartupTask TaskId="Bare" Enabled="true" DisplayName="ms-resource:Missing" />
        </uap5:Extension>)",
                          "ms-resource:AlsoMissing"));
    fake().packages.push_back({.family = L"Contoso.Quirks_q1", .fullName = L"Contoso.Quirks_1.0.0.0_x64__q1", .folder = quirks.wstring()});
    addTaskKey(L"Contoso.Quirks_q1", L"Bare", {{.name = L"State", .type = REG_DWORD, .data = dwordData(2)}});
    addTaskKey(L"Contoso.Quirks_q1", L"BARE", {{.name = L"State", .type = REG_DWORD, .data = dwordData(2)}});
    addTaskKey(L"Contoso.Quirks_q1", L"Text", {{.name = L"State", .type = REG_SZ, .data = stringData(L"2")}});

    // Families whose package can't be read: the family's names, then a package's folder, refused after
    // they were sized; a folder with no manifest; and a manifest another handle holds exclusively.
    fake().familyFailingAfterSizing = L"Contoso.Listless_l1";
    fake().packages.push_back({.family = L"Contoso.Listless_l1", .fullName = L"Contoso.Listless_1_x64__l1", .folder = quirks.wstring()});
    fake().packages.push_back({.family = L"Contoso.Pathless_p1",
                               .fullName = L"Contoso.Pathless_1_x64__p1",
                               .folder = quirks.wstring(),
                               .pathFailsAfterSizing = true});
    fake().packages.push_back(
        {.family = L"Contoso.NoManifest_n1", .fullName = L"Contoso.NoManifest_1_x64__n1", .folder = makeFolder(L"empty").wstring()});
    const std::filesystem::path locked = makeFolder(L"locked");
    writeFile(locked / L"AppxManifest.xml", manifestXml(""));
    fake().packages.push_back({.family = L"Contoso.Locked_k1", .fullName = L"Contoso.Locked_1_x64__k1", .folder = locked.wstring()});
    for (const wchar_t* family : {L"Contoso.Listless_l1", L"Contoso.Pathless_p1", L"Contoso.NoManifest_n1", L"Contoso.Locked_k1"})
    {
        addTaskKey(family, L"Bare", {{.name = L"State", .type = REG_DWORD, .data = dwordData(2)}});
    }
    HANDLE lock =
        CreateFileW((locked / L"AppxManifest.xml").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);

    WindowsStartupProbe probe(fakeFunctions());
    const std::vector<StartupEntry> entries = probe.enumerate();
    CloseHandle(lock);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].registrationName, "Bare");
    EXPECT_EQ(entries[0].name, "Bare") << "neither the task's nor the package's name loads";
    EXPECT_EQ(entries[0].packageFamily, "Contoso.Quirks_q1");
}

// #3221: the odd answers the registry, known-folder and package APIs can give each list nothing, and
// close every key they open: a key that opens to no handle, a subkey that can't be enumerated, a
// known folder with no path, numbers of the wrong size, a family sized as no packages, a package
// listed without a name, and a package folder sized as no characters.
TEST_F(WindowsStartupProbeSeamsTest, OddAnswersFromTheRegistryAndPackageApisListNothing)
{
    fake().knownFolderWithoutPath = true;
    addKey(HKEY_CURRENT_USER, RUN_KEY).opensToNoHandle = true;

    const std::vector<FakeValue> enabled = {{.name = L"State", .type = REG_DWORD, .data = dwordData(2)}};
    // A State of two bytes, and a REG_QWORD one of four: neither is a number, so neither is a task.
    addTaskKey(L"Contoso.Short_s1", L"Word", {{.name = L"State", .type = REG_DWORD, .data = {2, 0}}});
    addTaskKey(L"Contoso.Short_s1",
               L"Qword",
               {{.name = L"State", .type = REG_QWORD, .data = dwordData(2)},
                {.name = L"LastDisabledTime", .type = REG_QWORD, .data = dwordData(1)}});
    // A family whose task subkey can't be enumerated, and one whose key opens to no handle.
    addTaskKey(L"Contoso.Unlisted_u1", L"Task", enabled);
    ensureUserKey(std::format(L"{}\\Contoso.Unlisted_u1", Math::PACKAGED_TASKS_KEY)).enumKeyFails = {0};
    addTaskKey(L"Contoso.NoHandle_n1", L"Task", enabled);
    ensureUserKey(std::format(L"{}\\Contoso.NoHandle_n1", Math::PACKAGED_TASKS_KEY)).opensToNoHandle = true;

    // Recorded tasks whose packages can't be found or read.
    const std::filesystem::path folder = makeFolder(L"package");
    writeFile(folder / L"AppxManifest.xml",
              manifestXml(
                  R"(<uap5:Extension Category="windows.startupTask"><uap5:StartupTask TaskId="Task" Enabled="true" /></uap5:Extension>)"));
    addTaskKey(L"Contoso.SizedEmpty_e1", L"Task", enabled);
    fake().familySizedEmpty = L"Contoso.SizedEmpty_e1";
    addTaskKey(L"Contoso.Unnamed_m1", L"Task", enabled);
    fake().packages.push_back({.family = L"Contoso.Unnamed_m1",
                               .fullName = L"Contoso.Unnamed_1.0.0.0_x64__m1",
                               .folder = folder.wstring(),
                               .listedWithoutName = true});
    addTaskKey(L"Contoso.NoFolder_f1", L"Task", enabled);
    fake().packages.push_back({.family = L"Contoso.NoFolder_f1",
                               .fullName = L"Contoso.NoFolder_1.0.0.0_x64__f1",
                               .folder = folder.wstring(),
                               .pathSizedEmpty = true});

    WindowsStartupProbe probe(fakeFunctions());
    EXPECT_TRUE(probe.enumerate().empty());
}

TEST(WindowsPackageManifestTest, EmptyElementsTasksOutsideAnApplicationAndCdataNames)
{
    const auto parsed = Windows::parsePackageManifest(R"(<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap5="http://schemas.microsoft.com/appx/manifest/uap/windows10/5">
  <Properties><DisplayName><![CDATA[Contoso & Co]]></DisplayName></Properties>
  <Extensions>
    <uap5:Extension Category="windows.startupTask" Executable="loose.exe">
      <uap5:StartupTask TaskId="Loose" Enabled="true"></uap5:StartupTask>
    </uap5:Extension>
    <uap5:Extension Category="windows.startupTask" />
  </Extensions>
  <Applications><Application Id="App" Executable="app.exe" /></Applications>
</Package>)");
    ASSERT_TRUE(parsed.has_value());
    const Windows::PackageManifest manifest = parsed.value_or(Windows::PackageManifest{});
    EXPECT_EQ(manifest.displayName, "Contoso & Co") << "a CDATA section is text too";
    ASSERT_EQ(manifest.startupTasks.size(), 1U);
    EXPECT_EQ(manifest.startupTasks[0].taskId, "Loose");
    EXPECT_EQ(manifest.startupTasks[0].executable, "loose.exe") << "its extension's, with no Application around it";
}

TEST_F(WindowsStartupProbeSeamsTest, ARunValueOfOnlySpacesNamesNoProgram)
{
    addKey(HKEY_CURRENT_USER, RUN_KEY).values = {{.name = L"Spaces", .type = REG_SZ, .data = stringData(L"   ")}};
    WindowsStartupProbe probe(fakeFunctions());
    const std::vector<StartupEntry> entries = probe.enumerate();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].name, "Spaces");
    EXPECT_TRUE(entries[0].executablePath.empty());
    EXPECT_EQ(entries[0].target, StartupTargetState::Unresolved);
}

TEST_F(WindowsStartupProbeSeamsTest, AShortcutWithoutATargetReadsNothing)
{
    // A shortcut that was given arguments but no target.
    const std::filesystem::path shortcut = m_Root / L"NoTarget.lnk";
    {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> file;
        ASSERT_TRUE(SUCCEEDED(CoCreateInstance(
            CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.releaseAndGetAddressOf()))));
        ASSERT_TRUE(SUCCEEDED(link->SetArguments(L"--x")));
        ASSERT_TRUE(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.releaseAndGetAddressOf()))));
        ASSERT_TRUE(SUCCEEDED(file->Save(shortcut.c_str(), TRUE)));
    }
    EXPECT_FALSE(Windows::readShortcutCommand(shortcut).has_value());
}
} // namespace
} // namespace Platform
