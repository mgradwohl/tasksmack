#include "WindowsSignatureCheck.h"

// clang-format off
#include <softpub.h> // WINTRUST_ACTION_GENERIC_VERIFY_V2, DRIVER_ACTION_VERIFY
// clang-format on

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

#include "WinString.h"
#include "WindowsConversions.h"
#include "WindowsHandles.h"
#include "WindowsImageSignature.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

namespace
{

using WindowsSignature::TrustCheck;

/// WinVerifyTrust's settings for a quiet, offline check: no UI, no revocation, no URL fetched (cached only).
[[nodiscard]] WINTRUST_DATA offlineTrustData()
{
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwStateAction = WTD_STATEACTION_IGNORE;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    return data;
}

/// The leaf signing certificate's simple name ("Microsoft Windows") from a verified WinVerifyTrust state.
[[nodiscard]] std::string signerName(const TrustApiFunctions& fns, HANDLE state)
{
    CRYPT_PROVIDER_DATA* provider = fns.provDataFromStateData(state);
    if (provider == nullptr)
    {
        return {};
    }
    const CRYPT_PROVIDER_SGNR* signer = fns.provSignerFromChain(provider, 0, FALSE, 0);
    if (signer == nullptr || signer->csCertChain == 0 || signer->pasCertChain == nullptr || signer->pasCertChain->pCert == nullptr)
    {
        return {};
    }
    std::array<wchar_t, 256> name{};
    const DWORD length = fns.certGetNameString(
        signer->pasCertChain->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (length <= 1 || length > name.size())
    {
        return {}; // length counts the NUL: 1 is the empty name
    }
    return WinString::wideToUtf8(std::wstring_view(name.data(), length - 1));
}

/// WinVerifyTrust without an interactive user (INVALID_HANDLE_VALUE as the window: no UI at all), and the
/// signer's name when the signature is trusted. The verification state is always closed.
[[nodiscard]] TrustCheck verifyTrust(const TrustApiFunctions& fns, WINTRUST_DATA& data)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - WinVerifyTrust's documented "no interactive user" window
    auto* const noUser = static_cast<HWND>(INVALID_HANDLE_VALUE);
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    TrustCheck check{.result = fns.winVerifyTrust(noUser, &action, &data), .signer = {}};
    if (check.result == 0)
    {
        check.signer = signerName(fns, data.hWVTStateData);
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    (void) fns.winVerifyTrust(noUser, &action, &data);
    return check;
}

/// The HRESULT for the calling thread's last Win32 error.
[[nodiscard]] std::int32_t lastErrorResult()
{
    return HRESULT_FROM_WIN32(GetLastError());
}

/// A catalog admin context and the catalog found through it, released in that order.
class CatalogAdmin
{
  public:
    explicit CatalogAdmin(const TrustApiFunctions& fns) noexcept : m_Fns(&fns)
    {}
    CatalogAdmin(const CatalogAdmin&) = delete;
    CatalogAdmin& operator=(const CatalogAdmin&) = delete;
    CatalogAdmin(CatalogAdmin&&) = delete;
    CatalogAdmin& operator=(CatalogAdmin&&) = delete;
    ~CatalogAdmin()
    {
        if (m_Catalog != nullptr)
        {
            (void) m_Fns->releaseCatalogContext(m_Handle, m_Catalog, 0);
        }
        if (m_Handle != nullptr)
        {
            (void) m_Fns->releaseContext(m_Handle, 0);
        }
    }

    HCATADMIN* handleSlot() noexcept
    {
        return &m_Handle;
    }
    [[nodiscard]] HCATADMIN handle() const noexcept
    {
        return m_Handle;
    }
    void setCatalog(HCATINFO catalog) noexcept
    {
        m_Catalog = catalog;
    }
    [[nodiscard]] HCATINFO catalog() const noexcept
    {
        return m_Catalog;
    }

  private:
    const TrustApiFunctions* m_Fns;
    HCATADMIN m_Handle = nullptr;
    HCATINFO m_Catalog = nullptr;
};

/// @p file's hash under @p admin's algorithm, from its start; nullopt (with the last error set) when it
/// can't be computed.
[[nodiscard]] std::optional<std::vector<BYTE>> hashFile(const TrustApiFunctions& fns, HCATADMIN admin, HANDLE file)
{
    const LARGE_INTEGER start{};
    DWORD hashSize = 0;
    if (SetFilePointerEx(file, start, nullptr, FILE_BEGIN) == FALSE ||
        (fns.calcHashFromFileHandle2(admin, file, &hashSize, nullptr, 0) == FALSE && hashSize == 0))
    {
        return std::nullopt;
    }
    std::vector<BYTE> hash(hashSize);
    if (SetFilePointerEx(file, start, nullptr, FILE_BEGIN) == FALSE ||
        fns.calcHashFromFileHandle2(admin, file, &hashSize, hash.data(), 0) == FALSE)
    {
        return std::nullopt;
    }
    hash.resize(hashSize);
    return hash;
}

} // namespace

TrustCheck verifyEmbeddedSignature(const TrustApiFunctions& fns, const std::wstring& path)
{
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data = offlineTrustData();
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file; // NOLINT(cppcoreguidelines-pro-type-union-access) - the member dwUnionChoice selects
    return verifyTrust(fns, data);
}

std::optional<TrustCheck> verifyCatalogSignature(const TrustApiFunctions& fns, const std::wstring& path)
{
    const UniqueHandle file(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file)
    {
        return TrustCheck{.result = lastErrorResult(), .signer = {}};
    }
    const GUID subsystem = DRIVER_ACTION_VERIFY; // the system catalog database, where Windows' own catalogs are
    for (const wchar_t* algorithm : {BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA1_ALGORITHM})
    {
        CatalogAdmin catalogAdmin(fns);
        if (fns.acquireContext2(catalogAdmin.handleSlot(), &subsystem, algorithm, nullptr, 0) == FALSE)
        {
            return TrustCheck{.result = lastErrorResult(), .signer = {}};
        }
        std::optional<std::vector<BYTE>> hash = hashFile(fns, catalogAdmin.handle(), file.get());
        if (!hash.has_value())
        {
            return TrustCheck{.result = lastErrorResult(), .signer = {}};
        }
        const auto hashSize = static_cast<DWORD>(hash->size());
        catalogAdmin.setCatalog(fns.enumCatalogFromHash(catalogAdmin.handle(), hash->data(), hashSize, 0, nullptr));
        if (catalogAdmin.catalog() == nullptr)
        {
            continue; // not in a catalog under this algorithm's hash
        }
        CATALOG_INFO info{};
        info.cbStruct = sizeof(info);
        if (fns.catalogInfoFromContext(catalogAdmin.catalog(), &info, 0) == FALSE)
        {
            return TrustCheck{.result = lastErrorResult(), .signer = {}};
        }
        // The catalog names its members by their hash, in upper-case hex.
        const std::wstring memberTag = WinString::utf8ToWide(WindowsSignature::catalogMemberTag(std::span<const std::uint8_t>(*hash)));
        WINTRUST_CATALOG_INFO member{};
        member.cbStruct = sizeof(member);
        member.pcwszCatalogFilePath = info.wszCatalogFile;
        member.pcwszMemberTag = memberTag.c_str();
        member.pcwszMemberFilePath = path.c_str();
        member.hMemberFile = file.get();
        member.pbCalculatedFileHash = hash->data();
        member.cbCalculatedFileHash = hashSize;
        member.hCatAdmin = catalogAdmin.handle();
        WINTRUST_DATA data = offlineTrustData();
        data.dwUnionChoice = WTD_CHOICE_CATALOG;
        data.pCatalog = &member; // NOLINT(cppcoreguidelines-pro-type-union-access) - the member dwUnionChoice selects
        return verifyTrust(fns, data);
    }
    return std::nullopt;
}

std::optional<WindowsSignature::FileStamp> imageFileStamp(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(WinString::utf8ToWide(path).c_str(), GetFileExInfoStandard, &data) == FALSE)
    {
        return std::nullopt;
    }
    constexpr unsigned HIGH_SHIFT = 32;
    return WindowsSignature::FileStamp{
        .lastWriteTime = fileTimeTicks(data.ftLastWriteTime),
        .size = (std::uint64_t{data.nFileSizeHigh} << HIGH_SHIFT) | data.nFileSizeLow,
    };
}

std::string signatureErrorMessage(std::uint32_t code)
{
    const auto format = [code](DWORD source, HMODULE module)
    {
        std::array<wchar_t, 512> text{};
        const DWORD length =
            FormatMessageW(source | FORMAT_MESSAGE_IGNORE_INSERTS, module, code, 0, text.data(), static_cast<DWORD>(text.size()), nullptr);
        return length == 0 ? std::string{} : WinString::wideToUtf8(std::wstring_view(text.data(), length));
    };
    std::string text = format(FORMAT_MESSAGE_FROM_SYSTEM, nullptr);
    if (text.empty())
    {
        if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr)
        {
            text = format(FORMAT_MESSAGE_FROM_HMODULE, ntdll);
        }
    }
    return text;
}

WindowsSignature::SignatureFunctions systemSignatureFunctions() noexcept
{
    return {
        .errorMessage = &signatureErrorMessage,
        .fileStamp = &imageFileStamp,
        .verifyEmbeddedSignature = [](const std::string& path)
        { return verifyEmbeddedSignature(TrustApiFunctions{}, WinString::utf8ToWide(path)); },
        .verifyCatalogSignature = [](const std::string& path)
        { return verifyCatalogSignature(TrustApiFunctions{}, WinString::utf8ToWide(path)); },
    };
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
