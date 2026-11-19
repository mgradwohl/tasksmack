#pragma once

// The file a running process's image was mapped from, opened and held (#3861, #4021): the one Look up on VirusTotal
// hashes (WindowsProcessActions::imageSha256()) and the one the Signature and Publisher columns check
// (WindowsProcessProbe's signature worker). Not the file at QueryFullProcessImageNameW's path: that is the name at
// creation, and a running image can be renamed (its section shares delete) and another file put at the old name.
//
// GetMappedFileNameW at the image's entry point (ProcessImageInformation's TransferAddress) names the image's file as
// it is named now. That name is opened without FILE_SHARE_DELETE, so the file held can't be renamed or deleted while
// it is held, by Windows::openLocalNtFile() (WindowsLocalNtFile.h, #4482): NtCreateFile on the NT name itself, never
// through \\?\GLOBALROOT, with no junction or symlink on the way or at the name followed and identification-level
// security, so a link the process swaps in under its image's name can't make TaskSmack connect to a share; a network
// share's image, or one on a device TaskSmack can't tell is a local disk, isn't opened or read, and so isn't hashed or
// checked ("it isn't on a drive TaskSmack can check"). Then the image's name is read again, and the file at that name,
// opened the same way, must be the held one by identity (volume serial and file ID, never by name, so a short 8.3
// name's or another case's form of the same name matches), and the held file must have the image's size.
//
// What this can't rule out from user mode: a process that rewrites its own file after its image section was made
// (Process Herpaderping: the section keeps the original pages while the file on disk holds other bytes, of any size
// it likes) reads as the file, not as the pages it runs; and a process that maps another image where its own would
// be. The size check catches a careless swap, not a deliberate one. A check of the content against the mapped pages
// would need to read the process's memory.
//
// Nor can it confirm an image that isn't mapped at its entry point (a .NET Framework executable, or a native one
// relocated off a preferred base that was taken), so such an image is refused, never looked for elsewhere (#4021,
// #4057). It used to be looked for by the name the process started with, but a process can put any file under that
// name: rename its own exe away, copy a signed one (notepad.exe) to the old name, map that decoy with
// LoadLibraryExW(LOAD_LIBRARY_AS_IMAGE_RESOURCE), and pad its own exe to the decoy's size beforehand so the size
// check passes. Nothing in the process's own headers or its PEB (ImageBaseAddress) can tell the two apart, because
// the process writes both. So Look up on VirusTotal doesn't hash such an image, and the Signature and Publisher
// columns don't check it.

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
#include "WindowsLocalNtFile.h"
#include "WindowsNtQuery.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The calls openProcessImageFile() makes: kernel32's and ntdll's, or a test's fakes.
struct ImageFileFunctions
{
    /// ntdll's NtQueryInformationProcess, for ProcessImageInformation (the entry point and the image's size).
    NtQueryInformationProcessFn ntQueryInformationProcess = nullptr;
    /// GetMappedFileNameW() (K32GetMappedFileNameW): the NT name of the file mapped at an address now.
    DWORD(WINAPI* getMappedFileName)(HANDLE process, LPVOID address, LPWSTR name, DWORD size) = nullptr;
    /// VirtualQueryEx(): whether the entry point is in an image mapping. Null: the entry point is taken as the image.
    SIZE_T(WINAPI* virtualQueryEx)(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size) = nullptr;
    /// GetExitCodeProcess(): whether a process whose image's name can't be read again has exited. Nullable.
    BOOL(WINAPI* getExitCodeProcess)(HANDLE process, LPDWORD exitCode) = nullptr;
    /// GetProcessInformation(), for ProcessProtectionLevelInfo: whether a process whose image can't be found is a
    /// protected one, so the refusal says so (#4057). Nullable: then it reads as not protected.
    BOOL(WINAPI* getProcessInformation)(HANDLE process, PROCESS_INFORMATION_CLASS infoClass, LPVOID info, DWORD size) = nullptr;
    /// The calls the image's file is opened by its NT name with (Windows::openLocalNtFile(), #4482): the held file and
    /// the file under the image's name again. Without them nothing is opened, and the image reads as unreadable.
    LocalFileFunctions localFile{};
};

/// The system's calls.
[[nodiscard]] ImageFileFunctions systemImageFileFunctions() noexcept;

/// Whether @p process is a protected process (or a PPL), by fns.getProcessInformation's ProcessProtectionLevelInfo.
/// No call, or a failed one, reads as not protected.
[[nodiscard]] bool isProtectedProcess(const ImageFileFunctions& fns, HANDLE process);

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
/// process meant (Windows::openProcessVerified()); @p pid names it in the messages. An image that isn't at its entry
/// point is refused (see the top of this file), and so is a protected process's, saying so. @p notDone (may be empty)
/// ends a refusal that says what wasn't done with the file ("it wasn't hashed").
[[nodiscard]] std::expected<UniqueHandle, std::string>
openProcessImageFile(const ImageFileFunctions& fns, HANDLE process, std::int32_t pid, std::string_view notDone);

/// The name @p file has now (GetFinalPathNameByHandleW): a drive-letter path where its volume has one, else its NT
/// name under \\?\GLOBALROOT. Empty when it can't be read. While the file is held without FILE_SHARE_DELETE (as
/// openProcessImageFile() holds it) it can't be renamed, so the name stays its.
[[nodiscard]] std::wstring heldFileName(HANDLE file);

/// The CompanyName in the version info block @p block (VS_VERSIONINFO, as GetFileVersionInfoW or
/// heldFilePublisher() reads it), in the first of its translations that names one, else in US English's Unicode
/// or Windows-1252 table. Empty when it names none. @p block is read with VerQueryValueW, so it is given room after
/// the block (GetFileVersionInfoSizeW's). Nothing outside @p block is read, whatever length a node declares (#4057).
[[nodiscard]] std::string versionInfoCompanyName(std::span<BYTE> block);

/// The publisher (the version info's CompanyName) of the file @p file is open on (GENERIC_READ), read through that
/// handle and never by a name (#4057): a read-only data mapping of the file, its PE resource directory walked to
/// RT_VERSION (Windows::findVersionResource(), every offset checked against the file), and versionInfoCompanyName().
/// So a drive letter or subst that points elsewhere by the time a name is opened, or a language .mui beside the
/// file, can't supply it. Empty when the file names none, isn't a PE file, or can't be mapped or read.
[[nodiscard]] std::string heldFilePublisher(HANDLE file);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
