#include "WindowsDumpFolder.h"

#include "HandleRelativeFile.h"
#include "Platform/IProcessActions.h"
#include "WinString.h"
#include "WindowsHandles.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <aclapi.h>  // GetSecurityInfo
// clang-format on

#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

/// @p path as GetFinalPathNameByHandleW names it, so the two compare: a \\?\UNC\server\share path (finalPathOf() keeps
/// its prefix) is \\server\share.
[[nodiscard]] std::wstring withoutUncPrefix(std::wstring path)
{
    constexpr std::wstring_view UNC_PREFIX = LR"(\\?\UNC\)";
    if (path.size() >= UNC_PREFIX.size() && samePathText(std::wstring_view(path).substr(0, UNC_PREFIX.size()), UNC_PREFIX))
    {
        path.replace(0, UNC_PREFIX.size(), LR"(\\)");
    }
    return path;
}

/// Whether @p sid is one of the less-privileged accounts newFileReaders() looks for (WindowsDumpFolder.h).
[[nodiscard]] bool lessPrivileged(PSID sid, PSID user)
{
    return IsWellKnownSid(sid, WinWorldSid) != FALSE || IsWellKnownSid(sid, WinBuiltinUsersSid) != FALSE ||
           IsWellKnownSid(sid, WinAuthenticatedUserSid) != FALSE || IsWellKnownSid(sid, WinInteractiveSid) != FALSE ||
           (user != nullptr && EqualSid(sid, user) != FALSE);
}

} // namespace

std::expected<UniqueHandle, std::string> openHeldDumpFolder(const std::filesystem::path& folder)
{
    if (folder.empty() || !folder.is_absolute())
    {
        return std::unexpected(std::string("No folder was given to write the dump file in"));
    }
    std::filesystem::path wanted = folder.lexically_normal();
    if (!wanted.has_filename() && wanted.has_relative_path())
    {
        wanted = wanted.parent_path(); // "C:\Dumps\" is "C:\Dumps"; a drive's root keeps its separator
    }
    const std::string shown = WinString::wideToUtf8(wanted.native());
    auto held = openFolderByPath(wanted, READ_CONTROL);
    if (!held.has_value())
    {
        return std::unexpected(std::format("The folder {} can't be opened: {}", shown, held.error()));
    }
    if (isPlainFolder(held->get()) != std::optional(true))
    {
        return std::unexpected(std::format("{} is a junction, link or other reparse point, not a plain folder: as administrator, "
                                           "TaskSmack writes dumps only to a folder itself. Choose another with Save dump to...",
                                           shown));
    }
    const std::optional<std::wstring> final = finalPathOf(held->get());
    if (!final.has_value())
    {
        return std::unexpected(std::format("Windows can't name the folder {} opens as, so where it is can't be checked", shown));
    }
    if (!samePathText(withoutUncPrefix(*final), wanted.native()))
    {
        return std::unexpected(std::format("{} isn't where its path says: Windows opens it as {}, so a folder on its path is a "
                                           "junction or link. Choose the folder by its own path with Save dump to...",
                                           shown,
                                           WinString::wideToUtf8(*final)));
    }
    return std::move(*held);
}

DumpFolderAccess newFileReaders(PSECURITY_DESCRIPTOR descriptor, PSID user)
{
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (descriptor == nullptr || GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) == FALSE)
    {
        return DumpFolderAccess::Unknown;
    }
    if (present == FALSE || dacl == nullptr)
    {
        return DumpFolderAccess::ReadableByUser; // no DACL: everyone has every access
    }
    PSID owner = nullptr;
    BOOL ownerDefaulted = FALSE;
    if (user != nullptr && GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted) != FALSE && owner != nullptr &&
        EqualSid(owner, user) != FALSE)
    {
        return DumpFolderAccess::ReadableByUser; // its owner can rewrite the DACL new files inherit
    }
    for (DWORD index = 0; index < dacl->AceCount; ++index)
    {
        void* ace = nullptr;
        if (GetAce(dacl, index, &ace) == FALSE || ace == nullptr)
        {
            return DumpFolderAccess::Unknown;
        }
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & OBJECT_INHERIT_ACE) == 0)
        {
            continue; // a deny, an object or callback ACE, or one a new file doesn't inherit
        }
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if ((allowed->Mask & (FILE_READ_DATA | GENERIC_READ | GENERIC_ALL)) == 0)
        {
            continue;
        }
        if (lessPrivileged(&allowed->SidStart, user))
        {
            return DumpFolderAccess::ReadableByUser;
        }
    }
    return DumpFolderAccess::AdministratorsOnly;
}

DumpFolderAccess heldFolderReaders(HANDLE folder, PSID user)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(folder,
                        SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        &descriptor) != ERROR_SUCCESS)
    {
        return DumpFolderAccess::Unknown;
    }
    const std::unique_ptr<void, decltype(&::LocalFree)> owned(descriptor, &::LocalFree);
    return newFileReaders(descriptor, user);
}

std::vector<BYTE> currentUserSid()
{
    HANDLE rawToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) == FALSE)
    {
        return {};
    }
    const UniqueHandle token(rawToken);
    DWORD size = 0;
    static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size));
    if (size == 0)
    {
        return {};
    }
    std::vector<BYTE> buffer(size);
    if (GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) == FALSE)
    {
        return {};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - GetTokenInformation fills a TOKEN_USER
    const PSID sid = reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid;
    if (sid == nullptr || IsValidSid(sid) == FALSE)
    {
        return {};
    }
    std::vector<BYTE> copy(GetLengthSid(sid));
    if (CopySid(static_cast<DWORD>(copy.size()), copy.data(), sid) == FALSE)
    {
        return {};
    }
    return copy;
}

DumpFolderCheck checkDumpFolder(const std::filesystem::path& folder)
{
    const auto held = openHeldDumpFolder(folder);
    if (!held.has_value())
    {
        return {.access = DumpFolderAccess::Refused, .problem = held.error()};
    }
    std::vector<BYTE> user = currentUserSid();
    return {.access = heldFolderReaders(held->get(), user.empty() ? nullptr : static_cast<PSID>(user.data())), .problem = {}};
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
