#pragma once

// Windows' image signature check (#1661, #3070): WinVerifyTrust on a file's own Authenticode signature,
// and on the signed catalog that holds its hash (CryptCATAdminAcquireContext2,
// CryptCATAdminCalcHashFromFileHandle2, CryptCATAdminEnumCatalogFromHash) -- most of Windows' own files
// are catalog-signed. Offline: WTD_REVOKE_NONE and WTD_CACHE_ONLY_URL_RETRIEVAL, so a check never goes to
// the network, and no UI. Milliseconds a file, more for a large one (the whole file is hashed), so it
// runs on a worker, never the UI thread or a sample: the Drivers section's read and the process probe's
// signature worker (WindowsSignatureWorker.h).
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

#include "WindowsImageSignature.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The calls verifyEmbeddedSignature() and verifyCatalogSignature() make: wintrust's and crypt32's, or a
/// test's fakes. The file itself is opened with CreateFileW either way.
struct TrustApiFunctions
{
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
};

/// WinVerifyTrust's answer for @p path's own (embedded) signature, with the signer's name when trusted.
/// The verification state is always closed.
[[nodiscard]] WindowsSignature::TrustCheck verifyEmbeddedSignature(const TrustApiFunctions& fns, const std::wstring& path);

/// WinVerifyTrust's answer for the system catalog that holds @p path's hash: its SHA-256 hash (current
/// catalogs), else its SHA-1 hash (older ones). nullopt when no catalog holds either; a failure's HRESULT
/// when the file can't be opened or hashed, or the catalogs can't be searched. Every context is released.
[[nodiscard]] std::optional<WindowsSignature::TrustCheck> verifyCatalogSignature(const TrustApiFunctions& fns, const std::wstring& path);

/// The file's last write time and size (GetFileAttributesExW); nullopt when it can't be read.
[[nodiscard]] std::optional<WindowsSignature::FileStamp> imageFileStamp(const std::string& path);

/// FormatMessage's text for a Win32 error or HRESULT, else ntdll's for an NTSTATUS. Empty when neither has one.
[[nodiscard]] std::string signatureErrorMessage(std::uint32_t code);

/// The system's calls, as WindowsImageSignature.h's checkSignature() takes them (UTF-8 paths).
[[nodiscard]] WindowsSignature::SignatureFunctions systemSignatureFunctions() noexcept;

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
