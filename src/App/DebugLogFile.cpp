#include "App/DebugLogFile.h"

#include "Core/LogFileSink.h"

#include <spdlog/common.h>

#include <exception>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>  // GetSecurityInfo, SetKernelObjectSecurity
#include <winternl.h> // OBJECT_ATTRIBUTES, IO_STATUS_BLOCK, UNICODE_STRING
#include <sddl.h>     // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <shlobj.h>   // SHGetKnownFolderPath
// clang-format on

#include "Core/Utf8Path.h"
#include "Platform/Windows/WindowsProcAddress.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string_view>
#include <system_error>
#include <vector>
#endif

namespace App::DebugLogFile
{
namespace
{

#ifdef _WIN32
// NtCreateFile dispositions and options, and the NTSTATUS values used here (ntifs.h / ntstatus.h, which user-mode
// headers don't declare).
constexpr ULONG NT_FILE_CREATE = 0x00000002; // create new only, fail if it exists
constexpr ULONG NT_FILE_OPEN = 0x00000001;   // open existing only
constexpr ULONG NT_FILE_DIRECTORY = 0x00000001;
constexpr ULONG NT_FILE_NON_DIRECTORY = 0x00000040;
constexpr ULONG NT_FILE_SYNCHRONOUS = 0x00000020;
constexpr ULONG NT_FILE_OPEN_REPARSE_POINT = 0x00200000;
constexpr ULONG_PTR NT_FILE_CREATED = 2; // IO_STATUS_BLOCK.Information after a create
constexpr auto STATUS_OBJECT_NAME_NOT_FOUND = static_cast<NTSTATUS>(0xC0000034L);
constexpr auto STATUS_OBJECT_NAME_COLLISION = static_cast<NTSTATUS>(0xC0000035L);
constexpr auto STATUS_OBJECT_PATH_NOT_FOUND = static_cast<NTSTATUS>(0xC000003AL);
constexpr auto STATUS_NO_ENTRY_POINT = static_cast<NTSTATUS>(0xC000007AL);

// FileRenameInformation and the delete classes (FILE_INFORMATION_CLASS), which winternl.h doesn't name.
const auto FILE_RENAME_INFORMATION_CLASS = static_cast<FILE_INFORMATION_CLASS>(10);
const auto FILE_DISPOSITION_INFORMATION_EX_CLASS = static_cast<FILE_INFORMATION_CLASS>(64);
const auto FILE_DISPOSITION_INFORMATION_CLASS = static_cast<FILE_INFORMATION_CLASS>(13);
constexpr DWORD FILE_DISPOSITION_DELETE = 0x00000001;
constexpr DWORD FILE_DISPOSITION_POSIX_SEMANTICS = 0x00000002;
constexpr DWORD FILE_DISPOSITION_IGNORE_READONLY = 0x00000010;

using NtCreateFileFn =
    NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtSetInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);

[[nodiscard]] NtCreateFileFn ntCreateFile()
{
    static auto* const function = Platform::Windows::getProcAddress<NtCreateFileFn>(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
    return function;
}

[[nodiscard]] NtSetInformationFileFn ntSetInformationFile()
{
    static auto* const function =
        Platform::Windows::getProcAddress<NtSetInformationFileFn>(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile");
    return function;
}

/// Closes a Win32 handle; INVALID_HANDLE_VALUE and null are none.
struct HandleCloser
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
};
using OwnedHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] OwnedHandle own(HANDLE handle) noexcept
{
    return OwnedHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

[[nodiscard]] std::string lastError()
{
    return std::system_category().message(static_cast<int>(GetLastError()));
}

[[nodiscard]] std::string ntText(NTSTATUS status)
{
    return std::format("NTSTATUS 0x{:08X}", static_cast<std::uint32_t>(status));
}

/// A security descriptor built from @p sddl (LocalFree'd when it dies), or null when @p sddl doesn't parse.
using OwnedDescriptor = std::unique_ptr<void, decltype(&::LocalFree)>;
[[nodiscard]] OwnedDescriptor descriptorFrom(std::string_view sddl)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring wide(sddl.begin(), sddl.end()); // SDDL is ASCII
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(wide.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == FALSE)
    {
        return {nullptr, &::LocalFree};
    }
    return {descriptor, &::LocalFree};
}

/// Whether the owner of @p handle (opened with READ_CONTROL) is BUILTIN\Administrators or SYSTEM; nullopt when it
/// can't be read. A folder TaskSmack made while elevated is owned by Administrators; one a user made is not.
[[nodiscard]] std::optional<bool> ownerIsAdminOrSystem(HANDLE handle)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor) !=
            ERROR_SUCCESS ||
        owner == nullptr)
    {
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return std::nullopt;
    }
    const bool trusted = IsWellKnownSid(owner, WinBuiltinAdministratorsSid) != FALSE || IsWellKnownSid(owner, WinLocalSystemSid) != FALSE;
    LocalFree(descriptor);
    return trusted;
}

/// What an open expects @p name to be: a folder, a file, or either (whatever is there, e.g. an entry to be removed).
enum class OpenKind : std::uint8_t
{
    Directory,
    File,
    Either,
};

/// NtCreateFile relative to @p parent (RootDirectory), for the one plain name @p name, with exactly the access asked
/// (nothing added), so a delete-only open stays grantable through the parent's FILE_DELETE_CHILD. @p disposition says
/// open or create; @p descriptor is applied when it is created. Shared read and write, never delete, so it can't be
/// renamed or deleted from under the handle, but a sibling rename into the parent doesn't collide. A synchronous handle
/// only when SYNCHRONIZE was asked. @p outCreated, when set, says whether a create made a new object.
[[nodiscard]] std::expected<OwnedHandle, NTSTATUS> openRelative(HANDLE parent,
                                                                const std::wstring& name,
                                                                ACCESS_MASK access,
                                                                ULONG disposition,
                                                                OpenKind kind,
                                                                PSECURITY_DESCRIPTOR descriptor,
                                                                bool* outCreated = nullptr)
{
    if (ntCreateFile() == nullptr)
    {
        return std::unexpected(STATUS_NO_ENTRY_POINT); // LCOV_EXCL_LINE - ntdll always exports it
    }
    UNICODE_STRING text{};
    text.Buffer = const_cast<PWSTR>(name.c_str()); // NOLINT(cppcoreguidelines-pro-type-const-cast) - NtCreateFile only reads it
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &text;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    attributes.SecurityDescriptor = descriptor;
    IO_STATUS_BLOCK status{};
    HANDLE opened = nullptr;
    ULONG options = NT_FILE_OPEN_REPARSE_POINT;
    if ((access & SYNCHRONIZE) != 0)
    {
        options |= NT_FILE_SYNCHRONOUS;
    }
    if (kind == OpenKind::Directory)
    {
        options |= NT_FILE_DIRECTORY;
    }
    else if (kind == OpenKind::File)
    {
        options |= NT_FILE_NON_DIRECTORY;
    }
    const NTSTATUS result = ntCreateFile()(&opened,
                                           access,
                                           &attributes,
                                           &status,
                                           nullptr,
                                           FILE_ATTRIBUTE_NORMAL,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, // never FILE_SHARE_DELETE
                                           disposition,
                                           options,
                                           nullptr,
                                           0);
    if (result < 0)
    {
        return std::unexpected(result);
    }
    if (outCreated != nullptr)
    {
        *outCreated = status.Information == NT_FILE_CREATED;
    }
    return own(opened);
}

/// Renames the entry @p handle holds (opened with DELETE) aside, out of the way, through that handle (RootDirectory
/// @p parent): a fresh one can then be made under @p name. Any entry can be moved this way, a junction or a planted
/// file included, which only takes it out of the path. The moved-aside name has a random suffix, tried again only when
/// that name is taken, so one left from a crashed run doesn't block it. The NTSTATUS when it can't be moved.
[[nodiscard]] std::expected<void, NTSTATUS> renameAside(HANDLE handle, HANDLE parent, std::wstring_view name)
{
    if (ntSetInformationFile() == nullptr)
    {
        return std::unexpected(STATUS_NO_ENTRY_POINT); // LCOV_EXCL_LINE
    }
    NTSTATUS result = STATUS_OBJECT_NAME_COLLISION;
    std::random_device random;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const std::wstring moved = std::format(L"{}.untrusted-{:08X}", name, random());
        const std::size_t bytes = sizeof(FILE_RENAME_INFO) + (moved.size() * sizeof(wchar_t));
        std::vector<std::byte> buffer(bytes); // zero-initialised: ReplaceIfExists (a union, left untouched) is FALSE
        auto* const rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->RootDirectory = parent;
        rename->FileNameLength = static_cast<DWORD>(moved.size() * sizeof(wchar_t));
        std::ranges::copy(moved, static_cast<wchar_t*>(rename->FileName));
        IO_STATUS_BLOCK status{};
        result = ntSetInformationFile()(handle, &status, rename, static_cast<ULONG>(bytes), FILE_RENAME_INFORMATION_CLASS);
        if (result >= 0)
        {
            return {};
        }
        if (result != STATUS_OBJECT_NAME_COLLISION)
        {
            break; // held open, an oplock, access: another name won't help
        }
    }
    return std::unexpected(result);
}

/// Marks the file @p handle holds for deletion (opened with DELETE), through that handle: POSIX semantics where the
/// volume takes them, so the name is free at once, else the classic flag. False when neither takes.
[[nodiscard]] bool deleteThroughHandle(HANDLE handle)
{
    if (ntSetInformationFile() == nullptr)
    {
        return false; // LCOV_EXCL_LINE
    }
    FILE_DISPOSITION_INFO_EX exInfo{};
    exInfo.Flags = FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS | FILE_DISPOSITION_IGNORE_READONLY;
    IO_STATUS_BLOCK status{};
    if (ntSetInformationFile()(handle, &status, &exInfo, sizeof(exInfo), FILE_DISPOSITION_INFORMATION_EX_CLASS) >= 0)
    {
        return true;
    }
    FILE_DISPOSITION_INFO info{};
    info.DeleteFile = TRUE;
    return ntSetInformationFile()(handle, &status, &info, sizeof(info), FILE_DISPOSITION_INFORMATION_CLASS) >= 0;
}

/// Whether @p handle is a plain folder (not a reparse point), read through it. nullopt when it can't be read.
[[nodiscard]] std::optional<bool> isPlainFolder(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info) == FALSE)
    {
        return std::nullopt;
    }
    return (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// Folder @p name, under the held @p parent, secured to @p descriptor's DACL and owner, and held open (without
/// FILE_SHARE_DELETE). It is made fresh when it isn't there. An entry there already is taken over in place only when it
/// is a plain folder that Administrators or SYSTEM owns and this process can open for WRITE_DAC (its DACL reset, not its
/// children's: SetKernelObjectSecurity doesn't propagate). Anything else -- a user-owned folder, a protected DACL this
/// process can't open for WRITE_DAC, a junction or a planted file -- is moved aside (renameAside()) through a handle
/// opened for DELETE alone, which the parent's FILE_DELETE_CHILD grants even when the entry's own DACL grants this
/// process nothing, and a fresh one is made in its place. @p display names it for the error.
[[nodiscard]] std::expected<OwnedHandle, std::string> secureChild(HANDLE parent,
                                                                  const std::wstring& name,
                                                                  PSECURITY_DESCRIPTOR descriptor,
                                                                  const std::string& display,
                                                                  const OwnerTrustedFn& ownerTrusted)
{
    constexpr ACCESS_MASK HELD = READ_CONTROL | WRITE_DAC | FILE_LIST_DIRECTORY | SYNCHRONIZE | FILE_READ_ATTRIBUTES;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        bool created = false;
        auto fresh = openRelative(parent, name, HELD, NT_FILE_CREATE, OpenKind::Directory, descriptor, &created);
        if (fresh.has_value() && created)
        {
            return std::move(*fresh); // made fresh, with the descriptor's owner and DACL
        }
        if (!fresh.has_value() && fresh.error() != STATUS_OBJECT_NAME_COLLISION)
        {
            return std::unexpected(std::format("{} can't be made: {}", display, ntText(fresh.error())));
        }
        // There already. A plain folder Administrators or SYSTEM owns, that this process can open for WRITE_DAC, is kept
        // and its DACL reset; the open for WRITE_DAC is tried separately so a protected DACL doesn't stop the removal below.
        if (auto dacl = openRelative(parent, name, HELD, NT_FILE_OPEN, OpenKind::Directory, nullptr);
            dacl.has_value() && isPlainFolder(dacl->get()) == std::optional(true) &&
            (ownerTrusted ? ownerTrusted(dacl->get()) : ownerIsAdminOrSystem(dacl->get())) == std::optional(true))
        {
            if (SetKernelObjectSecurity(dacl->get(), DACL_SECURITY_INFORMATION, descriptor) == FALSE)
            {
                return std::unexpected(std::format("{}'s security can't be set: {}", display, lastError()));
            }
            return std::move(*dacl);
        }
        // Otherwise move it aside and make a fresh one on the next turn. Opened for DELETE only, whatever it is (a folder,
        // a junction, a planted file), which the parent's FILE_DELETE_CHILD grants without the entry's own DACL.
        auto removable = openRelative(parent, name, DELETE, NT_FILE_OPEN, OpenKind::Either, nullptr);
        if (!removable.has_value())
        {
            return std::unexpected(std::format("the existing {} can't be opened to move aside: {}", display, ntText(removable.error())));
        }
        if (const auto moved = renameAside(removable->get(), parent, name); !moved.has_value())
        {
            return std::unexpected(std::format("the existing {} can't be moved aside: {}", display, ntText(moved.error())));
        }
    }
    return std::unexpected(std::format("{} can't be secured: it keeps being replaced", display));
}

/// The log written through the file handle checked when it was opened (#4105), never reopened by its path.
class HandleFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    explicit HandleFileSink(OwnedHandle file) : m_File(std::move(file))
    {}

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        DWORD written = 0;
        static_cast<void>(WriteFile(m_File.get(), formatted.data(), static_cast<DWORD>(formatted.size()), &written, nullptr));
    }

    void flush_() override
    {}

  private:
    OwnedHandle m_File;
};

/// An elevated session's log (#4105), in %ProgramData%\TaskSmack\Logs, as this file's header says.
[[nodiscard]] std::expected<Opened, std::string> openElevated(const Functions& functions)
{
    const std::optional<std::filesystem::path> programData = functions.programDataFolder();
    if (!programData.has_value())
    {
        return std::unexpected(std::string("the ProgramData folder isn't known"));
    }
    const OwnedDescriptor descriptor = descriptorFrom(functions.folderSecurity);
    if (descriptor == nullptr)
    {
        return std::unexpected(std::string("the log folder's security can't be built"));
    }
    // %ProgramData% by path, once: a system folder owned by SYSTEM, the trust root. Held without FILE_SHARE_DELETE.
    const OwnedHandle root = own(CreateFileW(programData->c_str(),
                                             FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                             nullptr));
    if (root == nullptr)
    {
        return std::unexpected(std::format("{} can't be opened: {}", Core::pathToUtf8(*programData), lastError()));
    }
    if (isPlainFolder(root.get()) != std::optional(true))
    {
        return std::unexpected(std::format("{} isn't a plain folder", Core::pathToUtf8(*programData)));
    }
    auto taskSmack =
        secureChild(root.get(), L"TaskSmack", descriptor.get(), Core::pathToUtf8(*programData / "TaskSmack"), functions.ownerTrusted);
    if (!taskSmack.has_value())
    {
        return std::unexpected(std::move(taskSmack.error()));
    }
    const std::filesystem::path logs = elevatedLogFolder(*programData);
    auto logsHandle = secureChild(taskSmack->get(), L"Logs", descriptor.get(), Core::pathToUtf8(logs), functions.ownerTrusted);
    if (!logsHandle.has_value())
    {
        return std::unexpected(std::move(logsHandle.error()));
    }
    // The log, relative to the held Logs folder. Any existing one is deleted through its own handle, then a fresh one is
    // made with CREATE-only: a planted file with a user's DACL is never written to, and a race that re-makes one is refused.
    const std::wstring name(FILE_NAME.begin(), FILE_NAME.end());
    {
        // DELETE alone, whatever it is (a file, a link): granted through the held Logs folder's FILE_DELETE_CHILD even
        // when the planted file's own DACL grants this process nothing. Closed before the fresh create, so the classic
        // delete-on-close (where POSIX semantics aren't taken) has freed the name.
        auto stale = openRelative(logsHandle->get(), name, DELETE, NT_FILE_OPEN, OpenKind::Either, nullptr);
        if (stale.has_value())
        {
            if (!deleteThroughHandle(stale->get()))
            {
                return std::unexpected(std::format("the stale {} can't be removed", std::string(FILE_NAME)));
            }
        }
        else if (stale.error() != STATUS_OBJECT_NAME_NOT_FOUND && stale.error() != STATUS_OBJECT_PATH_NOT_FOUND)
        {
            return std::unexpected(std::format("{} can't be checked: {}", std::string(FILE_NAME), ntText(stale.error())));
        }
    }
    bool created = false;
    auto file =
        openRelative(logsHandle->get(), name, GENERIC_WRITE | SYNCHRONIZE, NT_FILE_CREATE, OpenKind::File, descriptor.get(), &created);
    if (!file.has_value() || !created)
    {
        return std::unexpected(
            std::format("{} can't be made fresh{}", std::string(FILE_NAME), file.has_value() ? "" : ": " + ntText(file.error())));
    }
    // Where it is, as Windows names it: the held folder's final path, which doesn't follow a link, plus the name.
    const std::wstring logsFinal = functions.finalPath(logsHandle->get());
    const std::filesystem::path path = logsFinal.empty() ? logs / FILE_NAME : std::filesystem::path(logsFinal) / FILE_NAME;
    return Opened{.path = path, .sink = std::make_shared<HandleFileSink>(std::move(*file))};
}
#endif

} // namespace

std::filesystem::path systemTempFolder()
{
    return std::filesystem::temp_directory_path();
}

std::optional<std::filesystem::path> systemProgramDataFolder()
{
#ifdef _WIN32
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path);
    // Freed whether or not the call succeeded, as SHGetKnownFolderPath's contract asks.
    const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> owned(path, &::CoTaskMemFree);
    if (FAILED(result) || owned == nullptr || owned.get()[0] == L'\0')
    {
        return std::nullopt;
    }
    return std::filesystem::path(owned.get());
#else
    return std::nullopt;
#endif
}

std::filesystem::path elevatedLogFolder(const std::filesystem::path& programData)
{
    return programData / "TaskSmack" / "Logs";
}

#ifdef _WIN32
std::wstring systemFinalPath(void* handle)
{
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const DWORD length =
            GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0)
        {
            return {};
        }
        if (length < path.size())
        {
            path.resize(length);
            constexpr std::wstring_view LONG_PREFIX = LR"(\\?\)";
            if (path.starts_with(LONG_PREFIX) && !path.starts_with(LR"(\\?\UNC\)"))
            {
                path.erase(0, LONG_PREFIX.size());
            }
            return path;
        }
        path.assign(length, L'\0');
    }
    return {};
}

std::expected<void, std::string> secureFolder(const std::filesystem::path& folder, std::string_view sddl)
{
    const OwnedDescriptor descriptor = descriptorFrom(sddl);
    if (descriptor == nullptr)
    {
        return std::unexpected(std::string("its security can't be built"));
    }
    const OwnedHandle parent = own(CreateFileW(folder.parent_path().c_str(),
                                               FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                                               nullptr,
                                               OPEN_EXISTING,
                                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                               nullptr));
    if (parent == nullptr)
    {
        return std::unexpected(std::format("{} can't be opened: {}", Core::pathToUtf8(folder.parent_path()), lastError()));
    }
    auto held = secureChild(parent.get(), folder.filename().native(), descriptor.get(), Core::pathToUtf8(folder), {});
    if (!held.has_value())
    {
        return std::unexpected(std::move(held.error()));
    }
    return {};
}
#endif

std::expected<Opened, std::string> open([[maybe_unused]] bool lessPrivilegedEnvironment, const Functions& functions)
{
    try
    {
#ifdef _WIN32
        if (lessPrivilegedEnvironment)
        {
            return openElevated(functions);
        }
#endif
        // Opened by its native path (UTF-16 on Windows, SPDLOG_WCHAR_FILENAMES), never a narrowed one, so a temp folder
        // with any characters works with or without the manifest (#1648).
        std::filesystem::path path = functions.tempFolder() / FILE_NAME;
        spdlog::sink_ptr sink = Core::makeLogFileSink(path, true);
        return Opened{.path = std::move(path), .sink = std::move(sink)};
    }
    catch (const std::exception& e)
    {
        return std::unexpected(std::string(e.what()));
    }
    catch (...)
    {
        return std::unexpected(std::string("unknown error"));
    }
}

} // namespace App::DebugLogFile
