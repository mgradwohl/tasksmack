#include "HandleRelativeFile.h"

#include "Platform/Windows/OrdinalCase.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsFinalPath.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcAddress.h"
#include "Platform/Windows/WindowsSecurityOwners.h"

// clang-format off
#include <sddl.h>   // ConvertStringSecurityDescriptorToSecurityDescriptorW
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// FileDispositionInformation and FileDispositionInformationEx, and the Ex flags (ntifs.h), which winternl.h doesn't name.
constexpr ULONG FILE_DISPOSITION_INFORMATION_CLASS = 13;
constexpr ULONG FILE_DISPOSITION_INFORMATION_EX_CLASS = 64;
constexpr DWORD FILE_DISPOSITION_DELETE = 0x00000001;
constexpr DWORD FILE_DISPOSITION_POSIX_SEMANTICS = 0x00000002;
constexpr DWORD FILE_DISPOSITION_IGNORE_READONLY = 0x00000010;

} // namespace

NTSTATUS NTAPI systemNtCreateFile(PHANDLE handle,
                                  ACCESS_MASK access,
                                  POBJECT_ATTRIBUTES attributes,
                                  PIO_STATUS_BLOCK status,
                                  PLARGE_INTEGER allocationSize,
                                  ULONG fileAttributes,
                                  ULONG share,
                                  ULONG disposition,
                                  ULONG options,
                                  PVOID eaBuffer,
                                  ULONG eaLength)
{
    static auto* const create = getProcAddress<NtCreateFileFn>(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
    if (create == nullptr)
    {
        return NT_NO_ENTRY_POINT; // LCOV_EXCL_LINE - ntdll always exports it
    }
    return create(handle, access, attributes, status, allocationSize, fileAttributes, share, disposition, options, eaBuffer, eaLength);
}

NTSTATUS NTAPI systemNtSetInformationFile(HANDLE file, PIO_STATUS_BLOCK status, PVOID information, ULONG length, ULONG what)
{
    static auto* const set = getProcAddress<NtSetInformationFileFn>(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile");
    if (set == nullptr)
    {
        return NT_NO_ENTRY_POINT; // LCOV_EXCL_LINE - ntdll always exports it
    }
    return set(file, status, information, length, what);
}

std::string ntStatusText(NTSTATUS status)
{
    return std::format("NTSTATUS 0x{:08X}", static_cast<std::uint32_t>(status));
}

std::string lastErrorText()
{
    return Platform::WinString::win32ErrorText(GetLastError());
}

std::expected<RelativeOpened, NTSTATUS>
openRelative(HANDLE parent, std::wstring_view name, const RelativeOpen& how, NtCreateFileFn ntCreateFile)
{
    UNICODE_STRING text{};
    text.Buffer = const_cast<PWSTR>(name.data()); // NOLINT(cppcoreguidelines-pro-type-const-cast) - NtCreateFile only reads it
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent; // relative to the folder held: nothing above it is looked up again
    attributes.ObjectName = &text;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    attributes.SecurityDescriptor = how.security;
    ULONG options = FILE_OPEN_REPARSE_POINT;
    if ((how.access & SYNCHRONIZE) != 0)
    {
        options |= FILE_SYNCHRONOUS_IO_NONALERT;
    }
    if (how.kind == EntryKind::Directory)
    {
        options |= FILE_DIRECTORY_FILE;
    }
    else if (how.kind == EntryKind::File)
    {
        options |= FILE_NON_DIRECTORY_FILE;
    }
    IO_STATUS_BLOCK status{};
    HANDLE opened = nullptr;
    const NTSTATUS result = ntCreateFile(
        &opened, how.access, &attributes, &status, nullptr, FILE_ATTRIBUTE_NORMAL, how.share, how.disposition, options, nullptr, 0);
    if (result < 0)
    {
        return std::unexpected(result);
    }
    return RelativeOpened{.handle = UniqueHandle(opened), .created = status.Information == FILE_CREATED};
}

NTSTATUS renameRelative(HANDLE file,
                        HANDLE rootDirectory,
                        std::wstring_view newName,
                        ULONG informationClass,
                        ULONG flags,
                        NtSetInformationFileFn ntSetInformationFile)
{
    // FILE_RENAME_INFORMATION(_EX), whose layout FILE_RENAME_INFO mirrors (Flags in place of ReplaceIfExists), handed
    // to NtSetInformationFile itself: kernel32's SetFileInformationByHandle refuses a relative name with a RootDirectory
    // (ERROR_INVALID_PARAMETER). Zero-initialised, so the name is terminated.
    const std::size_t nameBytes = newName.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
    // The flags are the struct's first ULONG (ReplaceIfExists, padded, in the older SDK layout this builds with).
    std::memcpy(buffer.data(), &flags, sizeof(flags));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(
        buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - a variable-length Win32 struct
    rename->RootDirectory = rootDirectory;
    rename->FileNameLength = static_cast<DWORD>(nameBytes);
    std::ranges::copy(newName, static_cast<wchar_t*>(rename->FileName));
    IO_STATUS_BLOCK status{};
    return ntSetInformationFile(file, &status, rename, static_cast<ULONG>(buffer.size()), informationClass);
}

std::expected<void, NTSTATUS> renameAside(HANDLE file, HANDLE parent, std::wstring_view name, NtSetInformationFileFn ntSetInformationFile)
{
    NTSTATUS result = NT_OBJECT_NAME_COLLISION;
    std::random_device random;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const std::wstring moved = std::format(L"{}.untrusted-{:08X}", name, random());
        result = renameRelative(file, parent, moved, FILE_RENAME_INFORMATION_CLASS, 0, ntSetInformationFile);
        if (result >= 0)
        {
            return {};
        }
        if (result != NT_OBJECT_NAME_COLLISION)
        {
            break; // held open, an oplock, access: another name won't help
        }
    }
    return std::unexpected(result);
}

bool deleteThroughHandle(HANDLE file, NtSetInformationFileFn ntSetInformationFile)
{
    FILE_DISPOSITION_INFO_EX exInfo{};
    exInfo.Flags = FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS | FILE_DISPOSITION_IGNORE_READONLY;
    IO_STATUS_BLOCK status{};
    if (ntSetInformationFile(file, &status, &exInfo, sizeof(exInfo), FILE_DISPOSITION_INFORMATION_EX_CLASS) >= 0)
    {
        return true;
    }
    FILE_DISPOSITION_INFO info{};
    info.DeleteFile = TRUE;
    return ntSetInformationFile(file, &status, &info, sizeof(info), FILE_DISPOSITION_INFORMATION_CLASS) >= 0;
}

void markDeleteOnClose(HANDLE file) noexcept
{
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    static_cast<void>(SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)));
}

std::optional<std::wstring> finalPathOf(HANDLE handle, GetFinalPathFn getFinalPath)
{
    // Sized as the first answer says, and only a drive path's \\?\ dropped: a \\?\UNC\ path keeps it.
    return readFinalPath(
        handle, getFinalPath, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS, FinalPathBuffer::AsAnswered, FinalPathPrefix::KeptForUnc);
}

std::optional<bool> isPlainFolder(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info) == FALSE)
    {
        return std::nullopt;
    }
    return (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::optional<bool> ownerIsSystemOrAdministratorsOrTrustedInstaller(HANDLE handle)
{
    const auto owner = ownerSidOf(handle);
    if (!owner.has_value())
    {
        return std::nullopt;
    }
    return isAdministratorsOrSystem(owner->sid) || isTrustedInstaller(owner->sid);
}

OwnedSecurityDescriptor securityDescriptorFrom(std::string_view sddl)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring wide(sddl.begin(), sddl.end()); // SDDL is ASCII
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(wide.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == FALSE)
    {
        return {nullptr, &::LocalFree};
    }
    return {descriptor, &::LocalFree};
}

bool samePathText(std::wstring_view left, std::wstring_view right) noexcept
{
    return samePathIgnoringTrailingSeparators(left, right);
}

std::expected<UniqueHandle, std::string> openFolderByPath(const std::filesystem::path& folder, ACCESS_MASK extraAccess)
{
    UniqueHandle handle(CreateFileW(folder.c_str(),
                                    FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | extraAccess,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr));
    if (!handle)
    {
        return std::unexpected(lastErrorText());
    }
    return handle;
}

std::expected<UniqueHandle, std::string> secureChildFolder(
    HANDLE parent, const std::wstring& name, PSECURITY_DESCRIPTOR security, const std::string& display, const OwnerTrustedFn& ownerTrusted)
{
    constexpr ACCESS_MASK HELD = READ_CONTROL | WRITE_DAC | FILE_LIST_DIRECTORY | SYNCHRONIZE | FILE_READ_ATTRIBUTES;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        auto fresh = openRelative(
            parent, name, RelativeOpen{.access = HELD, .disposition = FILE_CREATE, .kind = EntryKind::Directory, .security = security});
        if (fresh.has_value() && fresh->created)
        {
            return std::move(fresh->handle); // made fresh, with the descriptor's owner and DACL
        }
        if (!fresh.has_value() && fresh.error() != NT_OBJECT_NAME_COLLISION)
        {
            return std::unexpected(std::format("{} can't be made: {}", display, ntStatusText(fresh.error())));
        }
        // There already. A plain folder SYSTEM, TrustedInstaller or Administrators owns, that this process can open for
        // WRITE_DAC, is kept and its DACL reset; the open for WRITE_DAC is tried separately so a protected DACL doesn't stop
        // the removal below.
        if (auto dacl = openRelative(parent, name, RelativeOpen{.access = HELD, .disposition = FILE_OPEN, .kind = EntryKind::Directory});
            dacl.has_value() && isPlainFolder(dacl->handle.get()) == std::optional(true) &&
            (ownerTrusted ? ownerTrusted(dacl->handle.get()) : ownerIsSystemOrAdministratorsOrTrustedInstaller(dacl->handle.get())) ==
                std::optional(true))
        {
            if (SetKernelObjectSecurity(dacl->handle.get(), DACL_SECURITY_INFORMATION, security) == FALSE)
            {
                return std::unexpected(std::format("{}'s security can't be set: {}", display, lastErrorText()));
            }
            return std::move(dacl->handle);
        }
        // Otherwise move it aside and make a fresh one on the next turn. Opened for DELETE only, whatever it is (a folder,
        // a junction, a planted file), which the parent's FILE_DELETE_CHILD grants without the entry's own DACL.
        auto removable = openRelative(parent, name, RelativeOpen{.access = DELETE, .disposition = FILE_OPEN, .kind = EntryKind::Either});
        if (!removable.has_value())
        {
            return std::unexpected(
                std::format("the existing {} can't be opened to move aside: {}", display, ntStatusText(removable.error())));
        }
        if (const auto moved = renameAside(removable->handle.get(), parent, name); !moved.has_value())
        {
            return std::unexpected(std::format("the existing {} can't be moved aside: {}", display, ntStatusText(moved.error())));
        }
    }
    return std::unexpected(std::format("{} can't be secured: it keeps being replaced", display));
}

} // namespace Platform::Windows
