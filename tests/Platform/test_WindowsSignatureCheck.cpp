/// @file test_WindowsSignatureCheck.cpp
/// @brief Windows' image signature check (#1661, #3070): WinVerifyTrust's embedded and catalog paths through
/// fake wintrust and crypt32 calls (the offline settings asked for, the signer read only when trusted, the
/// state always closed, SHA-256 then SHA-1 catalogs, the member tag, every context released, each failure's
/// HRESULT); then the real calls on a signed system binary and on an unsigned file this test writes.

#include "Platform/ImageSignature.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsImageSignature.h"
#include "Platform/Windows/WindowsSignatureCheck.h"

#include <gtest/gtest.h>

// clang-format off
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
// clang-format on

#include <cwchar>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{
namespace
{

using WindowsSignature::TrustCheck;

constexpr LONG TRUST_E_NOSIGNATURE_RESULT = static_cast<LONG>(0x800B0100U);

/// What the fakes answer, and what they were asked.
struct FakeTrust // NOLINT(clang-analyzer-optin.performance.Padding)
{
    LONG embeddedResult = 0;
    LONG catalogResult = 0;
    bool noProviderData = false;
    std::wstring signerName = L"Fake Signer";
    bool failAcquire = false;
    bool failHash = false;
    bool sha256InCatalog = true; ///< Else only the SHA-1 hash is in a catalog
    bool sha1InCatalog = true;
    bool failCatalogInfo = false;
    // Seen
    int verifyCalls = 0;
    int closeCalls = 0;
    int nameReads = 0;
    int acquired = 0;
    int released = 0;
    int catalogsReleased = 0;
    std::vector<std::wstring> algorithms;
    std::wstring memberTag;
    DWORD revocationChecks = 0xFFFFFFFF;
    DWORD provFlags = 0;
    DWORD uiChoice = 0;
};

FakeTrust& fake()
{
    static FakeTrust state;
    return state;
}

// A non-null stand-in for every handle and certificate the fakes hand out.
CERT_CONTEXT g_cert{};
CRYPT_PROVIDER_CERT g_providerCert{};
CRYPT_PROVIDER_SGNR g_signer{};
CRYPT_PROVIDER_DATA g_providerData{};
int g_adminToken = 0;
int g_catalogToken = 0;

LONG WINAPI fakeWinVerifyTrust(HWND /*window*/, GUID* /*action*/, LPVOID data)
{
    auto* trust = static_cast<WINTRUST_DATA*>(data);
    if (trust->dwStateAction == WTD_STATEACTION_CLOSE)
    {
        ++fake().closeCalls;
        return 0;
    }
    ++fake().verifyCalls;
    fake().revocationChecks = trust->fdwRevocationChecks;
    fake().provFlags = trust->dwProvFlags;
    fake().uiChoice = trust->dwUIChoice;
    if (trust->dwUnionChoice == WTD_CHOICE_CATALOG)
    {
        fake().memberTag = trust->pCatalog->pcwszMemberTag; // NOLINT(cppcoreguidelines-pro-type-union-access)
        return fake().catalogResult;
    }
    return fake().embeddedResult;
}

CRYPT_PROVIDER_DATA* WINAPI fakeProvData(HANDLE /*state*/)
{
    return fake().noProviderData ? nullptr : &g_providerData;
}

CRYPT_PROVIDER_SGNR* WINAPI fakeProvSigner(CRYPT_PROVIDER_DATA* /*data*/, DWORD /*index*/, BOOL /*counter*/, DWORD /*counterIndex*/)
{
    g_providerCert.pCert = &g_cert;
    g_signer.csCertChain = 1;
    g_signer.pasCertChain = &g_providerCert;
    return &g_signer;
}

DWORD WINAPI fakeCertName(PCCERT_CONTEXT /*cert*/, DWORD /*type*/, DWORD /*flags*/, void* /*para*/, LPWSTR name, DWORD size)
{
    ++fake().nameReads;
    const std::wstring& signer = fake().signerName;
    if (size <= signer.size())
    {
        return 0;
    }
    wcsncpy_s(name, size, signer.c_str(), signer.size());
    return static_cast<DWORD>(signer.size() + 1);
}

BOOL WINAPI fakeAcquire(HCATADMIN* admin, const GUID* /*subsystem*/, PCWSTR algorithm, PCCERT_STRONG_SIGN_PARA /*policy*/, DWORD /*flags*/)
{
    fake().algorithms.emplace_back(algorithm);
    if (fake().failAcquire)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    ++fake().acquired;
    *admin = &g_adminToken;
    return TRUE;
}

BOOL WINAPI fakeCalcHash(HCATADMIN /*admin*/, HANDLE /*file*/, DWORD* size, BYTE* hash, DWORD /*flags*/)
{
    if (fake().failHash)
    {
        SetLastError(ERROR_READ_FAULT);
        return FALSE;
    }
    const bool sha256 = fake().algorithms.back() == BCRYPT_SHA256_ALGORITHM;
    const DWORD needed = sha256 ? 32 : 20;
    if (hash == nullptr)
    {
        *size = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    for (DWORD i = 0; i < needed; ++i)
    {
        hash[i] = static_cast<BYTE>(sha256 ? 0xAB : 0x01); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    *size = needed;
    return TRUE;
}

HCATINFO WINAPI fakeEnumCatalog(HCATADMIN /*admin*/, BYTE* /*hash*/, DWORD size, DWORD /*flags*/, HCATINFO* /*previous*/)
{
    const bool inCatalog = size == 32 ? fake().sha256InCatalog : fake().sha1InCatalog;
    return inCatalog ? &g_catalogToken : nullptr;
}

BOOL WINAPI fakeCatalogInfo(HCATINFO /*catalog*/, CATALOG_INFO* info, DWORD /*flags*/)
{
    if (fake().failCatalogInfo)
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    wcscpy_s(info->wszCatalogFile, L"C:\\fake\\fake.cat");
    return TRUE;
}

BOOL WINAPI fakeReleaseCatalog(HCATADMIN /*admin*/, HCATINFO /*catalog*/, DWORD /*flags*/)
{
    ++fake().catalogsReleased;
    return TRUE;
}

BOOL WINAPI fakeReleaseContext(HCATADMIN /*admin*/, DWORD /*flags*/)
{
    ++fake().released;
    return TRUE;
}

TrustApiFunctions fakeApi()
{
    return {
        .winVerifyTrust = &fakeWinVerifyTrust,
        .provDataFromStateData = &fakeProvData,
        .provSignerFromChain = &fakeProvSigner,
        .certGetNameString = &fakeCertName,
        .acquireContext2 = &fakeAcquire,
        .calcHashFromFileHandle2 = &fakeCalcHash,
        .enumCatalogFromHash = &fakeEnumCatalog,
        .catalogInfoFromContext = &fakeCatalogInfo,
        .releaseCatalogContext = &fakeReleaseCatalog,
        .releaseContext = &fakeReleaseContext,
    };
}

/// @p unit @p count times over.
std::wstring repeated(std::wstring_view unit, int count)
{
    std::wstring text;
    for (int i = 0; i < count; ++i)
    {
        text += unit;
    }
    return text;
}

class WindowsSignatureCheckTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeTrust{};
        m_File = std::filesystem::temp_directory_path() / std::format(L"tasksmack-3070-{}-image.exe", GetCurrentProcessId());
        std::ofstream(m_File, std::ios::binary) << "not really a program";
    }
    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove(m_File, ignored);
    }

    [[nodiscard]] std::wstring file() const
    {
        return m_File.wstring();
    }

  private:
    std::filesystem::path m_File;
};

// --- The embedded path ---------------------------------------------------------------------------------

TEST_F(WindowsSignatureCheckTest, ATrustedEmbeddedSignatureNamesItsSignerOfflineAndClosesItsState)
{
    const TrustCheck check = verifyEmbeddedSignature(fakeApi(), L"C:\\fake\\app.exe");
    EXPECT_EQ(check.result, 0);
    EXPECT_EQ(check.signer, "Fake Signer");
    EXPECT_EQ(fake().verifyCalls, 1);
    EXPECT_EQ(fake().closeCalls, 1);
    EXPECT_EQ(fake().revocationChecks, static_cast<DWORD>(WTD_REVOKE_NONE));
    EXPECT_NE(fake().provFlags & WTD_CACHE_ONLY_URL_RETRIEVAL, 0U) << "never goes to the network";
    EXPECT_NE(fake().provFlags & WTD_REVOCATION_CHECK_NONE, 0U);
    EXPECT_EQ(fake().uiChoice, static_cast<DWORD>(WTD_UI_NONE));
}

TEST_F(WindowsSignatureCheckTest, AnUntrustedSignatureReadsNoSignerButStillClosesItsState)
{
    fake().embeddedResult = TRUST_E_NOSIGNATURE_RESULT;
    const TrustCheck check = verifyEmbeddedSignature(fakeApi(), L"C:\\fake\\app.exe");
    EXPECT_EQ(check.result, TRUST_E_NOSIGNATURE_RESULT);
    EXPECT_TRUE(check.signer.empty());
    EXPECT_EQ(fake().nameReads, 0);
    EXPECT_EQ(fake().closeCalls, 1);
}

TEST_F(WindowsSignatureCheckTest, ASignerThatCantBeReadIsLeftUnnamed)
{
    fake().noProviderData = true;
    EXPECT_TRUE(verifyEmbeddedSignature(fakeApi(), L"x").signer.empty());
    fake().noProviderData = false;
    fake().signerName = std::wstring(300, L'n'); // longer than the name buffer: CertGetNameString fails
    EXPECT_TRUE(verifyEmbeddedSignature(fakeApi(), L"x").signer.empty());
    fake().signerName.clear(); // a certificate naming no one: just the NUL
    EXPECT_TRUE(verifyEmbeddedSignature(fakeApi(), L"x").signer.empty());
}

// --- The catalog path ----------------------------------------------------------------------------------

TEST_F(WindowsSignatureCheckTest, ACatalogHoldingTheSha256HashIsVerifiedWithItsHexTag)
{
    const std::optional<TrustCheck> check = verifyCatalogSignature(fakeApi(), file());
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->result, 0);
    EXPECT_EQ(check->signer, "Fake Signer");
    EXPECT_EQ(fake().algorithms, (std::vector<std::wstring>{BCRYPT_SHA256_ALGORITHM}));
    EXPECT_EQ(fake().memberTag, repeated(L"AB", 32)) << "the 32-byte hash, two upper-case hex digits a byte";
    EXPECT_EQ(fake().acquired, 1);
    EXPECT_EQ(fake().released, 1);
    EXPECT_EQ(fake().catalogsReleased, 1);
    EXPECT_EQ(fake().closeCalls, 1);
}

TEST_F(WindowsSignatureCheckTest, AnOlderCatalogIsFoundByTheSha1HashAndEveryContextIsReleased)
{
    fake().sha256InCatalog = false;
    fake().catalogResult = static_cast<LONG>(0x800B0101U); // expired
    const std::optional<TrustCheck> check = verifyCatalogSignature(fakeApi(), file());
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->result, static_cast<LONG>(0x800B0101U));
    EXPECT_TRUE(check->signer.empty());
    EXPECT_EQ(fake().algorithms, (std::vector<std::wstring>{BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA1_ALGORITHM}));
    EXPECT_EQ(fake().memberTag, repeated(L"01", 20)) << "the 20-byte SHA-1 hash";
    EXPECT_EQ(fake().acquired, 2);
    EXPECT_EQ(fake().released, 2);
    EXPECT_EQ(fake().catalogsReleased, 1);
}

TEST_F(WindowsSignatureCheckTest, AFileInNoCatalogIsNullopt)
{
    fake().sha256InCatalog = false;
    fake().sha1InCatalog = false;
    EXPECT_FALSE(verifyCatalogSignature(fakeApi(), file()).has_value());
    EXPECT_EQ(fake().verifyCalls, 0);
    EXPECT_EQ(fake().released, 2);
    EXPECT_EQ(fake().catalogsReleased, 0);
}

TEST_F(WindowsSignatureCheckTest, EachCatalogFailureIsItsHresult)
{
    fake().failAcquire = true;
    std::optional<TrustCheck> check = verifyCatalogSignature(fakeApi(), file());
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->result, HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    EXPECT_EQ(fake().released, 0) << "nothing acquired, nothing released";

    fake() = FakeTrust{};
    fake().failHash = true;
    check = verifyCatalogSignature(fakeApi(), file());
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->result, HRESULT_FROM_WIN32(ERROR_READ_FAULT));
    EXPECT_EQ(fake().released, 1);

    fake() = FakeTrust{};
    fake().failCatalogInfo = true;
    check = verifyCatalogSignature(fakeApi(), file());
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->result, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    EXPECT_EQ(fake().catalogsReleased, 1);
    EXPECT_EQ(fake().released, 1);

    fake() = FakeTrust{};
    check = verifyCatalogSignature(fakeApi(), L"C:\\TaskSmack-3070-no-such-dir\\none.exe");
    ASSERT_TRUE(check.has_value()) << "a file that can't be opened is an error, not 'in no catalog'";
    EXPECT_EQ(check->result, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
    EXPECT_TRUE(fake().algorithms.empty());
}

// --- The real calls --------------------------------------------------------------------------------------

std::string systemBinary(const wchar_t* name)
{
    std::wstring dir(MAX_PATH, L'\0');
    const UINT length = GetSystemDirectoryW(dir.data(), static_cast<UINT>(dir.size()));
    dir.resize(length);
    return WinString::wideToUtf8(dir + L"\\" + name);
}

TEST(WindowsSignatureCheckRealTest, NotepadIsSignedByMicrosoftAndAFileThisTestWritesIsUnsigned)
{
    const WindowsSignature::SignatureFunctions fns = systemSignatureFunctions();
    const std::string notepad = systemBinary(L"notepad.exe");
    const SignatureCheck system = WindowsSignature::checkSignature(fns, nullptr, notepad, notepad);
    EXPECT_TRUE(isVerifiedSignature(system.signature)) << notepad << ": " << system.note;
    EXPECT_NE(system.signer.find("Microsoft"), std::string::npos) << system.signer;

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / std::format(L"tasksmack-3070-{}-unsigned.exe", GetCurrentProcessId());
    std::ofstream(file, std::ios::binary) << "MZ but not really a program";
    const std::string path = WinString::wideToUtf8(file.wstring());
    const SignatureCheck unsignedFile = WindowsSignature::checkSignature(fns, nullptr, path, path);
    EXPECT_EQ(unsignedFile.signature, ImageSignature::Unsigned) << unsignedFile.note;
    EXPECT_TRUE(unsignedFile.signer.empty());
    std::error_code ignored;
    std::filesystem::remove(file, ignored);

    const std::string missing = "C:\\TaskSmack-3070-no-such-dir\\none.exe";
    EXPECT_FALSE(fns.fileStamp(missing).has_value());
    EXPECT_EQ(WindowsSignature::checkSignature(fns, nullptr, missing, missing).signature, ImageSignature::Unknown);
    EXPECT_FALSE(fns.errorMessage(ERROR_ACCESS_DENIED).empty());
    EXPECT_FALSE(fns.errorMessage(0xC0000034U).empty()); // an NTSTATUS, from ntdll's messages
    EXPECT_TRUE(fns.errorMessage(0x2FFFFFFFU).empty());
}

} // namespace
} // namespace Platform::Windows
