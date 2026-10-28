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
#include <appmodel.h>
#include <objbase.h>
#include <objidl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <shlwapi.h>
#include <xmllite.h>
// clang-format on

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "xmllite.lib")

#include "ComPtr.h"
#include "ComScope.h"
#include "WinString.h"
#include "WindowsConversions.h"
#include "WindowsStartupImpact.h"
#include "WindowsStartupProbeMath.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
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

/// Closes a key through the injected RegCloseKey: advapi32's, or a test's fake for its fake keys.
struct InjectedKeyCloser
{
    Windows::StartupProbeFunctions::RegCloseKeyFn close = &::RegCloseKey;

    void operator()(HKEY key) const noexcept
    {
        close(key);
    }
};

using InjectableKey = std::unique_ptr<std::remove_pointer_t<HKEY>, InjectedKeyCloser>;

/// Calls `onValue(name, type, bytes)` for every value of `root\subkey`. A key that doesn't exist or
/// can't be opened has no values.
template<typename OnValue>
void forEachValue(const Windows::StartupProbeFunctions& api, HKEY root, const wchar_t* subkey, REGSAM view, OnValue onValue)
{
    HKEY rawKey = nullptr;
    if (api.openKey(root, subkey, 0, KEY_READ | view, &rawKey) != ERROR_SUCCESS || rawKey == nullptr)
    {
        return;
    }
    const InjectableKey key(rawKey, InjectedKeyCloser{.close = api.closeKey});
    DWORD valueCount = 0;
    DWORD maxNameChars = 0;
    DWORD maxDataBytes = 0;
    if (api.queryInfoKey(
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
        if (api.enumValue(key.get(), index, name.data(), &nameChars, nullptr, &type, data.data(), &dataBytes) != ERROR_SUCCESS)
        {
            continue;
        }
        onValue(std::wstring(name.data(), nameChars), type, std::span<const std::uint8_t>(data.data(), dataBytes));
    }
}

/// Calls `onSubkey(name)` for every subkey of `root\subkey`. A key that doesn't exist or can't be
/// opened has none.
template<typename OnSubkey>
void forEachSubkey(const Windows::StartupProbeFunctions& api, HKEY root, const std::wstring& subkey, OnSubkey onSubkey)
{
    HKEY rawKey = nullptr;
    if (api.openKey(root, subkey.c_str(), 0, KEY_READ, &rawKey) != ERROR_SUCCESS || rawKey == nullptr)
    {
        return;
    }
    const InjectableKey key(rawKey, InjectedKeyCloser{.close = api.closeKey});
    DWORD subkeyCount = 0;
    DWORD maxNameChars = 0;
    if (api.queryInfoKey(
            key.get(), nullptr, nullptr, nullptr, &subkeyCount, &maxNameChars, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) !=
        ERROR_SUCCESS)
    {
        return;
    }
    std::vector<wchar_t> name(maxNameChars + 1);
    DWORD index = 0;
    while (index < subkeyCount)
    {
        auto nameChars = static_cast<DWORD>(name.size());
        // A subkey added since RegQueryInfoKeyW with a longer name (ERROR_MORE_DATA) is skipped this pass.
        if (api.enumKey(key.get(), index, name.data(), &nameChars, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
        {
            onSubkey(std::wstring(name.data(), nameChars));
        }
        ++index;
    }
}

/// A REG_DWORD or REG_QWORD value as a number; nullopt for any other type or size.
[[nodiscard]] std::optional<std::uint64_t> registryNumber(DWORD type, std::span<const std::uint8_t> data)
{
    if (type == REG_DWORD && data.size() == sizeof(std::uint32_t))
    {
        std::uint32_t value = 0;
        std::memcpy(&value, data.data(), sizeof(value));
        return value;
    }
    if (type == REG_QWORD && data.size() == sizeof(std::uint64_t))
    {
        std::uint64_t value = 0;
        std::memcpy(&value, data.data(), sizeof(value));
        return value;
    }
    return std::nullopt;
}

/// One packaged startup task's recorded state: its key's name (the TaskId), State and LastDisabledTime.
struct PackagedTaskRecord
{
    std::wstring taskId;
    std::uint32_t state = 0;
    std::uint64_t lastDisabledUnixSeconds = 0;
};

/// A packaged startup task as listed: its name, and its program's full path.
struct ResolvedPackagedTask
{
    std::wstring taskId;
    std::string name;
    std::string executablePath;
    std::string iconPath; ///< The manifest's logo under the package's folder (#1800); empty when it names none.
};

/// What a package's manifest gives its startup tasks' entries, names resolved.
struct ResolvedPackage
{
    std::string publisher;
    std::vector<ResolvedPackagedTask> tasks;
};

/// The longest AppxManifest.xml read; real ones are tens of kilobytes.
constexpr std::uintmax_t MAX_MANIFEST_BYTES = std::uintmax_t{8} * 1024U * 1024U;

[[nodiscard]] std::optional<std::string> readSmallFile(const std::filesystem::path& path)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > MAX_MANIFEST_BYTES)
    {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (!file.read(contents.data(), static_cast<std::streamsize>(contents.size())))
    {
        return std::nullopt;
    }
    return contents;
}

[[nodiscard]] ApprovedMap readApproved(const Windows::StartupProbeFunctions& api, HKEY root, const wchar_t* subkey)
{
    ApprovedMap approved;
    if (subkey == nullptr)
    {
        return approved;
    }
    forEachValue(api,
                 root,
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

[[nodiscard]] std::wstring knownFolder(const Windows::StartupProbeFunctions& api, const KNOWNFOLDERID& id)
{
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(api.knownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) && path != nullptr)
    {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

/// What WindowsStartupProbe reads from an executable's version resource.
struct VersionStrings
{
    std::string companyName;     ///< The publisher.
    std::string fileDescription; ///< The program's name, as Settings > Apps > Startup shows it (#1873).
};

/// One string from a version resource: the first listed translation that has it, then US English in
/// Unicode and Windows-1252; trailing spaces dropped. Empty when no translation has it.
[[nodiscard]] std::string
readVersionString(std::span<const std::byte> block, const std::vector<std::pair<WORD, WORD>>& translations, const wchar_t* key)
{
    std::size_t index = 0;
    while (index < translations.size())
    {
        const auto [language, codePage] = translations[index];
        ++index;
        const std::wstring query = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\{}", language, codePage, key);
        void* value = nullptr;
        UINT chars = 0;
        if (VerQueryValueW(block.data(), query.c_str(), &value, &chars) != FALSE && value != nullptr && chars > 0)
        {
            std::string text = WinString::wideToUtf8(static_cast<const wchar_t*>(value));
            if (const auto end = text.find_last_not_of(' '); end != std::string::npos)
            {
                text.erase(end + 1);
                return text;
            }
        }
    }
    return {};
}

/// The CompanyName and FileDescription from a file's version resource; empty when the file has none.
[[nodiscard]] VersionStrings readVersionStrings(const std::wstring& path)
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
    std::vector<std::pair<WORD, WORD>> translations;
    void* translationData = nullptr;
    UINT translationBytes = 0;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", &translationData, &translationBytes) != FALSE &&
        translationData != nullptr)
    {
        const std::span listed(static_cast<const Translation*>(translationData), translationBytes / sizeof(Translation));
        translations.reserve(listed.size() + 2);
        std::size_t index = 0;
        while (index < listed.size())
        {
            translations.emplace_back(listed[index].language, listed[index].codePage);
            ++index;
        }
    }
    translations.emplace_back(WORD{0x0409}, WORD{0x04B0});
    translations.emplace_back(WORD{0x0409}, WORD{0x04E4});
    return {.companyName = readVersionString(block, translations, L"CompanyName"),
            .fileDescription = readVersionString(block, translations, L"FileDescription")};
}

/// The version strings read for one file, kept until the file's last-write time changes.
struct CachedVersionStrings
{
    std::uint64_t lastWrite = 0;
    VersionStrings strings;
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

namespace
{

/// The current element's attributes, by local name (namespace prefixes dropped).
[[nodiscard]] std::unordered_map<std::wstring, std::wstring> readAttributes(IXmlReader& reader)
{
    std::unordered_map<std::wstring, std::wstring> attributes;
    HRESULT moved = reader.MoveToFirstAttribute();
    while (moved == S_OK)
    {
        const wchar_t* name = nullptr;
        const wchar_t* value = nullptr;
        if (SUCCEEDED(reader.GetLocalName(&name, nullptr)) && SUCCEEDED(reader.GetValue(&value, nullptr)) && name != nullptr &&
            value != nullptr)
        {
            attributes.emplace(name, value);
        }
        moved = reader.MoveToNextAttribute();
    }
    reader.MoveToElement();
    return attributes;
}

[[nodiscard]] std::string attribute(const std::unordered_map<std::wstring, std::wstring>& attributes, const wchar_t* name)
{
    const auto it = attributes.find(name);
    return it == attributes.end() ? std::string{} : WinString::wideToUtf8(it->second);
}

[[nodiscard]] std::string trimmed(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    text.erase(text.find_last_not_of(" \t\r\n") + 1);
    text.erase(0, first);
    return text;
}

} // namespace

std::optional<PackageManifest> parsePackageManifest(std::string_view xml)
{
    if (xml.size() > std::numeric_limits<UINT>::max())
    {
        return std::nullopt;
    }
    ComPtr<IStream> stream;
    *stream.releaseAndGetAddressOf() = SHCreateMemStream(reinterpret_cast<const BYTE*>(xml.data()), static_cast<UINT>(xml.size()));
    ComPtr<IXmlReader> reader;
    // The reinterpret_cast is COM's out-parameter convention, as in readShortcutCommandWide.
    if (!stream || FAILED(CreateXmlReader(IID_IXmlReader, reinterpret_cast<void**>(reader.releaseAndGetAddressOf()), nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) || FAILED(reader->SetInput(stream.get())))
    {
        return std::nullopt;
    }

    struct Application
    {
        std::string executable;
        std::string displayName; ///< VisualElements DisplayName.
        std::string logo;        ///< VisualElements Square44x44Logo (#1800).
    };
    struct Task
    {
        PackageStartupTask task;
        std::optional<std::size_t> application; ///< Index into applications.
    };
    PackageManifest manifest;
    std::vector<Application> applications;
    std::vector<Task> tasks;
    std::vector<std::wstring> path;                   ///< The open elements' local names.
    std::optional<std::size_t> applicationDepth;      ///< path.size() inside the open Application.
    std::optional<std::size_t> startupExtensionDepth; ///< path.size() inside an open windows.startupTask Extension.
    std::string extensionExecutable;

    XmlNodeType type = XmlNodeType_None;
    HRESULT read = reader->Read(&type);
    while (read == S_OK)
    {
        if (type == XmlNodeType_Element)
        {
            const wchar_t* rawName = nullptr;
            if (FAILED(reader->GetLocalName(&rawName, nullptr)) || rawName == nullptr)
            {
                return std::nullopt;
            }
            const std::wstring name = rawName;
            const bool empty = reader->IsEmptyElement() != FALSE;
            const auto attributes = readAttributes(*reader.get());
            const std::wstring parent = path.empty() ? std::wstring{} : path.back();
            if (name == L"Application" && parent == L"Applications")
            {
                applications.push_back({.executable = attribute(attributes, L"Executable"), .displayName = {}, .logo = {}});
                applicationDepth = empty ? std::nullopt : std::optional(path.size() + 1);
            }
            else if (name == L"VisualElements" && applicationDepth && path.size() == *applicationDepth)
            {
                applications.back().displayName = attribute(attributes, L"DisplayName");
                applications.back().logo = attribute(attributes, L"Square44x44Logo");
            }
            else if (name == L"Extension" && attribute(attributes, L"Category") == "windows.startupTask")
            {
                extensionExecutable = attribute(attributes, L"Executable");
                startupExtensionDepth = empty ? std::nullopt : std::optional(path.size() + 1);
            }
            else if (name == L"StartupTask" && startupExtensionDepth && path.size() == *startupExtensionDepth)
            {
                Task task;
                task.task.taskId = attribute(attributes, L"TaskId");
                task.task.displayName = attribute(attributes, L"DisplayName");
                task.task.executable = extensionExecutable;
                task.task.enabledByDefault = attribute(attributes, L"Enabled") == "true";
                if (applicationDepth)
                {
                    task.application = applications.size() - 1;
                }
                if (!task.task.taskId.empty())
                {
                    tasks.push_back(std::move(task));
                }
            }
            if (!empty)
            {
                path.push_back(name);
            }
        }
        else if (type == XmlNodeType_EndElement)
        {
            if (startupExtensionDepth && path.size() == *startupExtensionDepth)
            {
                startupExtensionDepth.reset();
                extensionExecutable.clear();
            }
            if (applicationDepth && path.size() == *applicationDepth)
            {
                applicationDepth.reset();
            }
            if (!path.empty())
            {
                path.pop_back();
            }
        }
        else if ((type == XmlNodeType_Text || type == XmlNodeType_CDATA) && path.size() >= 2 && path[path.size() - 2] == L"Properties")
        {
            const wchar_t* value = nullptr;
            if (SUCCEEDED(reader->GetValue(&value, nullptr)) && value != nullptr)
            {
                if (path.back() == L"DisplayName")
                {
                    manifest.displayName += WinString::wideToUtf8(value);
                }
                else if (path.back() == L"PublisherDisplayName")
                {
                    manifest.publisherDisplayName += WinString::wideToUtf8(value);
                }
                else if (path.back() == L"Logo")
                {
                    manifest.logo += WinString::wideToUtf8(value);
                }
            }
        }
        read = reader->Read(&type);
    }
    if (read != S_FALSE) // S_FALSE: the end of the document; anything else is malformed XML
    {
        return std::nullopt;
    }

    manifest.displayName = trimmed(std::move(manifest.displayName));
    manifest.publisherDisplayName = trimmed(std::move(manifest.publisherDisplayName));
    manifest.logo = trimmed(std::move(manifest.logo));
    manifest.startupTasks.reserve(tasks.size());
    for (Task& task : tasks)
    {
        if (task.application)
        {
            const Application& application = applications[*task.application];
            if (task.task.displayName.empty())
            {
                task.task.displayName = application.displayName;
            }
            if (task.task.executable.empty())
            {
                task.task.executable = application.executable;
            }
            task.task.logo = application.logo;
        }
        if (task.task.logo.empty())
        {
            task.task.logo = manifest.logo;
        }
        manifest.startupTasks.push_back(std::move(task.task));
    }
    return manifest;
}

} // namespace Windows

struct WindowsStartupProbe::Impl
{
    Windows::StartupProbeFunctions api;
    std::unordered_map<std::wstring, CachedVersionStrings> versionStrings; ///< By lower-cased path.
    /// By lower-cased package full name, which names the version: an update reads the new manifest.
    std::unordered_map<std::wstring, ResolvedPackage> packages;

    /// @p value, or the string its "ms-resource:" reference names in package @p fullName's resources;
    /// empty when that can't be loaded.
    [[nodiscard]] std::string
    resolveManifestString(const std::wstring& fullName, std::string_view packageName, const std::string& value) const
    {
        const std::string uri = Windows::StartupMath::resourceUri(value, packageName);
        if (uri.empty())
        {
            return value;
        }
        const std::wstring source = std::format(L"@{{{}?{}}}", fullName, WinString::utf8ToWide(uri));
        std::array<wchar_t, 512> text{};
        if (FAILED(api.loadIndirectString(source.c_str(), text.data(), static_cast<UINT>(text.size()), nullptr)))
        {
            return {};
        }
        return WinString::wideToUtf8(text.data());
    }

    /// The full names of @p family's packages installed for the current user.
    [[nodiscard]] std::vector<std::wstring> packageFullNames(const std::wstring& family) const
    {
        UINT32 count = 0;
        UINT32 bufferChars = 0;
        if (api.packagesByFamily(family.c_str(), &count, nullptr, &bufferChars, nullptr) != ERROR_INSUFFICIENT_BUFFER || count == 0)
        {
            return {};
        }
        std::vector<PWSTR> names(count);
        std::vector<WCHAR> buffer(bufferChars);
        if (api.packagesByFamily(family.c_str(), &count, names.data(), &bufferChars, buffer.data()) != ERROR_SUCCESS)
        {
            return {};
        }
        std::vector<std::wstring> fullNames;
        std::size_t index = 0;
        while (index < count && index < names.size())
        {
            if (names[index] != nullptr)
            {
                fullNames.emplace_back(names[index]);
            }
            ++index;
        }
        return fullNames;
    }

    /// Package @p fullName's startup tasks, named, from its installed manifest: cached until the
    /// package's version changes; nullptr when its folder or manifest can't be read.
    [[nodiscard]] const ResolvedPackage*
    resolvePackage(const std::wstring& fullName, std::string_view packageName, std::unordered_set<std::wstring>& seenPackages)
    {
        const std::wstring key = toLower(fullName);
        seenPackages.insert(key);
        if (const auto it = packages.find(key); it != packages.end())
        {
            return &it->second;
        }
        UINT32 pathChars = 0;
        if (api.packagePath(fullName.c_str(), &pathChars, nullptr) != ERROR_INSUFFICIENT_BUFFER || pathChars == 0)
        {
            return nullptr;
        }
        std::wstring folder(pathChars, L'\0');
        if (api.packagePath(fullName.c_str(), &pathChars, folder.data()) != ERROR_SUCCESS)
        {
            return nullptr;
        }
        folder.resize(std::wcslen(folder.c_str()));
        const std::filesystem::path root(folder);
        const auto xml = readSmallFile(root / L"AppxManifest.xml");
        const auto manifest = xml ? Windows::parsePackageManifest(*xml) : std::nullopt;
        if (!manifest)
        {
            return nullptr; // not cached: a manifest mid-update is read again next pass
        }

        ResolvedPackage resolved;
        resolved.publisher = resolveManifestString(fullName, packageName, manifest->publisherDisplayName);
        for (const Windows::PackageStartupTask& task : manifest->startupTasks)
        {
            std::string name = resolveManifestString(fullName, packageName, task.displayName);
            if (name.empty())
            {
                name = resolveManifestString(fullName, packageName, manifest->displayName);
            }
            if (name.empty())
            {
                name = task.taskId;
            }
            std::string executablePath;
            if (!task.executable.empty())
            {
                executablePath = WinString::wideToUtf8((root / WinString::utf8ToWide(task.executable)).make_preferred().wstring());
            }
            std::string iconPath;
            if (!task.logo.empty())
            {
                iconPath = WinString::wideToUtf8((root / WinString::utf8ToWide(task.logo)).make_preferred().wstring());
            }
            resolved.tasks.push_back({.taskId = WinString::utf8ToWide(task.taskId),
                                      .name = std::move(name),
                                      .executablePath = std::move(executablePath),
                                      .iconPath = std::move(iconPath)});
        }
        return &packages.insert_or_assign(key, std::move(resolved)).first->second;
    }

    /// The startup tasks recorded for @p family: each task key's State and LastDisabledTime. A key
    /// without a State is not a startup task (a package's other app data lives beside them).
    [[nodiscard]] std::vector<PackagedTaskRecord> readTaskRecords(const std::wstring& family) const
    {
        std::vector<PackagedTaskRecord> records;
        const std::wstring familyKey = std::format(L"{}\\{}", Windows::StartupMath::PACKAGED_TASKS_KEY, family);
        forEachSubkey(api,
                      HKEY_CURRENT_USER,
                      familyKey,
                      [&](const std::wstring& taskId)
                      {
                          const std::wstring taskKey = std::format(L"{}\\{}", familyKey, taskId);
                          std::optional<std::uint64_t> state;
                          std::uint64_t lastDisabled = 0;
                          forEachValue(api,
                                       HKEY_CURRENT_USER,
                                       taskKey.c_str(),
                                       0,
                                       [&](const std::wstring& name, DWORD type, std::span<const std::uint8_t> data)
                                       {
                                           const std::wstring lower = toLower(name);
                                           if (lower == L"state")
                                           {
                                               state = registryNumber(type, data);
                                           }
                                           else if (lower == L"lastdisabledtime")
                                           {
                                               lastDisabled = registryNumber(type, data).value_or(0);
                                           }
                                       });
                          if (state)
                          {
                              records.push_back(
                                  {.taskId = taskId, .state = static_cast<std::uint32_t>(*state), .lastDisabledUnixSeconds = lastDisabled});
                          }
                      });
        return records;
    }

    /// The current user's packaged apps' startup tasks (#1798), as Task Manager lists them: a task with
    /// recorded state whose package is installed and whose manifest still declares it; one entry per
    /// family and TaskId, whatever versions or architectures of the package are installed.
    void readPackagedTasks(std::vector<StartupEntry>& entries)
    {
        std::unordered_set<std::wstring> seenPackages;
        std::unordered_set<std::wstring> listed; ///< Lower-cased "family!TaskId".
        forEachSubkey(api,
                      HKEY_CURRENT_USER,
                      Windows::StartupMath::PACKAGED_TASKS_KEY,
                      [&](const std::wstring& family)
                      {
                          const std::vector<PackagedTaskRecord> records = readTaskRecords(family);
                          if (records.empty())
                          {
                              return;
                          }
                          const std::string familyUtf8 = WinString::wideToUtf8(family);
                          const std::string_view packageName = Windows::StartupMath::packageNameOfFamily(familyUtf8);
                          const std::vector<std::wstring> fullNames = packageFullNames(family);
                          for (const PackagedTaskRecord& record : records)
                          {
                              if (!listed.insert(toLower(std::format(L"{}!{}", family, record.taskId))).second)
                              {
                                  continue;
                              }
                              const ResolvedPackagedTask* task = nullptr;
                              const ResolvedPackage* package = nullptr;
                              std::size_t index = 0;
                              while (task == nullptr && index < fullNames.size())
                              {
                                  package = resolvePackage(fullNames[index], packageName, seenPackages);
                                  if (package != nullptr)
                                  {
                                      const auto found = std::ranges::find_if(package->tasks,
                                                                              [&](const ResolvedPackagedTask& t)
                                                                              { return toLower(t.taskId) == toLower(record.taskId); });
                                      task = (found != package->tasks.end()) ? &*found : nullptr;
                                  }
                                  ++index;
                              }
                              if (task == nullptr)
                              {
                                  continue; // not installed, or no longer declared: Task Manager doesn't list it either
                              }
                              entries.push_back(makePackagedEntry(*package, *task, record, familyUtf8));
                          }
                      });
        // Packages no longer installed (or updated to a new version) need no cached manifest.
        std::erase_if(packages, [&seenPackages](const auto& cached) { return !seenPackages.contains(cached.first); });
    }

    [[nodiscard]] static StartupEntry makePackagedEntry(const ResolvedPackage& package,
                                                        const ResolvedPackagedTask& task,
                                                        const PackagedTaskRecord& record,
                                                        const std::string& family)
    {
        StartupEntry entry;
        entry.name = task.name;
        entry.registrationName = WinString::wideToUtf8(record.taskId); // the task key's name, as written
        entry.packageFamily = family;
        entry.publisher = package.publisher;
        entry.location = StartupLocation::PackagedApp;
        entry.scope = Windows::StartupMath::scopeOf(StartupLocation::PackagedApp);
        entry.enabled = Windows::StartupMath::packagedTaskEnabled(record.state);
        entry.setByPolicy = Windows::StartupMath::packagedTaskSetByPolicy(record.state);
        entry.disabledAtUnixSeconds = entry.enabled ? 0 : record.lastDisabledUnixSeconds;
        entry.iconPath = task.iconPath;
        if (!task.executablePath.empty())
        {
            entry.executablePath = task.executablePath;
            entry.command = std::format("\"{}\"", task.executablePath);
            const DWORD attributes = GetFileAttributesW(WinString::utf8ToWide(task.executablePath).c_str());
            entry.target = (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                             ? StartupTargetState::Present
                             : StartupTargetState::Missing;
        }
        return entry;
    }

    /// Sets `entry`'s program name and the name it is shown by (#1971): the program name, except that a
    /// RunOnce entry is shown by its registry value name. The entry's location and registrationName
    /// must be set first.
    static void nameEntry(StartupEntry& entry, std::string programName)
    {
        entry.name = Windows::StartupMath::startupEntryName(entry.location, programName, entry.registrationName);
        entry.programName = std::move(programName);
    }

    /// Fills `entry`'s executable, target state and publisher from its command, and names it (#1873):
    /// its program's FileDescription, else the program's file name, else @p fallbackName. A RunOnce
    /// entry is shown by its value name instead, with the program's name kept in programName (#1971).
    void resolveTarget(StartupEntry& entry, std::string_view fallbackName, std::unordered_set<std::wstring>& seenPaths)
    {
        nameEntry(entry, Windows::StartupMath::startupDisplayName({}, {}, fallbackName));
        if (entry.command.empty())
        {
            return; // a shortcut whose target couldn't be read
        }
        entry.executablePath = Windows::resolveStartupExecutable(entry.command);
        nameEntry(entry, Windows::StartupMath::startupDisplayName({}, entry.executablePath, fallbackName));
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
        const std::uint64_t lastWrite = Windows::fileTimeTicks(attributes.ftLastWriteTime);
        auto [it, inserted] = versionStrings.try_emplace(key);
        if (inserted || it->second.lastWrite != lastWrite)
        {
            it->second = {.lastWrite = lastWrite, .strings = readVersionStrings(path)};
        }
        entry.publisher = it->second.strings.companyName;
        nameEntry(entry, Windows::StartupMath::startupDisplayName(it->second.strings.fileDescription, entry.executablePath, fallbackName));
    }

    void readRunKeys(std::vector<StartupEntry>& entries, std::unordered_set<std::wstring>& seenPaths)
    {
        for (const RunSource& source : RUN_SOURCES)
        {
            const ApprovedMap approved = readApproved(api, source.root, source.approvedSubkey);
            forEachValue(api,
                         source.root,
                         source.subkey,
                         source.view,
                         [&](const std::wstring& name, DWORD type, std::span<const std::uint8_t> data)
                         {
                             if ((type != REG_SZ && type != REG_EXPAND_SZ) || name.empty())
                             {
                                 return; // the unnamed default value is not an entry
                             }
                             StartupEntry entry;
                             entry.registrationName = WinString::wideToUtf8(name);
                             entry.command = WinString::wideToUtf8(registryString(data));
                             entry.location = source.location;
                             entry.scope = Windows::StartupMath::scopeOf(source.location);
                             applyApproved(entry, approved, name);
                             resolveTarget(entry, entry.registrationName, seenPaths);
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
        const std::wstring folder = knownFolder(api, folderId);
        if (folder.empty())
        {
            return;
        }
        const ApprovedMap approved = readApproved(api, approvedRoot, APPROVED_FOLDER_KEY);
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
            entry.registrationName = WinString::wideToUtf8(fileName);
            entry.sourcePath = WinString::wideToUtf8(file.wstring());
            entry.location = location;
            entry.scope = Windows::StartupMath::scopeOf(location);
            applyApproved(entry, approved, fileName);
            if (!isShortcut)
            {
                entry.command = std::format("\"{}\"", entry.sourcePath);
            }
            else if (const auto command = comUsable ? Windows::readShortcutCommand(file) : std::nullopt)
            {
                entry.command = *command;
            }
            // else: the shortcut's target couldn't be read; listed with target Unresolved, by its name.
            // A shortcut is named without its extension when its program can't name it.
            resolveTarget(entry, WinString::wideToUtf8(isShortcut ? file.stem().wstring() : fileName), seenPaths);
            entries.push_back(std::move(entry));
        }
    }
};

WindowsStartupProbe::WindowsStartupProbe() : WindowsStartupProbe(Windows::StartupProbeFunctions{})
{}

WindowsStartupProbe::WindowsStartupProbe(const Windows::StartupProbeFunctions& functions) : m_Impl(std::make_unique<Impl>())
{
    m_Impl->api = functions;
}

WindowsStartupProbe::~WindowsStartupProbe() = default;

StartupCapabilities WindowsStartupProbe::capabilities() const
{
    return {
        .canEnumerate = true,
        .hasEnabledState = true,
        .hasDisabledTime = true,
        .hasPublisher = true,
        .canResolveShortcuts = true,
        .hasImpact = true,
        .unavailableReason = {},
    };
}

StartupImpactReading WindowsStartupProbe::readImpact()
{
    StartupImpactReading reading = WindowsStartupImpact::readNewestSignIn(WindowsStartupImpact::systemFunctions());
    if (!reading.unavailableReason.empty())
    {
        spdlog::debug("Startup impact: {}", reading.unavailableReason);
    }
    return reading;
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
    // After the ComScope: resolving a manifest's ms-resource names loads the package's resources.
    m_Impl->readPackagedTasks(entries);

    // Executables no longer started need no cached publisher.
    std::erase_if(m_Impl->versionStrings, [&seenPaths](const auto& cached) { return !seenPaths.contains(cached.first); });
    return entries;
}

} // namespace Platform
