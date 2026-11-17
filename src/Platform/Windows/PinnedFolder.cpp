#include "PinnedFolder.h"

#include "Platform/Windows/HandleRelativeFile.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

/// What a volume or Windows without FileRenameInformationEx answers it.
constexpr auto STATUS_BAD_INFO_CLASS = static_cast<NTSTATUS>(0xC0000003L);
constexpr auto STATUS_BAD_PARAMETER = static_cast<NTSTATUS>(0xC000000DL);
constexpr auto STATUS_UNSUPPORTED = static_cast<NTSTATUS>(0xC00000BBL);
/// FILE_RENAME_INFORMATION_EX's flags (ntifs.h): replace the target, and with POSIX semantics, even one held open.
constexpr DWORD FILE_RENAME_FLAGS_REPLACE_IF_EXISTS = 0x00000001;
constexpr DWORD FILE_RENAME_FLAGS_POSIX_SEMANTICS = 0x00000002;

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

} // namespace

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
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return std::unexpected(std::format("{} has become a reparse point since it was checked", utf8(m_Path)));
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return std::unexpected(std::format("{} is no longer a folder as it was when it was checked", utf8(m_Path)));
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
    // Relative to the folder held: nothing above it is looked up again, and a link is opened itself, never followed.
    auto opened = openRelative(parent,
                               name,
                               RelativeOpen{
                                   .access = how.access | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                                   .share = how.share,
                                   .disposition = how.disposition,
                                   .kind = how.directory ? EntryKind::Directory : EntryKind::File,
                               },
                               m_Functions.ntCreateFile);
    if (!opened.has_value() && isNotFound(opened.error()) && how.disposition == FILE_OPEN)
    {
        return UniqueHandle{}; // not there
    }
    if (!opened.has_value())
    {
        return std::unexpected(std::format("couldn't open {}: {}", WinString::wideToUtf8(expected), ntStatusText(opened.error())));
    }
    UniqueHandle file = std::move(opened->handle);
    // A file this open created and may delete (replaceFile()'s staging file) is removed if a check below refuses it:
    // nothing it made is left behind. Disarmed once every check has passed.
    class RemoveIfRefused
    {
      public:
        explicit RemoveIfRefused(HANDLE created) noexcept : m_Created(created)
        {}
        RemoveIfRefused(const RemoveIfRefused&) = delete;
        RemoveIfRefused& operator=(const RemoveIfRefused&) = delete;
        RemoveIfRefused(RemoveIfRefused&&) = delete;
        RemoveIfRefused& operator=(RemoveIfRefused&&) = delete;
        ~RemoveIfRefused()
        {
            if (m_Created != nullptr)
            {
                markDeleteOnClose(m_Created);
            }
        }
        void disarm() noexcept
        {
            m_Created = nullptr;
        }

      private:
        HANDLE m_Created;
    };
    RemoveIfRefused removeIfRefused(how.disposition == FILE_CREATE && (how.access & DELETE) != 0 ? file.get() : nullptr);
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
    removeIfRefused.disarm();
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
    // Removed on every way out but a rename that published it: a failed write or rename, a refusal, an exception.
    class StagingGuard
    {
      public:
        explicit StagingGuard(HANDLE file) noexcept : m_File(file)
        {}
        StagingGuard(const StagingGuard&) = delete;
        StagingGuard& operator=(const StagingGuard&) = delete;
        StagingGuard(StagingGuard&&) = delete;
        StagingGuard& operator=(StagingGuard&&) = delete;
        ~StagingGuard()
        {
            if (m_File != nullptr)
            {
                markDeleteOnClose(m_File); // deleted when the handle closes, just after this
            }
        }
        void published() noexcept
        {
            m_File = nullptr;
        }

      private:
        HANDLE m_File;
    };
    StagingGuard guard(staging.get()); // declared after staging, so it runs before staging closes
    std::string_view rest = contents;
    while (!rest.empty())
    {
        const DWORD size = static_cast<DWORD>(std::min<std::size_t>(rest.size(), std::size_t{1} << 20));
        DWORD written = 0;
        if (WriteFile(staging.get(), rest.data(), size, &written, nullptr) == FALSE || written == 0)
        {
            return std::unexpected(std::format("couldn't write {}: {}", utf8(m_Path / stagingName), lastErrorText()));
        }
        rest.remove_prefix(written);
    }
    // Renamed over @p name in the held folder: RootDirectory is the held handle, so the name is looked up there.
    const std::wstring& target = components->front();
    // POSIX semantics, so a config.toml an indexer or antivirus holds open (with FILE_SHARE_DELETE) is still replaced;
    // FileRenameInformation, replace only, where the volume has no Ex.
    NTSTATUS renamed = renameRelative(staging.get(),
                                      m_Handle.get(),
                                      target,
                                      FILE_RENAME_INFORMATION_EX_CLASS,
                                      FILE_RENAME_FLAGS_REPLACE_IF_EXISTS | FILE_RENAME_FLAGS_POSIX_SEMANTICS,
                                      m_Functions.ntSetInformationFile);
    if (renamed == STATUS_BAD_INFO_CLASS || renamed == STATUS_UNSUPPORTED || renamed == STATUS_BAD_PARAMETER)
    {
        renamed = renameRelative(staging.get(),
                                 m_Handle.get(),
                                 target,
                                 FILE_RENAME_INFORMATION_CLASS,
                                 FILE_RENAME_FLAGS_REPLACE_IF_EXISTS, // ReplaceIfExists = TRUE
                                 m_Functions.ntSetInformationFile);
    }
    if (renamed < 0)
    {
        return std::unexpected(std::format("couldn't replace {}: {}", utf8(m_Path / name), ntStatusText(renamed)));
    }
    guard.published();
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
