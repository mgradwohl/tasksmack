#include "WindowsTaskManagerInstall.h"

#include "Platform/TaskManagerReplacement.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <knownfolders.h>
#include <objbase.h> // CoTaskMemFree
#include <shlobj.h>

namespace Platform::Windows
{

namespace
{

[[nodiscard]] std::string lastErrorText()
{
    return std::system_category().message(static_cast<int>(GetLastError()));
}

/// @p ownToken restricted as UAC restricts an administrator's: Administrators and "local account and
/// member of Administrators" deny-only (allow ACEs for them don't count, deny ACEs still do), and every
/// privilege but SeChangeNotifyPrivilege removed, then duplicated for identification.
[[nodiscard]] std::expected<UniqueHandle, std::string> restrictedTokenOf(HANDLE ownToken)
{
    std::array<std::array<std::byte, SECURITY_MAX_SID_SIZE>, 2> sids{};
    std::array<SID_AND_ATTRIBUTES, 2> disable{};
    const std::array<WELL_KNOWN_SID_TYPE, 2> types{WinBuiltinAdministratorsSid, WinLocalAccountAndAdministratorSid};
    for (std::size_t i = 0; i < types.size(); ++i)
    {
        auto size = static_cast<DWORD>(sids.at(i).size());
        if (CreateWellKnownSid(types.at(i), nullptr, sids.at(i).data(), &size) == FALSE)
        {
            return std::unexpected(std::format("couldn't make an administrators' SID: {}", lastErrorText()));
        }
        disable.at(i) = SID_AND_ATTRIBUTES{.Sid = sids.at(i).data(), .Attributes = 0};
    }
    UniqueHandle restricted;
    if (CreateRestrictedToken(ownToken,
                              DISABLE_MAX_PRIVILEGE,
                              static_cast<DWORD>(disable.size()),
                              disable.data(),
                              0,
                              nullptr,
                              0,
                              nullptr,
                              restricted.put()) == FALSE)
    {
        return std::unexpected(std::format("couldn't restrict TaskSmack's token: {}", lastErrorText()));
    }
    UniqueHandle identification;
    if (DuplicateToken(restricted.get(), SecurityIdentification, identification.put()) == FALSE)
    {
        return std::unexpected(std::format("couldn't copy TaskSmack's restricted token: {}", lastErrorText()));
    }
    return identification;
}

} // namespace

std::vector<std::string> installFolders(KnownFolderPathFn knownFolderPath)
{
    std::vector<std::string> folders;
    if (knownFolderPath == nullptr)
    {
        return folders;
    }
    const std::array<const KNOWNFOLDERID*, 3> ids{&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX64, &FOLDERID_ProgramFilesX86};
    for (const KNOWNFOLDERID* id : ids)
    {
        PWSTR path = nullptr;
        const HRESULT result = knownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &path);
        // Freed whether or not the call succeeded, as SHGetKnownFolderPath's contract asks.
        const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> owned(path, &::CoTaskMemFree);
        if (SUCCEEDED(result) && owned != nullptr && *owned != L'\0')
        {
            folders.push_back(WinString::wideToUtf8(owned.get()));
        }
    }
    return folders;
}

std::expected<UniqueHandle, std::string> unelevatedTokenOf(HANDLE ownToken)
{
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD length = 0;
    if (GetTokenInformation(ownToken, TokenElevationType, &type, sizeof(type), &length) == FALSE)
    {
        return std::unexpected(std::format("couldn't tell whether TaskSmack runs elevated: {}", lastErrorText()));
    }
    if (type != TokenElevationTypeFull)
    {
        return restrictedTokenOf(ownToken);
    }
    // Elevated under UAC: the linked token is this user's own unelevated one, what the programs they
    // run have. A caller without SeTcbPrivilege gets it as an identification token, as AccessCheck takes.
    TOKEN_LINKED_TOKEN linked{};
    if (GetTokenInformation(ownToken, TokenLinkedToken, &linked, sizeof(linked), &length) == FALSE)
    {
        return std::unexpected(std::format("couldn't read TaskSmack's unelevated token: {}", lastErrorText()));
    }
    return UniqueHandle(linked.LinkedToken);
}

std::expected<void, std::string> checkUnchangeableByUnelevatedUser(const std::wstring& exe)
{
    // The path written must be the file checked: through a link or junction, the file is checked where
    // it really is, but the path Windows starts could later lead somewhere else.
    const auto final = BrokerHelper::finalPath(exe);
    if (!final.has_value())
    {
        return std::unexpected(std::format("couldn't open {}", WinString::wideToUtf8(exe)));
    }
    if (CompareStringOrdinal(final->c_str(), static_cast<int>(final->size()), exe.c_str(), static_cast<int>(exe.size()), TRUE) !=
        CSTR_EQUAL)
    {
        return std::unexpected(std::format("{} is really {}", WinString::wideToUtf8(exe), WinString::wideToUtf8(*final)));
    }
    UniqueHandle own;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, own.put()) == FALSE)
    {
        return std::unexpected(std::format("couldn't open TaskSmack's token: {}", lastErrorText()));
    }
    const auto unelevated = unelevatedTokenOf(own.get());
    if (!unelevated.has_value())
    {
        return std::unexpected(unelevated.error());
    }
    const std::array<std::wstring, 1> files{exe};
    return BrokerHelper::checkUnchangeableBy(unelevated->get(), files);
}

std::expected<void, std::string>
checkDirectWrite(std::string_view selfPath, std::span<const std::string> installFolders, DirectWriteCheck check)
{
    const std::vector<std::string_view> folders(installFolders.begin(), installFolders.end());
    if (!TaskManagerReplacement::isInstalledCopy(selfPath, folders))
    {
        return std::unexpected(std::format("{} isn't installed in Program Files", selfPath));
    }
    if (check == nullptr)
    {
        return std::unexpected(std::string("nothing checks who can change this copy"));
    }
    return check(WinString::utf8ToWide(selfPath));
}

} // namespace Platform::Windows
