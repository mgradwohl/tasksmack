#include "WindowsStartupProbe.h"

#include "Platform/IStartupProbe.h"

#include <spdlog/spdlog.h>

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

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "version.lib")

#include "ComPtr.h"
#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsStartupProbeMath.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Platform
{

namespace
{

constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* RUN_ONCE_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
using Windows::StartupMath::APPROVED_FOLDER_KEY;
using Windows::StartupMath::APPROVED_RUN32_KEY;
using Windows::StartupMath::APPROVED_RUN_KEY;

/// One Run/RunOnce key and the StartupApproved key that records its entries' state (none for RunOnce).
struct RunSource
{
    HKEY root;
    const wchar_t* subkey;
    REGSAM view;
    StartupLocation location;
    const wchar_t* approvedSubkey;
};

// const, not constexpr: the predefined HKEY roots are integer-to-pointer casts.
const std::array<RunSource, 5> RUN_SOURCES = {{
    {.root = HKEY_CURRENT_USER, .subkey = RUN_KEY, .view = 0, .location = StartupLocation::RunUser, .approvedSubkey = APPROVED_RUN_KEY},
    {.root = HKEY_LOCAL_MACHINE,
     .subkey = RUN_KEY,
     .view = KEY_WOW64_64KEY,
     .location = StartupLocation::RunMachine,
     .approvedSubkey = APPROVED_RUN_KEY},
    {.root = HKEY_LOCAL_MACHINE,
     .subkey = RUN_KEY,
     .view = KEY_WOW64_32KEY,
     .location = StartupLocation::RunMachine32,
     .approvedSubkey = APPROVED_RUN32_KEY},
    {.root = HKEY_CURRENT_USER, .subkey = RUN_ONCE_KEY, .view = 0, .location = StartupLocation::RunOnceUser, .approvedSubkey = nullptr},
    {.root = HKEY_LOCAL_MACHINE,
     .subkey = RUN_ONCE_KEY,
     .view = KEY_WOW64_64KEY,
     .location = StartupLocation::RunOnceMachine,
     .approvedSubkey = nullptr},
}};

/// StartupApproved records, by lower-cased value name.
using ApprovedMap = std::unordered_map<std::wstring, Windows::StartupMath::ApprovedState>;

[[nodiscard]] std::wstring toLower(std::wstring text)
{
    for (wchar_t& c : text)
    {
        c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    }
    return text;
}

/// Calls `onValue(name, type, bytes)` for every value of `root\subkey`. A key that doesn't exist or
/// can't be opened has no values.
template<typename OnValue> void forEachValue(HKEY root, const wchar_t* subkey, REGSAM view, OnValue onValue)
{
    Windows::UniqueRegistryKey key;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | view, key.put()) != ERROR_SUCCESS)
    {
        return;
    }
    DWORD valueCount = 0;
    DWORD maxNameChars = 0;
    DWORD maxDataBytes = 0;
    if (RegQueryInfoKeyW(
            key.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &valueCount, &maxNameChars, &maxDataBytes, nullptr, nullptr) !=
        ERROR_SUCCESS)
    {
        return;
    }
    std::vector<wchar_t> name(maxNameChars + 1);
    std::vector<std::uint8_t> data(maxDataBytes + sizeof(wchar_t));
    for (DWORD index = 0; index < valueCount; ++index)
    {
        auto nameChars = static_cast<DWORD>(name.size());
        auto dataBytes = static_cast<DWORD>(data.size());
        DWORD type = 0;
        // A value that grew since RegQueryInfoKeyW (ERROR_MORE_DATA) is skipped this pass.
        if (RegEnumValueW(key.get(), index, name.data(), &nameChars, nullptr, &type, data.data(), &dataBytes) != ERROR_SUCCESS)
        {
            continue;
        }
        onValue(std::wstring(name.data(), nameChars), type, std::span<const std::uint8_t>(data.data(), dataBytes));
    }
}

[[nodiscard]] ApprovedMap readApproved(HKEY root, const wchar_t* subkey)
{
    ApprovedMap approved;
    if (subkey == nullptr)
    {
        return approved;
    }
    forEachValue(root,
                 subkey,
                 0,
                 [&approved](const std::wstring& name, DWORD type, std::span<const std::uint8_t> data)
                 {
                     if (type != REG_BINARY)
                     {
                         return;
                     }
                     if (const auto state = Windows::StartupMath::parseStartupApproved(data))
                     {
                         approved.emplace(toLower(name), *state);
                     }
                 });
    return approved;
}

/// A REG_SZ/REG_EXPAND_SZ value's text, without the terminator(s) the data may or may not carry.
[[nodiscard]] std::wstring registryString(std::span<const std::uint8_t> data)
{
    std::wstring text(data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), data.data(), text.size() * sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0')
    {
        text.pop_back();
    }
    return text;
}

void applyApproved(StartupEntry& entry, const ApprovedMap& approved, const std::wstring& key)
{
    if (const auto it = approved.find(toLower(key)); it != approved.end())
    {
        entry.enabled = it->second.enabled;
        entry.disabledAtUnixSeconds = it->second.disabledAtUnixSeconds;
    }
}

/// COM on the calling thread for one enumerate(): IShellLinkW needs it. A thread that already has
/// another apartment (RPC_E_CHANGED_MODE) can still use COM; only the scope's own init is undone.
class ComScope
{
  public:
    ComScope() noexcept : m_Result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))
    {}
    ~ComScope()
    {
        if (SUCCEEDED(m_Result))
        {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;

    [[nodiscard]] bool usable() const noexcept
    {
        return SUCCEEDED(m_Result) || m_Result == RPC_E_CHANGED_MODE;
    }

  private:
    HRESULT m_Result;
};

/// A shortcut's target and arguments, raw (environment variables unexpanded); nullopt when the
/// shortcut can't be loaded or has no file-system target (e.g. an advertised installer shortcut).
[[nodiscard]] std::optional<std::wstring> readShortcutCommandWide(const std::wstring& shortcutPath)
{
    // The reinterpret_casts below are COM's out-parameter convention: the call writes an interface pointer
    // of the IID's type through void**, and the ComPtr's slot holds exactly that type.
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.releaseAndGetAddressOf()))))
    {
        return std::nullopt;
    }
    ComPtr<IPersistFile> file;
    if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.releaseAndGetAddressOf()))) ||
        FAILED(file->Load(shortcutPath.c_str(), STGM_READ)))
    {
        return std::nullopt;
    }
    std::array<wchar_t, MAX_PATH> target{};
    if (link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH) != S_OK || target[0] == L'\0')
    {
        return std::nullopt;
    }
    std::array<wchar_t, INFOTIPSIZE> arguments{};
    std::wstring command = std::format(L"\"{}\"", target.data());
    if (SUCCEEDED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size()))) && arguments[0] != L'\0')
    {
        command += L' ';
        command += arguments.data();
    }
    return command;
}

[[nodiscard]] std::wstring knownFolder(const KNOWNFOLDERID& id)
{
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) && path != nullptr)
    {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

[[nodiscard]] std::uint64_t toUint64(const FILETIME& time) noexcept
{
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
}

/// The CompanyName from a file's version resource: the first listed translation that has one, then
/// US English in Unicode and Windows-1252. Empty when the file has no version resource.
[[nodiscard]] std::string readCompanyName(const std::wstring& path)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
    {
        return {};
    }
    std::vector<std::byte> block(size);
    if (GetFileVersionInfoW(path.c_str(), 0, size, block.data()) == FALSE)
    {
        return {};
    }

    struct Translation
    {
        WORD language;
        WORD codePage;
    };
    std::vector<Translation> translations;
    void* translationData = nullptr;
    UINT translationBytes = 0;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", &translationData, &translationBytes) != FALSE &&
        translationData != nullptr)
    {
        const std::span listed(static_cast<const Translation*>(translationData), translationBytes / sizeof(Translation));
        translations.assign(listed.begin(), listed.end());
    }
    translations.push_back({.language = 0x0409, .codePage = 0x04B0});
    translations.push_back({.language = 0x0409, .codePage = 0x04E4});

    for (const Translation& translation : translations)
    {
        const std::wstring query = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\CompanyName", translation.language, translation.codePage);
        void* value = nullptr;
        UINT chars = 0;
        if (VerQueryValueW(block.data(), query.c_str(), &value, &chars) != FALSE && value != nullptr && chars > 0)
        {
            std::string company = WinString::wideToUtf8(static_cast<const wchar_t*>(value));
            if (const auto end = company.find_last_not_of(' '); end != std::string::npos)
            {
                company.erase(end + 1);
                return company;
            }
        }
    }
    return {};
}

/// A publisher read for one file, kept until the file's last-write time changes.
struct CachedPublisher
{
    std::uint64_t lastWrite = 0;
    std::string publisher;
};

} // namespace

namespace Windows
{

std::string expandEnvironmentStrings(std::string_view text)
{
    const std::wstring wide = WinString::utf8ToWide(text);
    if (!wide.contains(L'%'))
    {
        return std::string(text);
    }
    const DWORD needed = ExpandEnvironmentStringsW(wide.c_str(), nullptr, 0);
    if (needed == 0)
    {
        return std::string(text);
    }
    std::wstring expanded(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(wide.c_str(), expanded.data(), needed);
    if (written == 0 || written > needed)
    {
        return std::string(text);
    }
    expanded.resize(written - 1); // written counts the terminator
    return WinString::wideToUtf8(expanded);
}

std::string resolveStartupExecutable(std::string_view commandLine)
{
    std::string executable = StartupMath::executableFromCommandLine(expandEnvironmentStrings(commandLine));
    if (executable.empty() || executable.find_first_of("\\/:") != std::string::npos)
    {
        return executable;
    }
    // A bare file name runs from the search path (System32, PATH), as the shell would find it.
    const std::wstring name = WinString::utf8ToWide(executable);
    std::array<wchar_t, MAX_PATH> found{};
    const DWORD length = SearchPathW(nullptr, name.c_str(), L".exe", static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (length > 0 && length < found.size())
    {
        return WinString::wideToUtf8(std::wstring_view(found.data(), length));
    }
    return executable;
}

std::optional<std::string> readShortcutCommand(const std::filesystem::path& shortcut)
{
    if (const auto command = readShortcutCommandWide(shortcut.wstring()))
    {
        return WinString::wideToUtf8(*command);
    }
    return std::nullopt;
}

} // namespace Windows

struct WindowsStartupProbe::Impl
{
    std::unordered_map<std::wstring, CachedPublisher> publishers; ///< By lower-cased path.

    /// Fills `entry`'s executable, target state and publisher from its command.
    void resolveTarget(StartupEntry& entry, std::unordered_set<std::wstring>& seenPaths)
    {
        entry.executablePath = Windows::resolveStartupExecutable(entry.command);
        if (entry.executablePath.empty())
        {
            return;
        }
        const std::wstring path = WinString::utf8ToWide(entry.executablePath);
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) == FALSE ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            entry.target = StartupTargetState::Missing;
            return;
        }
        entry.target = StartupTargetState::Present;

        const std::wstring key = toLower(path);
        seenPaths.insert(key);
        const std::uint64_t lastWrite = toUint64(attributes.ftLastWriteTime);
        auto [it, inserted] = publishers.try_emplace(key);
        if (inserted || it->second.lastWrite != lastWrite)
        {
            it->second = {.lastWrite = lastWrite, .publisher = readCompanyName(path)};
        }
        entry.publisher = it->second.publisher;
    }

    void readRunKeys(std::vector<StartupEntry>& entries, std::unordered_set<std::wstring>& seenPaths)
    {
        for (const RunSource& source : RUN_SOURCES)
        {
            const ApprovedMap approved = readApproved(source.root, source.approvedSubkey);
            forEachValue(source.root,
                         source.subkey,
                         source.view,
                         [&](const std::wstring& name, DWORD type, std::span<const std::uint8_t> data)
                         {
                             if ((type != REG_SZ && type != REG_EXPAND_SZ) || name.empty())
                             {
                                 return; // the unnamed default value is not an entry
                             }
                             StartupEntry entry;
                             entry.name = WinString::wideToUtf8(name);
                             entry.command = WinString::wideToUtf8(registryString(data));
                             entry.location = source.location;
                             entry.scope = Windows::StartupMath::scopeOf(source.location);
                             applyApproved(entry, approved, name);
                             resolveTarget(entry, seenPaths);
                             entries.push_back(std::move(entry));
                         });
        }
    }

    void readStartupFolder(const KNOWNFOLDERID& folderId,
                           StartupLocation location,
                           HKEY approvedRoot,
                           bool comUsable,
                           std::vector<StartupEntry>& entries,
                           std::unordered_set<std::wstring>& seenPaths)
    {
        const std::wstring folder = knownFolder(folderId);
        if (folder.empty())
        {
            return;
        }
        const ApprovedMap approved = readApproved(approvedRoot, APPROVED_FOLDER_KEY);
        std::error_code error;
        for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        {
            // Its own error code: one entry that can't be stat'ed is skipped, not the end of the listing.
            std::error_code entryError;
            if (it->is_directory(entryError) || entryError)
            {
                continue;
            }
            const std::filesystem::path& file = it->path();
            const std::wstring fileName = file.filename().wstring();
            if (toLower(fileName) == L"desktop.ini")
            {
                continue;
            }
            const bool isShortcut = toLower(file.extension().wstring()) == L".lnk";

            StartupEntry entry;
            entry.name = WinString::wideToUtf8(isShortcut ? file.stem().wstring() : fileName);
            entry.sourcePath = WinString::wideToUtf8(file.wstring());
            entry.location = location;
            entry.scope = Windows::StartupMath::scopeOf(location);
            applyApproved(entry, approved, fileName);
            if (!isShortcut)
            {
                entry.command = std::format("\"{}\"", entry.sourcePath);
                resolveTarget(entry, seenPaths);
            }
            else if (const auto command = comUsable ? Windows::readShortcutCommand(file) : std::nullopt)
            {
                entry.command = *command;
                resolveTarget(entry, seenPaths);
            }
            // else: the shortcut's target couldn't be read; listed with target Unresolved.
            entries.push_back(std::move(entry));
        }
    }
};

WindowsStartupProbe::WindowsStartupProbe() : m_Impl(std::make_unique<Impl>())
{}

WindowsStartupProbe::~WindowsStartupProbe() = default;

StartupCapabilities WindowsStartupProbe::capabilities() const
{
    return {
        .canEnumerate = true,
        .hasEnabledState = true,
        .hasDisabledTime = true,
        .hasPublisher = true,
        .canResolveShortcuts = true,
        .unavailableReason = {},
    };
}

std::vector<StartupEntry> WindowsStartupProbe::enumerate()
{
    std::vector<StartupEntry> entries;
    std::unordered_set<std::wstring> seenPaths;
    m_Impl->readRunKeys(entries, seenPaths);

    const ComScope com;
    if (!com.usable())
    {
        spdlog::debug("WindowsStartupProbe: COM unavailable on this thread; Startup folder shortcuts are listed unresolved");
    }
    m_Impl->readStartupFolder(FOLDERID_Startup, StartupLocation::StartupFolderUser, HKEY_CURRENT_USER, com.usable(), entries, seenPaths);
    m_Impl->readStartupFolder(
        FOLDERID_CommonStartup, StartupLocation::StartupFolderCommon, HKEY_LOCAL_MACHINE, com.usable(), entries, seenPaths);

    // Executables no longer started need no cached publisher.
    std::erase_if(m_Impl->publishers, [&seenPaths](const auto& cached) { return !seenPaths.contains(cached.first); });
    return entries;
}

} // namespace Platform
