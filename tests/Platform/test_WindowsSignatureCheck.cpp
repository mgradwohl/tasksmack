/// @file test_WindowsSignatureCheck.cpp
/// @brief Windows' image signature check (#1661, #3070): WinVerifyTrust's embedded and catalog paths through
/// fake wintrust and crypt32 calls (the offline settings asked for, the signer read only when trusted, the
/// state always closed, SHA-256 then SHA-1 catalogs, the member tag, every context released, each failure's
/// HRESULT, both read through the check's own handle); the file stamp through a fake
/// GetFileInformationByHandleEx and the file opened shared for read only (#3292); then the real calls on a
/// signed system binary and on an unsigned file this test writes.

#include "Platform/ImageSignature.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
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

#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
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
    // GetFileInformationByHandleEx's answers
    bool failBasicInfo = false;
    bool failStandardInfo = false;
    bool failIdInfo = false;
    bool zeroId = false;
    LONGLONG changeTime = 1234;
    LONGLONG endOfFile = 4096;
    // Seen
    HANDLE verifiedFile = nullptr; ///< WINTRUST_FILE_INFO::hFile, or WINTRUST_CATALOG_INFO::hMemberFile
    HANDLE hashedFile = nullptr;
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
        fake().verifiedFile = trust->pCatalog->hMemberFile; // NOLINT(cppcoreguidelines-pro-type-union-access)
        return fake().catalogResult;
    }
    fake().verifiedFile = trust->pFile->hFile; // NOLINT(cppcoreguidelines-pro-type-union-access)
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

BOOL WINAPI fakeCalcHash(HCATADMIN /*admin*/, HANDLE file, DWORD* size, BYTE* hash, DWORD /*flags*/)
{
    fake().hashedFile = file;
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

BOOL WINAPI fakeFileInfo(HANDLE /*file*/, FILE_INFO_BY_HANDLE_CLASS type, LPVOID out, DWORD size)
{
    switch (type)
    {
    case FileBasicInfo:
    {
        if (fake().failBasicInfo || size != sizeof(FILE_BASIC_INFO))
        {
            return FALSE;
        }
        auto* basic = static_cast<FILE_BASIC_INFO*>(out);
        basic->LastWriteTime.QuadPart = 1; // what SetFileTime sets: not in the stamp
        basic->ChangeTime.QuadPart = fake().changeTime;
        return TRUE;
    }
    case FileStandardInfo:
    {
        if (fake().failStandardInfo || size != sizeof(FILE_STANDARD_INFO))
        {
            return FALSE;
        }
        static_cast<FILE_STANDARD_INFO*>(out)->EndOfFile.QuadPart = fake().endOfFile;
        return TRUE;
    }
    case FileIdInfo:
    {
        if (fake().failIdInfo || size != sizeof(FILE_ID_INFO))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        auto* id = static_cast<FILE_ID_INFO*>(out);
        id->VolumeSerialNumber = 0xC0FFEE;
        for (std::size_t i = 0; i < std::size(id->FileId.Identifier); ++i)
        {
            id->FileId.Identifier[i] =
                fake().zeroId ? 0 : static_cast<BYTE>(i + 1); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        }
        return TRUE;
    }
    default:
        return FALSE;
    }
}

TrustApiFunctions fakeApi()
{
    return {
        .getFileInformationByHandleEx = &fakeFileInfo,
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
        m_Handle.reset(CreateFileW(m_File.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        ASSERT_TRUE(m_Handle.valid());
    }
    void TearDown() override
    {
        m_Handle.reset();
        std::error_code ignored;
        std::filesystem::remove(m_File, ignored);
    }

    [[nodiscard]] std::wstring file() const
    {
        return m_File.wstring();
    }

    /// Closes the fixture's own handle on the file, so the test can open it its way.
    void closeHandle()
    {
        m_Handle.reset();
    }

    /// The test's file, open for read.
    [[nodiscard]] HANDLE handle() const
    {
        return m_Handle.get();
    }

  private:
    std::filesystem::path m_File;
    UniqueHandle m_Handle;
};

// --- The file and its stamp (#3292) ----------------------------------------------------------------------

TEST_F(WindowsSignatureCheckTest, TheStampIsTheFileIdChangeTimeAndSizeReadThroughTheHandle)
{
    const std::optional<WindowsSignature::FileStamp> stamp = readFileStamp(fakeApi(), handle());
    ASSERT_TRUE(stamp.has_value());
    ASSERT_TRUE(stamp->fileId.has_value());
    EXPECT_EQ(stamp->fileId->volumeSerial, 0xC0FFEEU);
    EXPECT_EQ(stamp->fileId->id[0], 1U);
    EXPECT_EQ(stamp->fileId->id[WindowsSignature::FileId::SIZE - 1], WindowsSignature::FileId::SIZE);
    EXPECT_EQ(stamp->changeTime, 1234U) << "ChangeTime, not LastWriteTime (which SetFileTime sets)";
    EXPECT_EQ(stamp->size, 4096U);
}

TEST_F(WindowsSignatureCheckTest, AFileWithoutAnIdHasAStampWithoutOne)
{
    fake().failIdInfo = true; // FAT, or a share that doesn't say
    std::optional<WindowsSignature::FileStamp> stamp = readFileStamp(fakeApi(), handle());
    ASSERT_TRUE(stamp.has_value()) << "still checked, just never cached";
    EXPECT_FALSE(stamp->fileId.has_value());
    EXPECT_EQ(stamp->changeTime, 1234U);

    fake() = FakeTrust{};
    fake().zeroId = true;
    stamp = readFileStamp(fakeApi(), handle());
    ASSERT_TRUE(stamp.has_value());
    EXPECT_FALSE(stamp->fileId.has_value()) << "an ID of all zeros is none";
}

TEST_F(WindowsSignatureCheckTest, AStampWithoutItsChangeTimeOrSizeIsNone)
{
    fake().failBasicInfo = true;
    EXPECT_FALSE(readFileStamp(fakeApi(), handle()).has_value());
    fake() = FakeTrust{};
    fake().failStandardInfo = true;
    EXPECT_FALSE(readFileStamp(fakeApi(), handle()).has_value());
}

TEST_F(WindowsSignatureCheckTest, AnImageIsOpenedSharedForReadOnlyWhileItsHandleIsHeld)
{
    closeHandle();
    std::optional<WindowsSignature::OpenedImage> image = openImage(fakeApi(), file());
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->stamp.changeTime, 1234U);
    // Held: nobody can write it, rename it, or delete it and put another in its place.
    const UniqueHandle writer(CreateFileW(
        file().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    EXPECT_FALSE(writer.valid());
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    const std::wstring moved = file() + L".moved";
    EXPECT_EQ(MoveFileExW(file().c_str(), moved.c_str(), 0), FALSE) << "no rename over it while it's held";
    EXPECT_EQ(DeleteFileW(file().c_str()), FALSE);
    closeImage(image->file);

    const UniqueHandle afterwards(CreateFileW(file().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    EXPECT_TRUE(afterwards.valid()) << "closed: writable again";
}

TEST_F(WindowsSignatureCheckTest, AnImageThatCantBeOpenedOrStampedIsNone)
{
    closeHandle();
    EXPECT_FALSE(openImage(fakeApi(), L"C:\\TaskSmack-3070-no-such-dir\\none.exe").has_value());

    const UniqueHandle writer(
        CreateFileW(file().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(writer.valid());
    EXPECT_FALSE(openImage(fakeApi(), file()).has_value()) << "open for writing elsewhere: it could change under the check";
}

TEST_F(WindowsSignatureCheckTest, AnImageWhoseStampCantBeReadIsNone)
{
    closeHandle();
    fake().failBasicInfo = true;
    EXPECT_FALSE(openImage(fakeApi(), file()).has_value());
    const UniqueHandle writer(CreateFileW(file().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    EXPECT_TRUE(writer.valid()) << "nothing left open";
}

// --- The embedded path ---------------------------------------------------------------------------------

TEST_F(WindowsSignatureCheckTest, ATrustedEmbeddedSignatureNamesItsSignerOfflineAndClosesItsState)
{
    const TrustCheck check = verifyEmbeddedSignature(fakeApi(), file(), handle());
    EXPECT_EQ(check.result, 0);
    EXPECT_EQ(check.signer, "Fake Signer");
    EXPECT_EQ(fake().verifyCalls, 1);
    EXPECT_EQ(fake().closeCalls, 1);
    EXPECT_EQ(fake().revocationChecks, static_cast<DWORD>(WTD_REVOKE_NONE));
    EXPECT_NE(fake().provFlags & WTD_CACHE_ONLY_URL_RETRIEVAL, 0U) << "never goes to the network";
    EXPECT_NE(fake().provFlags & WTD_REVOCATION_CHECK_NONE, 0U);
    EXPECT_EQ(fake().uiChoice, static_cast<DWORD>(WTD_UI_NONE));
    EXPECT_EQ(fake().verifiedFile, handle()) << "read through the check's handle, not by opening the path again";
}

TEST_F(WindowsSignatureCheckTest, AnUntrustedSignatureReadsNoSignerButStillClosesItsState)
{
    fake().embeddedResult = TRUST_E_NOSIGNATURE_RESULT;
    const TrustCheck check = verifyEmbeddedSignature(fakeApi(), file(), handle());
    EXPECT_EQ(check.result, TRUST_E_NOSIGNATURE_RESULT);
    EXPECT_TRUE(check.signer.empty());
    EXPECT_EQ(fake().nameReads, 0);
    EXPECT_EQ(fake().closeCalls, 1);
}

TEST_F(WindowsSignatureCheckTest, ASignerThatCantBeReadIsLeftUnnamed)
{
    fake().noProviderData = true;
    EXPECT_TRUE(verifyEmbeddedSignature(fakeApi(), file(), handle()).signer.empty());
    fake().noProviderData = false;
    fake().signerName = std::wstring(300, L'n'); // longer than the name buffer: CertGetNameString fails
    EXPECT_TRUE(verifyEmbeddedSignature(fakeApi(), file(), handle()).signer.empty());
    fake().signerName.clear(); // a certificate naming no one: just the NUL
    EXPECT_TRUE(verifyEmbeddedSignature(fakeApi(), file(), handle()).signer.empty());
}

TEST_F(WindowsSignatureCheckTest, AHandleThatCantBeRewoundIsAnError)
{
    const TrustCheck check = verifyEmbeddedSignature(fakeApi(), file(), INVALID_HANDLE_VALUE);
    EXPECT_EQ(check.result, HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
    EXPECT_EQ(fake().verifyCalls, 0);
}

// --- The catalog path ----------------------------------------------------------------------------------

TEST_F(WindowsSignatureCheckTest, ACatalogHoldingTheSha256HashIsVerifiedWithItsHexTag)
{
    const std::optional<TrustCheck> check = verifyCatalogSignature(fakeApi(), file(), handle());
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->result, 0);
    EXPECT_EQ(check->signer, "Fake Signer");
    EXPECT_EQ(fake().algorithms, (std::vector<std::wstring>{BCRYPT_SHA256_ALGORITHM}));
    EXPECT_EQ(fake().memberTag, repeated(L"AB", 32)) << "the 32-byte hash, two upper-case hex digits a byte";
    EXPECT_EQ(fake().acquired, 1);
    EXPECT_EQ(fake().released, 1);
    EXPECT_EQ(fake().catalogsReleased, 1);
    EXPECT_EQ(fake().closeCalls, 1);
    EXPECT_EQ(fake().hashedFile, handle()) << "hashed through the check's handle";
    EXPECT_EQ(fake().verifiedFile, handle()) << "and verified through it";
}

TEST_F(WindowsSignatureCheckTest, AnOlderCatalogIsFoundByTheSha1HashAndEveryContextIsReleased)
{
    fake().sha256InCatalog = false;
    fake().catalogResult = static_cast<LONG>(0x800B0101U); // expired
    const std::optional<TrustCheck> check = verifyCatalogSignature(fakeApi(), file(), handle());
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
    EXPECT_FALSE(verifyCatalogSignature(fakeApi(), file(), handle()).has_value());
    EXPECT_EQ(fake().verifyCalls, 0);
    EXPECT_EQ(fake().released, 2);
    EXPECT_EQ(fake().catalogsReleased, 0);
}

TEST_F(WindowsSignatureCheckTest, EachCatalogFailureIsItsHresult)
{
    fake().failAcquire = true;
    std::optional<TrustCheck> check = verifyCatalogSignature(fakeApi(), file(), handle());
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->result, HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    EXPECT_EQ(fake().released, 0) << "nothing acquired, nothing released";

    fake() = FakeTrust{};
    fake().failHash = true;
    check = verifyCatalogSignature(fakeApi(), file(), handle());
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->result, HRESULT_FROM_WIN32(ERROR_READ_FAULT));
    EXPECT_EQ(fake().released, 1);

    fake() = FakeTrust{};
    fake().failCatalogInfo = true;
    check = verifyCatalogSignature(fakeApi(), file(), handle());
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->result, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    EXPECT_EQ(fake().catalogsReleased, 1);
    EXPECT_EQ(fake().released, 1);

    fake() = FakeTrust{};
    check = verifyCatalogSignature(fakeApi(), file(), INVALID_HANDLE_VALUE);
    ASSERT_TRUE(check.has_value()) << "a file that can't be read is an error, not 'in no catalog'";
    EXPECT_EQ(check->result, HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
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
    EXPECT_FALSE(fns.openImage(missing).has_value());
    EXPECT_EQ(WindowsSignature::checkSignature(fns, nullptr, missing, missing).signature, ImageSignature::Unknown);
    EXPECT_FALSE(fns.errorMessage(ERROR_ACCESS_DENIED).empty());
    EXPECT_FALSE(fns.errorMessage(0xC0000034U).empty()); // an NTSTATUS, from ntdll's messages
    EXPECT_TRUE(fns.errorMessage(0x2FFFFFFFU).empty());
}

// #3292: the real stamp of a file on the system volume (NTFS) has its ID; writing it in place, then setting
// its write time back with SetFileTime, keeps the ID and the size but moves its ChangeTime.
TEST(WindowsSignatureCheckRealTest, AFileWrittenInPlaceAndItsWriteTimeSetBackHasANewStamp)
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / std::format(L"tasksmack-3292-{}-stamp.exe", GetCurrentProcessId());
    std::ofstream(file, std::ios::binary) << "MZ one";
    const std::optional<WindowsSignature::OpenedImage> before = openImage(TrustApiFunctions{}, file.wstring());
    ASSERT_TRUE(before.has_value());
    closeImage(before->file);
    EXPECT_TRUE(before->stamp.fileId.has_value()) << "NTFS (the temp folder's volume) keeps file IDs";
    {
        const UniqueHandle writer(CreateFileW(file.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        ASSERT_TRUE(writer.valid());
        FILETIME created{};
        FILETIME accessed{};
        FILETIME written{};
        ASSERT_NE(GetFileTime(writer.get(), &created, &accessed, &written), FALSE);
        Sleep(20); // the clock moves on, so a new ChangeTime can't equal the old one
        DWORD wrote = 0;
        ASSERT_NE(WriteFile(writer.get(), "MZ two", 6, &wrote, nullptr), FALSE); // the same size
        ASSERT_NE(SetFileTime(writer.get(), &created, &accessed, &written), FALSE);
    }
    const std::optional<WindowsSignature::OpenedImage> after = openImage(TrustApiFunctions{}, file.wstring());
    ASSERT_TRUE(after.has_value());
    closeImage(after->file);
    EXPECT_EQ(after->stamp.fileId, before->stamp.fileId);
    EXPECT_EQ(after->stamp.size, before->stamp.size);
    EXPECT_NE(after->stamp.changeTime, before->stamp.changeTime);
    EXPECT_NE(after->stamp, before->stamp) << "the cache would check it again";
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}

} // namespace
} // namespace Platform::Windows
