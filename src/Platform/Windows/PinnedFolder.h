#pragma once

// The handed-over config folder an elevated restart checked and holds (#2728, #3787), and every file TaskSmack opens
// or creates in it (#3774). Each one is opened by NtCreateFile relative to the held folder's handle
// (OBJECT_ATTRIBUTES.RootDirectory), one path component at a time, never by a path from the drive down, so nothing
// on the way to it is looked up again. Each open:
//   - first checks, through the held handle, that the folder is still no reparse point and still at its final path
//     (a folder that becomes a reparse point after the pin is used no more);
//   - opens a link, junction or mount point itself (FILE_OPEN_REPARSE_POINT), never what it names, and refuses it;
//   - refuses a file with more than one hard link;
//   - checks that the opened file's final path is inside the held folder, where it was asked for.
// A replaced file (config.toml) is written to a new file beside it, then renamed over it through FileRenameInfo with
// the held folder as RootDirectory. The NT and Win32 calls are function pointers, so the tests drive each answer. The
// handle-relative opens and the rename are HandleRelativeFile.h's, shared with App::DebugLogFile (#4143).

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h> // NtCreateFile's OBJECT_ATTRIBUTES, IO_STATUS_BLOCK and UNICODE_STRING
// clang-format on

#include "Platform/IHandedOverFolder.h"
#include "Platform/Windows/HandleRelativeFile.h" // openRelative(), renameRelative(), finalPathOf()
#include "Platform/Windows/WindowsHandles.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows
{

/// The calls PinnedFolder makes: the system's, or a test's fakes.
struct PinnedFolderFunctions
{
    using NtCreateFileFn = Platform::Windows::NtCreateFileFn;
    using NtSetInformationFileFn = Platform::Windows::NtSetInformationFileFn; ///< Its class as the ULONG the enum is passed as
    using FileInformationFn = BOOL(WINAPI*)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
    using FinalPathFn = GetFinalPathFn;

    NtCreateFileFn ntCreateFile = &systemNtCreateFile;
    NtSetInformationFileFn ntSetInformationFile = &systemNtSetInformationFile; ///< The rename, whose RootDirectory kernel32 won't pass
    FileInformationFn getFileInformation = &::GetFileInformationByHandle;
    FinalPathFn getFinalPath = &::GetFinalPathNameByHandleW;
};

/// A handed-over config folder that passed ElevatedRestart::checkConfigDir(), held open by its handle for as long as
/// this lives, and the one way into it.
class PinnedFolder final : public IHandedOverFolder
{
  public:
    /// Holds @p handle, the folder at @p path; its final path is read now, and every later open is checked against it.
    PinnedFolder(std::filesystem::path path, UniqueHandle handle, PinnedFolderFunctions functions = {});

    [[nodiscard]] const std::filesystem::path& path() const noexcept override
    {
        return m_Path;
    }

    /// The held folder's handle, still owned by this object.
    [[nodiscard]] HANDLE handle() const noexcept
    {
        return m_Handle.get();
    }

    [[nodiscard]] std::expected<void*, std::string> openLockFile(const std::filesystem::path& name) const override;
    [[nodiscard]] std::expected<std::optional<std::string>, std::string> readFile(const std::filesystem::path& relative) const override;
    [[nodiscard]] std::expected<void, std::string> replaceFile(const std::filesystem::path& name, std::string_view contents) const override;
    [[nodiscard]] std::expected<void, std::string> createFolder(const std::filesystem::path& name) const override;
    [[nodiscard]] std::expected<std::vector<std::filesystem::path>, std::string>
    listFiles(const std::filesystem::path& name) const override;

    /// Testing only: names replaceFile()'s new file instead of a random number.
    void setTempNameSourceForTesting(std::function<std::uint32_t()> source)
    {
        m_TempNameSource = std::move(source);
    }

  private:
    struct Open
    {
        ACCESS_MASK access = 0;
        ULONG share = 0;
        ULONG disposition = 0;
        bool directory = false;
    };

    /// Whether the held folder is still as it was checked: no reparse point, at its final path.
    [[nodiscard]] std::expected<void, std::string> stillHeld() const;
    /// @p name, one path component, opened in the folder @p parent (whose final path is @p parentPath) as @p how
    /// says, with the checks in this file's header. An invalid handle when it isn't there (and @p how doesn't
    /// create it).
    [[nodiscard]] std::expected<UniqueHandle, std::string>
    openIn(HANDLE parent, const std::wstring& parentPath, const std::wstring& name, const Open& how) const;
    /// The folder @p relative (components below the held folder) opened, and its final path; an invalid handle
    /// when one of them isn't there.
    [[nodiscard]] std::expected<std::pair<UniqueHandle, std::wstring>, std::string> openFolder(const std::filesystem::path& relative) const;

    std::filesystem::path m_Path;
    UniqueHandle m_Handle;
    PinnedFolderFunctions m_Functions;
    std::wstring m_FinalPath; ///< The held folder's final path when it was taken; empty when it couldn't be read
    std::function<std::uint32_t()> m_TempNameSource;
};

} // namespace Platform::Windows

#endif // _WIN32
