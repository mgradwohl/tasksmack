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
#include <sddl.h>    // ConvertStringSidToSidW
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

/// TrustedInstaller's service SID (NT SERVICE\TrustedInstaller), which owns and may change Windows' own folders.
constexpr std::wstring_view TRUSTED_INSTALLER_SID = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

/// Whether @p sid is TrustedInstaller.
[[nodiscard]] bool isTrustedInstaller(PSID sid)
{
    static const std::unique_ptr<void, decltype(&::LocalFree)> trustedInstaller = []
    {
        PSID made = nullptr;
        const std::wstring text(TRUSTED_INSTALLER_SID);
        if (ConvertStringSidToSidW(text.c_str(), &made) == FALSE)
        {
            made = nullptr; // LCOV_EXCL_LINE - a constant, well-formed SID
        }
        return std::unique_ptr<void, decltype(&::LocalFree)>(made, &::LocalFree);
    }();
    return trustedInstaller != nullptr && EqualSid(sid, trustedInstaller.get()) != FALSE;
}

/// Whether @p sid is one newFileReaders() trusts (WindowsDumpFolder.h): Administrators, SYSTEM or TrustedInstaller, and
/// CREATOR OWNER where @p creatorOwnerTrusted (an inherited ACE, once a new file's owner is known to be trusted).
[[nodiscard]] bool trustedSid(PSID sid, bool creatorOwnerTrusted)
{
    return IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != FALSE || IsWellKnownSid(sid, WinLocalSystemSid) != FALSE ||
           isTrustedInstaller(sid) || (creatorOwnerTrusted && IsWellKnownSid(sid, WinCreatorOwnerSid) != FALSE);
}

/// An owner ownerIsAdministratorsOrSystem() trusts: BUILTIN\Administrators or SYSTEM.
[[nodiscard]] bool trustedOwner(PSID owner)
{
    return IsWellKnownSid(owner, WinBuiltinAdministratorsSid) != FALSE || IsWellKnownSid(owner, WinLocalSystemSid) != FALSE;
}

/// The access bits that let a new file be read (executing it maps it, which reads it), and those that let a DACL or owner
/// be changed.
constexpr ACCESS_MASK READ_RIGHTS = FILE_READ_DATA | FILE_EXECUTE | GENERIC_READ | GENERIC_EXECUTE | GENERIC_ALL;
constexpr ACCESS_MASK CONTROL_RIGHTS = WRITE_DAC | WRITE_OWNER | GENERIC_ALL;

} // namespace

bool acceptedDumpFolder(DWORD attributes, DWORD reparseTag) noexcept
{
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
    {
        return true;
    }
    constexpr DWORD CLOUD_TAG_MASK = 0xFFFF0FFFU; // IO_REPARSE_TAG_CLOUD_1..F differ only in bits 12-15
    return (reparseTag & CLOUD_TAG_MASK) == static_cast<DWORD>(IO_REPARSE_TAG_CLOUD);
}

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
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandleEx(held->get(), FileAttributeTagInfo, &tag, sizeof(tag)) == FALSE)
    {
        return std::unexpected(std::format("The folder {} can't be checked: {}", shown, lastErrorText()));
    }
    if (!acceptedDumpFolder(tag.FileAttributes, tag.ReparseTag))
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
    const std::wstring finalPath = withoutUncPrefix(*final);
    if (!samePathText(finalPath, wanted.native()))
    {
        // Another drive or a share: a mapped, substituted or DFS drive, whose real path Windows names instead.
        if (!samePathText(std::filesystem::path(finalPath).root_name().native(), wanted.root_name().native()))
        {
            return std::unexpected(std::format("{} is on a network or substituted drive: Windows opens it as {}. Choose it by that "
                                               "path (\\\\server\\share\\... for a network folder) with Save dump to...",
                                               shown,
                                               WinString::wideToUtf8(finalPath)));
        }
        return std::unexpected(std::format("{} isn't where its path says: Windows opens it as {}, so a folder on its path is a "
                                           "junction or link. Choose the folder by its own path with Save dump to...",
                                           shown,
                                           WinString::wideToUtf8(finalPath)));
    }
    return std::move(*held);
}

DumpFolderAccess newFileReaders(PSECURITY_DESCRIPTOR descriptor, bool defaultOwnerTrusted)
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
    if (!defaultOwnerTrusted)
    {
        // A new file's owner is the token's default owner, who always gets READ_CONTROL and WRITE_DAC on it: whatever the
        // folder's ACEs, that owner can grant itself a read.
        return DumpFolderAccess::ReadableByUser;
    }
    PSID owner = nullptr;
    BOOL ownerDefaulted = FALSE;
    if (GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted) == FALSE || owner == nullptr)
    {
        return DumpFolderAccess::Unknown;
    }
    if (!trustedOwner(owner))
    {
        return DumpFolderAccess::ReadableByUser; // its owner can rewrite the DACL new files inherit
    }
    bool inherited = false; // whether any allow ACE reaches a new file
    for (DWORD index = 0; index < dacl->AceCount; ++index)
    {
        void* ace = nullptr;
        if (GetAce(dacl, index, &ace) == FALSE || ace == nullptr)
        {
            return DumpFolderAccess::Unknown;
        }
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        const bool objectInherit = (header->AceFlags & OBJECT_INHERIT_ACE) != 0;
        const bool onFolder = (header->AceFlags & INHERIT_ONLY_ACE) == 0;
        switch (header->AceType)
        {
        case ACCESS_ALLOWED_ACE_TYPE:
        case ACCESS_ALLOWED_CALLBACK_ACE_TYPE: // the same layout, with a condition after the SID
        {
            auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            PSID sid = &allowed->SidStart;
            inherited = inherited || objectInherit;
            // Inherited by a new file: a read, or a WRITE_DAC or WRITE_OWNER that would let its holder grant itself one.
            if (objectInherit && (allowed->Mask & (READ_RIGHTS | CONTROL_RIGHTS)) != 0 && !trustedSid(sid, true))
            {
                return DumpFolderAccess::ReadableByUser;
            }
            if (onFolder && (allowed->Mask & CONTROL_RIGHTS) != 0 && !trustedSid(sid, false))
            {
                return DumpFolderAccess::ReadableByUser; // can change who may read what is made there
            }
            break;
        }
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
        case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
        {
            // Rare on files; its SID's place depends on its flags, so any read or control it grants is flagged.
            const auto* allowed = static_cast<const ACCESS_ALLOWED_OBJECT_ACE*>(ace);
            inherited = inherited || objectInherit;
            if ((objectInherit && (allowed->Mask & (READ_RIGHTS | CONTROL_RIGHTS)) != 0) ||
                (onFolder && (allowed->Mask & CONTROL_RIGHTS) != 0))
            {
                return DumpFolderAccess::ReadableByUser;
            }
            break;
        }
        case ACCESS_DENIED_ACE_TYPE:
        case ACCESS_DENIED_OBJECT_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_ACE_TYPE:
        case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
        case SYSTEM_AUDIT_ACE_TYPE:
        case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
        case SYSTEM_AUDIT_CALLBACK_ACE_TYPE:
        case SYSTEM_AUDIT_CALLBACK_OBJECT_ACE_TYPE:
        case SYSTEM_ALARM_ACE_TYPE:
        case SYSTEM_ALARM_OBJECT_ACE_TYPE:
        case SYSTEM_ALARM_CALLBACK_ACE_TYPE:
        case SYSTEM_ALARM_CALLBACK_OBJECT_ACE_TYPE:
            break; // a deny grants nothing; an audit or alarm ACE neither
        default:
            return DumpFolderAccess::Unknown; // an allow type this doesn't read (a compound ACE, say): fail closed
        }
    }
    // No allow ACE reaches a new file: it gets the token's default DACL, which isn't read here.
    return inherited ? DumpFolderAccess::AdministratorsOnly : DumpFolderAccess::Unknown;
}

DumpFolderAccess heldFolderReaders(HANDLE folder, bool defaultOwnerTrusted)
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
    return newFileReaders(descriptor, defaultOwnerTrusted);
}

std::optional<bool> defaultOwnerIsAdministratorsOrSystem()
{
    HANDLE rawToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) == FALSE)
    {
        return std::nullopt;
    }
    const UniqueHandle token(rawToken);
    DWORD size = 0;
    static_cast<void>(GetTokenInformation(token.get(), TokenOwner, nullptr, 0, &size));
    if (size == 0)
    {
        return std::nullopt;
    }
    std::vector<BYTE> buffer(size);
    if (GetTokenInformation(token.get(), TokenOwner, buffer.data(), size, &size) == FALSE)
    {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - GetTokenInformation fills a TOKEN_OWNER
    const PSID owner = reinterpret_cast<const TOKEN_OWNER*>(buffer.data())->Owner;
    if (owner == nullptr || IsValidSid(owner) == FALSE)
    {
        return std::nullopt;
    }
    return trustedOwner(owner);
}

DumpFolderCheck checkDumpFolder(const std::filesystem::path& folder)
{
    const auto held = openHeldDumpFolder(folder);
    if (!held.has_value())
    {
        return {.access = DumpFolderAccess::Refused, .problem = held.error()};
    }
    const std::optional<bool> defaultOwnerTrusted = defaultOwnerIsAdministratorsOrSystem();
    if (!defaultOwnerTrusted.has_value())
    {
        return {.access = DumpFolderAccess::Unknown, .problem = {}};
    }
    return {.access = heldFolderReaders(held->get(), *defaultOwnerTrusted), .problem = {}};
}

std::expected<std::filesystem::path, std::string> makeAdminDumpFolder(const std::filesystem::path& programData,
                                                                      const AdminDumpFolderOptions& options)
{
    const std::string shownRoot = WinString::wideToUtf8(programData.native());
    if (programData.empty() || !programData.is_absolute())
    {
        return std::unexpected(std::string("the ProgramData folder isn't known"));
    }
    const OwnedSecurityDescriptor descriptor = securityDescriptorFrom(options.folderSecurity);
    if (descriptor == nullptr)
    {
        return std::unexpected(std::string("the dump folder's security can't be built"));
    }
    // %ProgramData% by its path, once: a system folder SYSTEM owns, the trust root, as the elevated log opens it (#4105).
    const auto root = openFolderByPath(programData);
    if (!root.has_value())
    {
        return std::unexpected(std::format("{} can't be opened: {}", shownRoot, root.error()));
    }
    if (isPlainFolder(root->get()) != std::optional(true))
    {
        return std::unexpected(std::format("{} isn't a plain folder", shownRoot));
    }
    const std::filesystem::path taskSmackPath = programData / L"TaskSmack";
    auto taskSmack =
        secureChildFolder(root->get(), L"TaskSmack", descriptor.get(), WinString::wideToUtf8(taskSmackPath.native()), options.ownerTrusted);
    if (!taskSmack.has_value())
    {
        return std::unexpected(std::move(taskSmack.error()));
    }
    const std::filesystem::path dumpsPath = taskSmackPath / L"Dumps";
    auto dumps =
        secureChildFolder(taskSmack->get(), L"Dumps", descriptor.get(), WinString::wideToUtf8(dumpsPath.native()), options.ownerTrusted);
    if (!dumps.has_value())
    {
        return std::unexpected(std::move(dumps.error()));
    }
    // As Windows names it, so the dump's own open of it (openHeldDumpFolder()) finds the same path.
    const std::optional<std::wstring> final = finalPathOf(dumps->get());
    return final.has_value() ? std::filesystem::path(withoutUncPrefix(*final)) : dumpsPath;
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
