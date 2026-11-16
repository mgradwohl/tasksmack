#pragma once

// The file a running process's image was mapped from, opened and held (#3861, #4021): the one Look up on VirusTotal
// hashes (WindowsProcessActions::imageSha256()) and the one the Signature and Publisher columns check
// (WindowsProcessProbe's signature worker). Not the file at QueryFullProcessImageNameW's path: that is the name at
// creation, and a running image can be renamed (its section shares delete) and another file put at the old name.
//
// GetMappedFileNameW at the image (its entry point, ProcessImageInformation's TransferAddress, or for an image the
// loader mapped elsewhere the image still named as the process started) names the image's file as it is named now.
// That name is opened through \\?\GLOBALROOT without FILE_SHARE_DELETE, so the file held can't be renamed or deleted
// while it is held. Then the image's name is read again, and the file at that name must be the held one by identity
// (volume serial and file ID, never by name, so a mapped network drive's, a short 8.3 name's or another case's form of
// the same name matches), and the held file must have the image's size.
//
// What this can't rule out from user mode: a process that rewrites its own file after its image section was made
// (Process Herpaderping: the section keeps the original pages while the file on disk holds other bytes, of any size
// it likes) reads as the file, not as the pages it runs; and a process that maps another image where its own would
// be. The size check catches a careless swap, not a deliberate one. A check of the content against the mapped pages
// would need to read the process's memory.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include "WindowsHandles.h"
#include "WindowsNtQuery.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The calls openProcessImageFile() makes on the process's handle: kernel32's and ntdll's, or a test's fakes. The
/// file itself is opened with CreateFileW either way.
struct ImageFileFunctions
{
    /// ntdll's NtQueryInformationProcess, for ProcessImageInformation (the entry point and the image's size).
    NtQueryInformationProcessFn ntQueryInformationProcess = nullptr;
    /// GetMappedFileNameW() (K32GetMappedFileNameW): the NT name of the file mapped at an address now.
    DWORD(WINAPI* getMappedFileName)(HANDLE process, LPVOID address, LPWSTR name, DWORD size) = nullptr;
    /// VirtualQueryEx(): whether the entry point is in an image mapping, and the scan for an image mapped elsewhere.
    /// Null: the entry point is taken as the image, and no scan.
    SIZE_T(WINAPI* virtualQueryEx)(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size) = nullptr;
    /// QueryFullProcessImageNameW(): the name the process started with, for an image mapped elsewhere. Null: no scan.
    BOOL(WINAPI* queryFullProcessImageName)(HANDLE process, DWORD flags, LPWSTR path, PDWORD size) = nullptr;
    /// GetExitCodeProcess(): whether a process whose image's name can't be read again has exited. Nullable.
    BOOL(WINAPI* getExitCodeProcess)(HANDLE process, LPDWORD exitCode) = nullptr;
};

/// The system's calls.
[[nodiscard]] ImageFileFunctions systemImageFileFunctions() noexcept;

/// A file's identity: its volume's serial and its file ID, the 128-bit one (FileIdInfo, which ReFS needs) or, where
/// the file system has none, the 64-bit index (GetFileInformationByHandle, as FAT and some redirectors answer).
struct FileIdentity
{
    ULONGLONG volume = 0;
    FILE_ID_128 id{};

    [[nodiscard]] bool operator==(const FileIdentity& other) const noexcept;
};

/// @p file's identity, or nullopt when neither call answers.
[[nodiscard]] std::optional<FileIdentity> fileIdentity(HANDLE file);

/// The file @p process's image was mapped from (see the top of this file), open for GENERIC_READ and shared for read
/// only, or why not. @p process is a handle with PROCESS_QUERY_LIMITED_INFORMATION, already checked to be the
/// process meant (Windows::openProcessVerified()); @p pid names it in the messages. With @p entryPointOnly, an image
/// that isn't at its entry point is refused rather than looked for by the name it started with, which a process can
/// put another mapped file under (the signature check, #4021). @p notDone (may be empty) ends a refusal that
/// says what wasn't done with the file ("it wasn't hashed").
[[nodiscard]] std::expected<UniqueHandle, std::string> openProcessImageFile(
    const ImageFileFunctions& fns, HANDLE process, std::int32_t pid, std::string_view notDone, bool entryPointOnly = false);

/// The name @p file has now (GetFinalPathNameByHandleW): a drive-letter path where its volume has one, else its NT
/// name under \\?\GLOBALROOT. Empty when it can't be read. While the file is held without FILE_SHARE_DELETE (as
/// openProcessImageFile() holds it) it can't be renamed, so the name stays its.
[[nodiscard]] std::wstring heldFileName(HANDLE file);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
