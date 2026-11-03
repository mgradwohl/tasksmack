#pragma once

// Windows' image signature check (#1661, #3070): WinVerifyTrust on a file's own Authenticode signature,
// and on the signed catalog that holds its hash (CryptCATAdminAcquireContext2,
// CryptCATAdminCalcHashFromFileHandle2, CryptCATAdminEnumCatalogFromHash) -- most of Windows' own files
// are catalog-signed. Offline: WTD_REVOKE_NONE and WTD_CACHE_ONLY_URL_RETRIEVAL, so a check never goes to
// the network, and no UI. Milliseconds a file, more for a large one (the whole file is hashed), so it
// runs on a worker, never the UI thread or a sample: the Drivers section's read and the process probe's
// signature worker (WindowsSignatureWorker.h). Both read the file through the one handle openImage()
// opened and read the stamp through, held for the whole check (#3292).
//
// The calls are a table of function pointers, the system's by default, so tests drive the embedded and
// catalog paths, their failures and the releases with fakes (tests/Platform/test_WindowsSignatureCheck.cpp).
// What the answers mean is WindowsImageSignature.h's.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <wincrypt.h>
#include <wintrust.h>
#include <mscat.h>
// clang-format on

#include "WinString.h"
#include "WindowsImageSignature.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The calls openImage(), verifyEmbeddedSignature() and verifyCatalogSignature() make: kernel32's,
/// wintrust's and crypt32's, or a test's fakes. The file itself is opened with CreateFileW either way.
struct TrustApiFunctions
{
    BOOL(WINAPI* getFileInformationByHandleEx)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD) = &::GetFileInformationByHandleEx;
    LONG(WINAPI* winVerifyTrust)(HWND, GUID*, LPVOID) = &::WinVerifyTrust;
    CRYPT_PROVIDER_DATA*(WINAPI* provDataFromStateData)(HANDLE) = &::WTHelperProvDataFromStateData;
    CRYPT_PROVIDER_SGNR*(WINAPI* provSignerFromChain)(CRYPT_PROVIDER_DATA*, DWORD, BOOL, DWORD) = &::WTHelperGetProvSignerFromChain;
    DWORD(WINAPI* certGetNameString)(PCCERT_CONTEXT, DWORD, DWORD, void*, LPWSTR, DWORD) = &::CertGetNameStringW;
    BOOL(WINAPI* acquireContext2)(HCATADMIN*, const GUID*, PCWSTR, PCCERT_STRONG_SIGN_PARA, DWORD) = &::CryptCATAdminAcquireContext2;
    BOOL(WINAPI* calcHashFromFileHandle2)(HCATADMIN, HANDLE, DWORD*, BYTE*, DWORD) = &::CryptCATAdminCalcHashFromFileHandle2;
    HCATINFO(WINAPI* enumCatalogFromHash)(HCATADMIN, BYTE*, DWORD, DWORD, HCATINFO*) = &::CryptCATAdminEnumCatalogFromHash;
    BOOL(WINAPI* catalogInfoFromContext)(HCATINFO, CATALOG_INFO*, DWORD) = &::CryptCATCatalogInfoFromContext;
    BOOL(WINAPI* releaseCatalogContext)(HCATADMIN, HCATINFO, DWORD) = &::CryptCATAdminReleaseCatalogContext;
    BOOL(WINAPI* releaseContext)(HCATADMIN, DWORD) = &::CryptCATAdminReleaseContext;
    BOOL(WINAPI* deviceIoControl)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED) = &::DeviceIoControl;
    /// The signer's name to UTF-8, between VERIFY and CLOSE: it allocates, so tests make it throw (#3339, #3342).
    std::string (*wideToUtf8)(std::wstring_view) = &WinString::wideToUtf8;
};

/// The stamp of the file @p file is open on (#3292): its volume serial and 128-bit file ID (FileIdInfo;
/// none when the call fails or the ID is all zeros), its ChangeTime (FileBasicInfo) and its size
/// (FileStandardInfo) and its change journal USN (FSCTL_READ_FILE_USN_DATA, #3313; 0 when the volume keeps
/// no journal or the read fails). nullopt when the change time or the size can't be read.
[[nodiscard]] std::optional<WindowsSignature::FileStamp> readFileStamp(const TrustApiFunctions& fns, HANDLE file);

/// @p path opened for its check: GENERIC_READ, shared for read only, so while the handle is held nobody
/// can write the file, rename it, or delete it and put another in its place; with its stamp read through
/// that handle. nullopt (nothing left open) when it can't be opened, a writer already has it open, or its
/// stamp can't be read. Close the handle with closeImage().
[[nodiscard]] std::optional<WindowsSignature::OpenedImage> openImage(const TrustApiFunctions& fns, const std::wstring& path);

/// Closes a handle openImage() gave.
void closeImage(void* file) noexcept;

/// WinVerifyTrust's answer for the own (embedded) signature of @p path, read through @p file (from its
/// start), with the signer's name when trusted. The verification state is always closed.
[[nodiscard]] WindowsSignature::TrustCheck verifyEmbeddedSignature(const TrustApiFunctions& fns, const std::wstring& path, HANDLE file);

/// WinVerifyTrust's answer for the system catalog that holds the hash of @p path, hashed through @p file:
/// its SHA-256 hash (current catalogs), else its SHA-1 hash (older ones). nullopt when no catalog holds
/// either; a failure's HRESULT when the file can't be hashed, or the catalogs can't be searched. Every
/// context is released.
[[nodiscard]] std::optional<WindowsSignature::TrustCheck>
verifyCatalogSignature(const TrustApiFunctions& fns, const std::wstring& path, HANDLE file);

/// FormatMessage's text for a Win32 error or HRESULT, else ntdll's for an NTSTATUS. Empty when neither has one.
[[nodiscard]] std::string signatureErrorMessage(std::uint32_t code);

/// The system's calls, as WindowsImageSignature.h's checkSignature() takes them (UTF-8 paths).
[[nodiscard]] WindowsSignature::SignatureFunctions systemSignatureFunctions() noexcept;

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
