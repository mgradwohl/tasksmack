#include "PinnedFolder.h"

#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcAddress.h"

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
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// NTSTATUS values NtCreateFile answers for a name that isn't there.
constexpr auto STATUS_NAME_NOT_FOUND = static_cast<NTSTATUS>(0xC0000034L);
constexpr auto STATUS_PATH_NOT_FOUND = static_cast<NTSTATUS>(0xC000003AL);
constexpr auto STATUS_NO_ENTRY_POINT = static_cast<NTSTATUS>(0xC000007AL);

/// FileRenameInformation, which winternl.h's FILE_INFORMATION_CLASS doesn't name.
constexpr ULONG FILE_RENAME_INFORMATION_CLASS = 10;

/// The most of a file read from the folder: config.toml and a theme are a few kilobytes.
constexpr std::size_t MAX_READ_BYTES = std::size_t{16} * 1024 * 1024;

/// Whether @p left and @p right are the same path, ignoring case as NTFS does.
[[nodiscard]] bool samePath(std::wstring_view left, std::wstring_view right) noexcept
{
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) ==
           CSTR_EQUAL;
}

/// @p relative's components, each a plain name: nothing absolute, no ".", "..", stream (':') or empty component.
[[nodiscard]] std::optional<std::vector<std::wstring>> componentsOf(const std::filesystem::path& relative)
{
    if (relative.empty() || relative.has_root_name() || relative.has_root_directory())
    {
        return std::nullopt;
    }
    std::vector<std::wstring> components;
    for (const std::filesystem::path& part : relative)
    {
        const std::wstring& name = part.native();
        if (name.empty() || name == L"." || name == L".." || name.find_first_of(L":\\/") != std::wstring::npos)
        {
            return std::nullopt;
        }
        components.push_back(name);
    }
    return components;
}

[[nodiscard]] std::string utf8(const std::filesystem::path& path)
{
    return WinString::wideToUtf8(path.native());
}

[[nodiscard]] std::string ntStatusText(NTSTATUS status)
{
    return std::format("NTSTATUS 0x{:08X}", static_cast<std::uint32_t>(status));
}

[[nodiscard]] std::string lastErrorText()
{
    return std::system_category().message(static_cast<int>(GetLastError()));
}

/// Marks @p file, opened with DELETE, to be deleted when its handle closes.
void deleteOnClose(HANDLE file) noexcept
{
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    static_cast<void>(SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)));
}

} // namespace

NTSTATUS NTAPI PinnedFolderFunctions::systemNtCreateFile(PHANDLE handle,
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
        return STATUS_NO_ENTRY_POINT; // LCOV_EXCL_LINE - ntdll always exports it
    }
    return create(handle, access, attributes, status, allocationSize, fileAttributes, share, disposition, options, eaBuffer, eaLength);
}

NTSTATUS NTAPI
PinnedFolderFunctions::systemNtSetInformationFile(HANDLE file, PIO_STATUS_BLOCK status, PVOID information, ULONG length, ULONG what)
{
    static auto* const set = getProcAddress<NtSetInformationFileFn>(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile");
    if (set == nullptr)
    {
        return STATUS_NO_ENTRY_POINT; // LCOV_EXCL_LINE - ntdll always exports it
    }
    return set(file, status, information, length, what);
}

std::optional<std::wstring> finalPathOf(HANDLE handle, PinnedFolderFunctions::FinalPathFn getFinalPath)
{
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const DWORD length = getFinalPath(handle, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0)
        {
            return std::nullopt;
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
        path.assign(length, L'\0'); // too short: length is the size it needs, with its terminator
    }
    return std::nullopt;
}

PinnedFolder::PinnedFolder(std::filesystem::path path, UniqueHandle handle, PinnedFolderFunctions functions)
    : m_Path(std::move(path)), m_Handle(std::move(handle)), m_Functions(functions)
{
    m_FinalPath = finalPathOf(m_Handle.get(), m_Functions.getFinalPath).value_or(std::wstring{});
}

std::expected<void, std::string> PinnedFolder::stillHeld() const
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (m_FinalPath.empty() || m_Functions.getFileInformation(m_Handle.get(), &info) == FALSE)
    {
        return std::unexpected(std::format("{} can't be read through the handle held on it", utf8(m_Path)));
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return std::unexpected(std::format("{} has become a reparse point since it was checked", utf8(m_Path)));
    }
    const std::optional<std::wstring> now = finalPathOf(m_Handle.get(), m_Functions.getFinalPath);
    if (!now.has_value() || !samePath(*now, m_FinalPath))
    {
        return std::unexpected(std::format("{} is no longer where it was checked", utf8(m_Path)));
    }
    return {};
}

std::expected<UniqueHandle, std::string>
PinnedFolder::openIn(HANDLE parent, const std::wstring& parentPath, const std::wstring& name, const Open& how) const
{
    const std::wstring expected = parentPath + L'\\' + name;
    UNICODE_STRING text{};
    text.Buffer = const_cast<PWSTR>(name.c_str()); // NOLINT(cppcoreguidelines-pro-type-const-cast) - NtCreateFile only reads it
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent; // relative to the folder held: nothing above it is looked up again
    attributes.ObjectName = &text;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK status{};
    HANDLE opened = nullptr;
    const ULONG options =
        FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | (how.directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    const NTSTATUS result = m_Functions.ntCreateFile(&opened,
                                                     how.access | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                                                     &attributes,
                                                     &status,
                                                     nullptr,
                                                     FILE_ATTRIBUTE_NORMAL,
                                                     how.share,
                                                     how.disposition,
                                                     options,
                                                     nullptr,
                                                     0);
    if ((result == STATUS_NAME_NOT_FOUND || result == STATUS_PATH_NOT_FOUND) && how.disposition == FILE_OPEN)
    {
        return UniqueHandle{}; // not there
    }
    if (result < 0)
    {
        return std::unexpected(std::format("couldn't open {}: {}", WinString::wideToUtf8(expected), ntStatusText(result)));
    }
    UniqueHandle file(opened);
    BY_HANDLE_FILE_INFORMATION info{};
    if (m_Functions.getFileInformation(file.get(), &info) == FALSE)
    {
        return std::unexpected(std::format("couldn't read {}: {}", WinString::wideToUtf8(expected), lastErrorText()));
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return std::unexpected(std::format("{} is a link, junction or mount point", WinString::wideToUtf8(expected)));
    }
    if (!how.directory && info.nNumberOfLinks != 1)
    {
        return std::unexpected(std::format("{} is hard-linked to another file", WinString::wideToUtf8(expected)));
    }
    if (const std::optional<std::wstring> actual = finalPathOf(file.get(), m_Functions.getFinalPath);
        !actual.has_value() || !samePath(*actual, expected))
    {
        return std::unexpected(std::format("{} opened outside the folder held: {}",
                                           WinString::wideToUtf8(expected),
                                           actual.has_value() ? WinString::wideToUtf8(*actual) : std::string("(its path can't be read)")));
    }
    return file;
}

std::expected<void*, std::string> PinnedFolder::openLockFile(const std::filesystem::path& name) const
{
    const auto components = componentsOf(name);
    if (!components.has_value() || components->size() != 1)
    {
        return std::unexpected(std::format("{} isn't a file name", utf8(name)));
    }
    if (auto held = stillHeld(); !held.has_value())
    {
        return std::unexpected(std::move(held.error()));
    }
    // Shared, as InstanceLock opens it by path: its byte-range lock is what excludes a second instance.
    auto file = openIn(m_Handle.get(),
                       m_FinalPath,
                       components->front(),
                       Open{
                           .access = GENERIC_READ | GENERIC_WRITE,
                           .share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           .disposition = FILE_OPEN_IF,
                           .directory = false,
                       });
    if (!file.has_value())
    {
        return std::unexpected(std::move(file.error()));
    }
    return file->release();
}

std::expected<std::pair<UniqueHandle, std::wstring>, std::string> PinnedFolder::openFolder(const std::filesystem::path& relative) const
{
    const auto components = componentsOf(relative);
    if (!components.has_value())
    {
        return std::unexpected(std::format("{} isn't a folder in {}", utf8(relative), utf8(m_Path)));
    }
    UniqueHandle folder;
    std::wstring folderPath = m_FinalPath;
    for (const std::wstring& component : *components)
    {
        auto next = openIn(folder ? folder.get() : m_Handle.get(),
                           folderPath,
                           component,
                           Open{
                               .access = FILE_LIST_DIRECTORY | FILE_TRAVERSE,
                               .share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               .disposition = FILE_OPEN,
                               .directory = true,
                           });
        if (!next.has_value())
        {
            return std::unexpected(std::move(next.error()));
        }
        if (!*next)
        {
            return std::pair{UniqueHandle{}, std::wstring{}}; // not there
        }
        folder = std::move(*next);
        folderPath += L'\\' + component;
    }
    return std::pair{std::move(folder), std::move(folderPath)};
}

std::expected<std::optional<std::string>, std::string> PinnedFolder::readFile(const std::filesystem::path& relative) const
{
    const auto components = componentsOf(relative);
    if (!components.has_value())
    {
        return std::unexpected(std::format("{} isn't a file in {}", utf8(relative), utf8(m_Path)));
    }
    if (auto held = stillHeld(); !held.has_value())
    {
        return std::unexpected(std::move(held.error()));
    }
    UniqueHandle parentOwned;
    HANDLE parent = m_Handle.get();
    std::wstring parentPath = m_FinalPath;
    if (components->size() > 1)
    {
        auto folder = openFolder(relative.parent_path());
        if (!folder.has_value())
        {
            return std::unexpected(std::move(folder.error()));
        }
        if (!folder->first)
        {
            return std::optional<std::string>{}; // its folder isn't there
        }
        parentOwned = std::move(folder->first);
        parent = parentOwned.get();
        parentPath = std::move(folder->second);
    }
    auto file = openIn(parent,
                       parentPath,
                       components->back(),
                       Open{
                           .access = FILE_GENERIC_READ,
                           .share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           .disposition = FILE_OPEN,
                           .directory = false,
                       });
    if (!file.has_value())
    {
        return std::unexpected(std::move(file.error()));
    }
    if (!*file)
    {
        return std::optional<std::string>{};
    }
    std::string contents;
    std::vector<char> chunk(std::size_t{64} * 1024);
    for (;;)
    {
        DWORD read = 0;
        if (ReadFile(file->get(), chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr) == FALSE)
        {
            return std::unexpected(std::format("couldn't read {}: {}", utf8(m_Path / relative), lastErrorText()));
        }
        if (read == 0)
        {
            break;
        }
        if (contents.size() + read > MAX_READ_BYTES)
        {
            return std::unexpected(std::format("{} is larger than TaskSmack reads", utf8(m_Path / relative)));
        }
        contents.append(chunk.data(), read);
    }
    return std::optional<std::string>(std::move(contents));
}

std::expected<void, std::string> PinnedFolder::replaceFile(const std::filesystem::path& name, std::string_view contents) const
{
    const auto components = componentsOf(name);
    if (!components.has_value() || components->size() != 1)
    {
        return std::unexpected(std::format("{} isn't a file name", utf8(name)));
    }
    if (auto held = stillHeld(); !held.has_value())
    {
        return std::unexpected(std::move(held.error()));
    }
    // A new file of this save's own beside it (FILE_CREATE: never one that is already there), with DELETE access so
    // it is renamed, or removed on a failure, through this same handle.
    std::random_device random;
    UniqueHandle staging;
    std::wstring stagingName;
    for (int attempt = 0; attempt < 8 && !staging; ++attempt)
    {
        stagingName = std::format(L".tasksmack-config.{:08x}.tmp", m_TempNameSource ? m_TempNameSource() : random());
        auto created = openIn(m_Handle.get(),
                              m_FinalPath,
                              stagingName,
                              Open{.access = FILE_GENERIC_WRITE | DELETE, .share = 0, .disposition = FILE_CREATE, .directory = false});
        if (created.has_value())
        {
            staging = std::move(*created);
        }
        else if (attempt == 7)
        {
            return std::unexpected(std::move(created.error()));
        }
    }
    std::string_view rest = contents;
    while (!rest.empty())
    {
        const DWORD size = static_cast<DWORD>(std::min<std::size_t>(rest.size(), std::size_t{1} << 20));
        DWORD written = 0;
        if (WriteFile(staging.get(), rest.data(), size, &written, nullptr) == FALSE || written == 0)
        {
            const std::string why = lastErrorText();
            deleteOnClose(staging.get());
            return std::unexpected(std::format("couldn't write {}: {}", utf8(m_Path / stagingName), why));
        }
        rest.remove_prefix(written);
    }
    // Renamed over @p name in the held folder: RootDirectory is the held handle, so the name is looked up there.
    const std::wstring& target = components->front();
    const std::size_t nameBytes = target.size() * sizeof(wchar_t);
    // FILE_RENAME_INFORMATION, which FILE_RENAME_INFO mirrors, handed to NtSetInformationFile itself: kernel32's
    // SetFileInformationByHandle refuses a relative name with a RootDirectory (ERROR_INVALID_PARAMETER).
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(
        buffer.data());             // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - a variable-length Win32 struct
    rename->ReplaceIfExists = TRUE; // NOLINT(cppcoreguidelines-pro-type-union-access) - FILE_RENAME_INFO's own union
    rename->RootDirectory = m_Handle.get();
    rename->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(static_cast<void*>(rename->FileName), target.c_str(), nameBytes + sizeof(wchar_t));
    IO_STATUS_BLOCK renameStatus{};
    if (const NTSTATUS renamed = m_Functions.ntSetInformationFile(
            staging.get(), &renameStatus, rename, static_cast<ULONG>(buffer.size()), FILE_RENAME_INFORMATION_CLASS);
        renamed < 0)
    {
        deleteOnClose(staging.get());
        return std::unexpected(std::format("couldn't replace {}: {}", utf8(m_Path / name), ntStatusText(renamed)));
    }
    const std::wstring expected = m_FinalPath + L'\\' + target;
    if (const auto actual = finalPathOf(staging.get(), m_Functions.getFinalPath); !actual.has_value() || !samePath(*actual, expected))
    {
        return std::unexpected(std::format("{} was renamed outside the folder held", utf8(m_Path / name))); // LCOV_EXCL_LINE
    }
    return {};
}

std::expected<void, std::string> PinnedFolder::createFolder(const std::filesystem::path& name) const
{
    const auto components = componentsOf(name);
    if (!components.has_value() || components->size() != 1)
    {
        return std::unexpected(std::format("{} isn't a folder name", utf8(name)));
    }
    if (auto held = stillHeld(); !held.has_value())
    {
        return std::unexpected(std::move(held.error()));
    }
    auto folder = openIn(m_Handle.get(),
                         m_FinalPath,
                         components->front(),
                         Open{
                             .access = FILE_LIST_DIRECTORY,
                             .share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             .disposition = FILE_OPEN_IF,
                             .directory = true,
                         });
    if (!folder.has_value())
    {
        return std::unexpected(std::move(folder.error()));
    }
    return {};
}

std::expected<std::vector<std::filesystem::path>, std::string> PinnedFolder::listFiles(const std::filesystem::path& name) const
{
    if (auto held = stillHeld(); !held.has_value())
    {
        return std::unexpected(std::move(held.error()));
    }
    auto folder = openFolder(name);
    if (!folder.has_value())
    {
        return std::unexpected(std::move(folder.error()));
    }
    std::vector<std::filesystem::path> files;
    if (!folder->first)
    {
        return files; // not there
    }
    // FILE_FULL_DIR_INFO records, each DWORD-aligned, NextEntryOffset apart; the buffer is filled afresh each call.
    std::vector<std::uint64_t> buffer(std::size_t{8} * 1024);
    FILE_INFO_BY_HANDLE_CLASS what = FileFullDirectoryRestartInfo;
    for (;;)
    {
        if (GetFileInformationByHandleEx(
                folder->first.get(), what, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t))) == FALSE)
        {
            if (GetLastError() == ERROR_NO_MORE_FILES)
            {
                break;
            }
            return std::unexpected(std::format("couldn't list {}: {}", utf8(m_Path / name), lastErrorText()));
        }
        what = FileFullDirectoryInfo;
        const auto* bytes =
            reinterpret_cast<const std::byte*>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - Win32 records
        for (std::size_t offset = 0;;)
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic) - Win32 records
            const auto* entry = reinterpret_cast<const FILE_FULL_DIR_INFO*>(bytes + offset);
            if ((entry->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0)
            {
                files.emplace_back(std::wstring(static_cast<const wchar_t*>(entry->FileName), entry->FileNameLength / sizeof(wchar_t)));
            }
            if (entry->NextEntryOffset == 0)
            {
                break;
            }
            offset += entry->NextEntryOffset;
        }
    }
    std::ranges::sort(files);
    return files;
}

} // namespace Platform::Windows
