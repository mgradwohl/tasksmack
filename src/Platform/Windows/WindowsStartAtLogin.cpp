#include "WindowsStartAtLogin.h"

#include "Domain/StartAtLogin.h"
#include "Platform/IStartAtLogin.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include "WinString.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <knownfolders.h>
#include <objbase.h> // CoTaskMemFree
#include <shlobj.h>

namespace Platform
{

namespace
{

/// Closes a key through the injected RegCloseKey: advapi32's, or a test's fake for its fake keys.
struct InjectedKeyCloser
{
    Windows::StartAtLoginFunctions::RegCloseKeyFn close = &::RegCloseKey;

    void operator()(HKEY key) const noexcept
    {
        close(key);
    }
};

using InjectableKey = std::unique_ptr<std::remove_pointer_t<HKEY>, InjectedKeyCloser>;

[[nodiscard]] std::string errorText(LSTATUS status)
{
    return std::system_category().message(static_cast<int>(status));
}

/// The longest path the wide Win32 calls return, in characters (an extended-length path).
constexpr DWORD LONGEST_PATH = 32'768;

/// This process's image, through the injected GetModuleFileNameW: MAX_PATH first, then the longest path.
[[nodiscard]] std::optional<std::wstring> ownImage(Windows::StartAtLoginFunctions::GetModuleFileNameFn moduleFileName)
{
    for (const DWORD size : {DWORD{MAX_PATH}, LONGEST_PATH})
    {
        std::wstring path(size, L'\0');
        const DWORD written = moduleFileName(nullptr, path.data(), size);
        if (written == 0)
        {
            return std::nullopt;
        }
        if (written < size)
        {
            path.resize(written);
            return path;
        }
    }
    return std::nullopt;
}

/// Whether @p executable is under Program Files (either of them), through the injected SHGetKnownFolderPath.
[[nodiscard]] bool underProgramFiles(Windows::StartAtLoginFunctions::KnownFolderPathFn knownFolderPath, std::string_view executable)
{
    const std::array<const KNOWNFOLDERID*, 2> folders{&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86};
    for (const KNOWNFOLDERID* folder : folders)
    {
        PWSTR path = nullptr;
        const HRESULT result = knownFolderPath(*folder, KF_FLAG_DONT_VERIFY, nullptr, &path);
        // Freed whether or not the call succeeded, as SHGetKnownFolderPath's contract asks.
        const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> owned(path, &::CoTaskMemFree);
        if (SUCCEEDED(result) && owned != nullptr &&
            Domain::StartAtLogin::isInside(executable, WinString::wideToUtf8(owned.get()), /*ignoreCase=*/true))
        {
            return true;
        }
    }
    return false;
}

/// The text of a REG_SZ or REG_EXPAND_SZ value's @p bytes (UTF-16), without its terminating nulls.
[[nodiscard]] std::wstring valueText(const std::vector<BYTE>& bytes)
{
    std::wstring text(bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), bytes.data(), text.size() * sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0')
    {
        text.pop_back();
    }
    return text;
}

} // namespace

WindowsStartAtLogin::WindowsStartAtLogin(std::wstring key, std::wstring valueName, Windows::StartAtLoginFunctions api)
    : m_Key(std::move(key)), m_ValueName(std::move(valueName)), m_Api(api)
{}

std::expected<StartAtLoginFacts, std::string> WindowsStartAtLogin::read()
{
    StartAtLoginFacts facts;
    facts.caseInsensitivePaths = true;

    const std::optional<std::wstring> image = ownImage(m_Api.moduleFileName);
    if (!image.has_value())
    {
        return std::unexpected(std::format("couldn't read TaskSmack's own path: {}", errorText(static_cast<LSTATUS>(GetLastError()))));
    }
    facts.executable = WinString::wideToUtf8(*image);
    facts.installed = underProgramFiles(m_Api.knownFolderPath, facts.executable);

    HKEY rawKey = nullptr;
    const LSTATUS opened = m_Api.openKey(HKEY_CURRENT_USER, m_Key.c_str(), 0, KEY_QUERY_VALUE, &rawKey);
    if (opened == ERROR_FILE_NOT_FOUND)
    {
        return facts; // no Run key at all: no entry
    }
    if (opened != ERROR_SUCCESS)
    {
        return std::unexpected(std::format("couldn't open the Run key: {}", errorText(opened)));
    }
    const InjectableKey key(rawKey, InjectedKeyCloser{.close = m_Api.closeKey});

    // The size first, then the bytes; a value that grows in between is read again.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        DWORD type = 0;
        DWORD size = 0;
        LSTATUS queried = m_Api.queryValue(key.get(), m_ValueName.c_str(), nullptr, &type, nullptr, &size);
        if (queried == ERROR_FILE_NOT_FOUND)
        {
            return facts; // no entry
        }
        if (queried != ERROR_SUCCESS)
        {
            return std::unexpected(std::format("couldn't read the Run value: {}", errorText(queried)));
        }
        std::vector<BYTE> bytes(size);
        queried = m_Api.queryValue(key.get(), m_ValueName.c_str(), nullptr, &type, bytes.data(), &size);
        if (queried == ERROR_MORE_DATA)
        {
            continue;
        }
        if (queried == ERROR_FILE_NOT_FOUND)
        {
            return facts; // deleted in between
        }
        if (queried != ERROR_SUCCESS)
        {
            return std::unexpected(std::format("couldn't read the Run value: {}", errorText(queried)));
        }
        bytes.resize(std::min<std::size_t>(bytes.size(), size));
        // A value that isn't text starts nothing TaskSmack can name: an empty command, which reads as another program.
        facts.command = (type == REG_SZ || type == REG_EXPAND_SZ) ? WinString::wideToUtf8(valueText(bytes)) : std::string{};
        return facts;
    }
    return std::unexpected(std::string("couldn't read the Run value: it kept changing"));
}

std::expected<void, std::string> WindowsStartAtLogin::write(std::string_view command)
{
    HKEY rawKey = nullptr;
    // Created when missing (a new profile may have no Run key yet); per-user, so no elevation.
    const LSTATUS created = m_Api.createKey(HKEY_CURRENT_USER, m_Key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &rawKey, nullptr);
    if (created != ERROR_SUCCESS)
    {
        return std::unexpected(std::format("couldn't open the Run key: {}", errorText(created)));
    }
    const InjectableKey key(rawKey, InjectedKeyCloser{.close = m_Api.closeKey});
    const std::wstring text = WinString::utf8ToWide(command);
    // REG_SZ, with its terminating null counted, as RegSetValueExW asks.
    const auto bytes = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    const LSTATUS set = m_Api.setValue(key.get(),
                                       m_ValueName.c_str(),
                                       0,
                                       REG_SZ,
                                       reinterpret_cast<const BYTE*>(text.c_str()),
                                       bytes); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - the registry takes bytes
    if (set != ERROR_SUCCESS)
    {
        return std::unexpected(std::format("couldn't write the Run value: {}", errorText(set)));
    }
    return {};
}

std::expected<void, std::string> WindowsStartAtLogin::remove()
{
    HKEY rawKey = nullptr;
    const LSTATUS opened = m_Api.openKey(HKEY_CURRENT_USER, m_Key.c_str(), 0, KEY_SET_VALUE, &rawKey);
    if (opened == ERROR_FILE_NOT_FOUND)
    {
        return {}; // no Run key: nothing to remove
    }
    if (opened != ERROR_SUCCESS)
    {
        return std::unexpected(std::format("couldn't open the Run key: {}", errorText(opened)));
    }
    const InjectableKey key(rawKey, InjectedKeyCloser{.close = m_Api.closeKey});
    const LSTATUS deleted = m_Api.deleteValue(key.get(), m_ValueName.c_str());
    if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND)
    {
        return std::unexpected(std::format("couldn't delete the Run value: {}", errorText(deleted)));
    }
    return {};
}

} // namespace Platform
