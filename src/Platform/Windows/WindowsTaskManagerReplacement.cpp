#include "WindowsTaskManagerReplacement.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/TaskManagerReplacement.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"

#include <cstddef>
#include <cwchar>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// Closes registry keys with RegCloseKey.
struct RegistryKeyTraits
{
    using Type = HKEY;

    [[nodiscard]] static Type invalid() noexcept
    {
        return nullptr;
    }
    [[nodiscard]] static bool isValid(Type key) noexcept
    {
        return key != nullptr;
    }
    static void close(Type key) noexcept
    {
        RegCloseKey(key);
    }
};
using UniqueKey = UniqueResource<RegistryKeyTraits>;

/// The 64-bit view, which is where Windows reads IFEO for the 64-bit taskmgr.exe.
constexpr REGSAM VIEW = KEY_WOW64_64KEY;

/// Whether @p narrow and @p wideText are the same ASCII text: the value names below are
/// TaskManagerReplacement.h's, spelled for the registry's wide calls.
[[nodiscard]] constexpr bool sameAscii(std::string_view narrow, std::wstring_view wideText) noexcept
{
    if (narrow.size() != wideText.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < narrow.size(); ++i)
    {
        if (static_cast<wchar_t>(narrow[i]) != wideText[i])
        {
            return false;
        }
    }
    return true;
}

constexpr const wchar_t* DEBUGGER = L"Debugger";
constexpr const wchar_t* MARKER = L"TaskSmackCreatedKey";
static_assert(sameAscii(TaskManagerReplacement::DEBUGGER_VALUE, DEBUGGER));
static_assert(sameAscii(TaskManagerReplacement::CREATED_MARKER_VALUE, MARKER));

[[nodiscard]] std::string failure(std::string_view what, LSTATUS status)
{
    return std::format("{} ({}, error {})", what, std::system_category().message(static_cast<int>(status)), status);
}

/// @p subKey of @p root opened in the 64-bit view with @p access; a key that isn't there is nullopt.
[[nodiscard]] std::expected<std::optional<UniqueKey>, std::string> openKey(HKEY root, const std::wstring& subKey, REGSAM access)
{
    UniqueKey key;
    const LSTATUS status = RegOpenKeyExW(root, subKey.c_str(), 0, access | VIEW, key.put());
    if (status == ERROR_FILE_NOT_FOUND)
    {
        return std::optional<UniqueKey>{};
    }
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(failure("couldn't open the key", status));
    }
    return std::optional<UniqueKey>{std::move(key)};
}

/// Writes @p name, of @p type, in @p subKey of @p root, which must be there.
[[nodiscard]] std::expected<void, std::string>
setValue(HKEY root, const std::wstring& subKey, const wchar_t* name, DWORD type, const void* data, DWORD bytes)
{
    const auto key = openKey(root, subKey, KEY_SET_VALUE);
    if (!key.has_value())
    {
        return std::unexpected(key.error());
    }
    if (!key->has_value())
    {
        return std::unexpected(std::string("the key isn't there"));
    }
    const LSTATUS status = RegSetValueExW((*key)->get(), name, 0, type, static_cast<const BYTE*>(data), bytes);
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(failure("couldn't write the value", status));
    }
    return {};
}

/// Removes @p name from @p subKey of @p root; a key or value that isn't there is no error.
[[nodiscard]] std::expected<void, std::string> deleteValue(HKEY root, const std::wstring& subKey, const wchar_t* name)
{
    const auto key = openKey(root, subKey, KEY_SET_VALUE);
    if (!key.has_value())
    {
        return std::unexpected(key.error());
    }
    if (!key->has_value())
    {
        return {};
    }
    const LSTATUS status = RegDeleteValueW((*key)->get(), name);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
    {
        return std::unexpected(failure("couldn't remove the value", status));
    }
    return {};
}

} // namespace

WindowsIfeoRegistry::WindowsIfeoRegistry()
    : WindowsIfeoRegistry(HKEY_LOCAL_MACHINE, WinString::utf8ToWide(TaskManagerReplacement::IFEO_KEY))
{}

WindowsIfeoRegistry::WindowsIfeoRegistry(HKEY root, std::wstring subKey) : m_Root(root), m_SubKey(std::move(subKey))
{}

std::expected<std::optional<std::string>, std::string> WindowsIfeoRegistry::readDebugger()
{
    const auto key = openKey(m_Root, m_SubKey, KEY_QUERY_VALUE);
    if (!key.has_value())
    {
        return std::unexpected(key.error());
    }
    if (!key->has_value())
    {
        return std::optional<std::string>{}; // no key: Task Manager is itself
    }
    // Text as written, unexpanded: TaskSmack writes REG_SZ, a tool may have written REG_EXPAND_SZ.
    constexpr DWORD FLAGS = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        DWORD bytes = 0;
        LSTATUS status = RegGetValueW((*key)->get(), nullptr, DEBUGGER, FLAGS, nullptr, nullptr, &bytes);
        if (status == ERROR_FILE_NOT_FOUND)
        {
            return std::optional<std::string>{};
        }
        if (status == ERROR_UNSUPPORTED_TYPE)
        {
            return std::optional<std::string>{"(a value that isn't text)"}; // another tool's: not TaskSmack's to touch
        }
        if (status != ERROR_SUCCESS)
        {
            return std::unexpected(failure("couldn't read the Debugger value", status));
        }
        std::wstring value((bytes / sizeof(wchar_t)) + 1, L'\0');
        auto size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        status = RegGetValueW((*key)->get(), nullptr, DEBUGGER, FLAGS, nullptr, value.data(), &size);
        if (status == ERROR_MORE_DATA)
        {
            continue; // it grew between the two calls
        }
        if (status != ERROR_SUCCESS)
        {
            return std::unexpected(failure("couldn't read the Debugger value", status));
        }
        value.resize(std::wcslen(value.c_str()));
        return std::optional<std::string>{WinString::wideToUtf8(value)};
    }
    return std::unexpected(std::string("the Debugger value kept changing while it was read"));
}

std::expected<bool, std::string> WindowsIfeoRegistry::createKey()
{
    UniqueKey key;
    DWORD disposition = 0;
    const LSTATUS status = RegCreateKeyExW(m_Root,
                                           m_SubKey.c_str(),
                                           0,
                                           nullptr,
                                           REG_OPTION_NON_VOLATILE,
                                           KEY_QUERY_VALUE | KEY_SET_VALUE | VIEW,
                                           nullptr,
                                           key.put(),
                                           &disposition);
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(failure("couldn't create the key", status));
    }
    return disposition == REG_CREATED_NEW_KEY;
}

std::expected<void, std::string> WindowsIfeoRegistry::writeDebugger(std::string_view value)
{
    const std::wstring text = WinString::utf8ToWide(value);
    return setValue(m_Root, m_SubKey, DEBUGGER, REG_SZ, text.c_str(), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
}

std::expected<void, std::string> WindowsIfeoRegistry::deleteDebugger()
{
    return deleteValue(m_Root, m_SubKey, DEBUGGER);
}

std::expected<bool, std::string> WindowsIfeoRegistry::readCreatedMarker()
{
    const auto key = openKey(m_Root, m_SubKey, KEY_QUERY_VALUE);
    if (!key.has_value())
    {
        return std::unexpected(key.error());
    }
    if (!key->has_value())
    {
        return false;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW((*key)->get(), nullptr, MARKER, RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_UNSUPPORTED_TYPE)
    {
        return false; // no mark, or not TaskSmack's: the key isn't TaskSmack's to delete
    }
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(failure("couldn't read TaskSmack's mark", status));
    }
    return value == 1;
}

std::expected<void, std::string> WindowsIfeoRegistry::writeCreatedMarker()
{
    const DWORD one = 1;
    return setValue(m_Root, m_SubKey, MARKER, REG_DWORD, &one, sizeof(one));
}

std::expected<void, std::string> WindowsIfeoRegistry::deleteCreatedMarker()
{
    return deleteValue(m_Root, m_SubKey, MARKER);
}

std::expected<bool, std::string> WindowsIfeoRegistry::deleteKeyIfEmpty()
{
    {
        const auto key = openKey(m_Root, m_SubKey, KEY_QUERY_VALUE);
        if (!key.has_value())
        {
            return std::unexpected(key.error());
        }
        if (!key->has_value())
        {
            return false;
        }
        DWORD subKeys = 0;
        DWORD values = 0;
        const LSTATUS status = RegQueryInfoKeyW(
            (*key)->get(), nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS)
        {
            return std::unexpected(failure("couldn't look in the key", status));
        }
        if (subKeys != 0 || values != 0)
        {
            return false; // something of another tool's is in it: it stays
        }
    }
    const LSTATUS status = RegDeleteKeyExW(m_Root, m_SubKey.c_str(), VIEW, 0);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
    {
        return std::unexpected(failure("couldn't delete the key", status));
    }
    return status == ERROR_SUCCESS;
}

Broker::ActionResult handleHelperRequest(const Broker::TaskManagerReplacementRequest& request,
                                         const std::wstring& besideTaskSmack,
                                         TaskManagerReplacement::IIfeoRegistry& registry,
                                         SameFileFn sameFile)
{
    if (request.change == Broker::TaskManagerChange::Clear)
    {
        return Broker::ActionResult::from(TaskManagerReplacement::applyClear(registry));
    }
    if (const auto valid = TaskManagerReplacement::validateTaskSmackPath(request.path); !valid.has_value())
    {
        return Broker::ActionResult::from(
            ProcessActionResult::error("TaskSmack's helper won't replace Task Manager with that program: " + std::string(valid.error())));
    }
    const std::string served = WinString::wideToUtf8(besideTaskSmack);
    if (besideTaskSmack.empty() || sameFile == nullptr || !TaskManagerReplacement::samePath(request.path, served) ||
        !sameFile(WinString::utf8ToWide(request.path), besideTaskSmack))
    {
        return Broker::ActionResult::from(
            ProcessActionResult::error("TaskSmack's helper replaces Task Manager only with the TaskSmack.exe beside it, " + served));
    }
    return Broker::ActionResult::from(TaskManagerReplacement::applySet(registry, request.path, request.replaceOther));
}

std::vector<std::string> installFolders()
{
    std::vector<std::string> folders;
    for (const wchar_t* name : {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)"})
    {
        std::wstring value(MAX_PATH, L'\0');
        DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (length > value.size())
        {
            value.resize(length);
            length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        }
        if (length == 0 || length >= value.size())
        {
            continue;
        }
        value.resize(length);
        folders.push_back(WinString::wideToUtf8(value));
    }
    return folders;
}

bool fileExists(std::string_view path)
{
    const DWORD attributes = GetFileAttributesW(WinString::utf8ToWide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

} // namespace Platform::Windows
