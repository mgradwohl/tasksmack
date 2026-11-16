/// @file test_WindowsSignatureCheck.cpp
/// @brief Windows' image signature check (#1661, #3070): WinVerifyTrust's embedded and catalog paths through
/// fake wintrust and crypt32 calls (the offline settings asked for, the signer read only when trusted, the
/// state always closed, even when a step reading the signer throws (#3339, #3342), SHA-256 then SHA-1 catalogs, the member tag, every
/// context released, each failure's HRESULT, both read through the check's own handle); the file stamp through a fake
/// GetFileInformationByHandleEx and DeviceIoControl (its USN, #3313) and the file opened shared for read
/// only (#3292); then the real calls on a
/// signed system binary and on an unsigned file this test writes; this process's image bound to its own file, and on a
/// CI runner a renamed image checked as itself, not the signed decoy left at its old name (#4021), and hashed for Look
/// up on VirusTotal as itself or not at all (#4057); the publisher read through a held file's handle (#4057).

#include "Platform/ImageSignature.h"
#include "Platform/Sha256.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsImageFile.h"
#include "Platform/Windows/WindowsImageSignature.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessProbe.h"
#include "Platform/Windows/WindowsSignatureCheck.h"
#include "Support/CiRealSystem.h"
#include "Support/PeImageBuilder.h"

#include <gtest/gtest.h>

// clang-format off
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <mscat.h>
#include <winioctl.h>
// clang-format on

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Platform::Windows
{
namespace
{

using WindowsSignature::TrustCheck;

constexpr LONG TRUST_E_NOSIGNATURE_RESULT = static_cast<LONG>(0x800B0100U);

/// Where reading a trusted signer's name, between VERIFY and CLOSE, throws (#3339, #3342): each step's
/// stand-in throws std::bad_alloc, as an allocation there might.
enum class SignerThrow : std::uint8_t
{
    None,
    ProviderData, ///< WTHelperProvDataFromStateData
    Signer,       ///< WTHelperGetProvSignerFromChain
    Name,         ///< CertGetNameStringW
    Conversion,   ///< The name's UTF-8 conversion, after CertGetNameStringW wrote it: #3339's own failure
};

/// What the fakes answer, and what they were asked.
struct FakeTrust // NOLINT(clang-analyzer-optin.performance.Padding)
{
    LONG embeddedResult = 0;
    LONG catalogResult = 0;
    bool noProviderData = false;
    SignerThrow throwAt = SignerThrow::None;
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
    // FSCTL_READ_FILE_USN_DATA's answer (#3313)
    bool noJournal = false;    ///< ERROR_JOURNAL_NOT_ACTIVE, as a volume with no change journal answers
    WORD usnRecordVersion = 2; ///< 2 (NTFS) or 3 (ReFS); any other is a record the stamp doesn't read
    USN usn = 77777;
    DWORD usnRequestMin = 0; ///< What READ_FILE_USN_DATA asked for
    DWORD usnRequestMax = 0;
    HANDLE usnFile = nullptr;
    // Seen
    HANDLE verifiedFile = nullptr; ///< WINTRUST_FILE_INFO::hFile, or WINTRUST_CATALOG_INFO::hMemberFile
    HANDLE hashedFile = nullptr;
    int verifyCalls = 0;
    int closeCalls = 0;
    int openStates = 0; ///< States VERIFY handed out that no CLOSE has been given back
    int nameReads = 0;
    int conversions = 0;
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

/// The state VERIFY hands out, which CLOSE must be given back.
HANDLE stateToken()
{
    static int token = 0;
    return &token;
}

LONG WINAPI fakeWinVerifyTrust(HWND /*window*/, GUID* /*action*/, LPVOID data)
{
    auto* trust = static_cast<WINTRUST_DATA*>(data);
    if (trust->dwStateAction == WTD_STATEACTION_CLOSE)
    {
        ++fake().closeCalls;
        if (trust->hWVTStateData == stateToken())
        {
            --fake().openStates;
            trust->hWVTStateData = nullptr;
        }
        return 0;
    }
    ++fake().verifyCalls;
    trust->hWVTStateData = stateToken(); // as WinVerifyTrust does, trusted or not
    ++fake().openStates;
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
    if (fake().throwAt == SignerThrow::ProviderData)
    {
        throw std::bad_alloc{};
    }
    return fake().noProviderData ? nullptr : &g_providerData;
}

CRYPT_PROVIDER_SGNR* WINAPI fakeProvSigner(CRYPT_PROVIDER_DATA* /*data*/, DWORD /*index*/, BOOL /*counter*/, DWORD /*counterIndex*/)
{
    if (fake().throwAt == SignerThrow::Signer)
    {
        throw std::bad_alloc{};
    }
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
    if (fake().throwAt == SignerThrow::Name)
    {
        throw std::bad_alloc{};
    }
    return static_cast<DWORD>(signer.size() + 1);
}

std::string fakeWideToUtf8(std::wstring_view wide)
{
    ++fake().conversions;
    if (fake().throwAt == SignerThrow::Conversion)
    {
        throw std::bad_alloc{};
    }
    return WinString::wideToUtf8(wide);
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

/// Copies @p record to @p out when it fits.
template<typename Record> BOOL writeUsnRecord(const Record& record, LPVOID out, DWORD outSize, LPDWORD returned)
{
    if (outSize < sizeof(record))
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(out, &record, sizeof(record));
    *returned = sizeof(record);
    return TRUE;
}

BOOL WINAPI fakeDeviceIoControl(
    HANDLE file, DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize, LPDWORD returned, LPOVERLAPPED /*overlapped*/)
{
    if (code != FSCTL_READ_FILE_USN_DATA || inSize != sizeof(READ_FILE_USN_DATA))
    {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    fake().usnFile = file;
    const auto* request = static_cast<const READ_FILE_USN_DATA*>(in);
    fake().usnRequestMin = request->MinMajorVersion;
    fake().usnRequestMax = request->MaxMajorVersion;
    if (fake().noJournal)
    {
        SetLastError(ERROR_JOURNAL_NOT_ACTIVE);
        return FALSE;
    }
    if (fake().usnRecordVersion == 3)
    {
        USN_RECORD_V3 record{};
        record.RecordLength = sizeof(record);
        record.MajorVersion = 3;
        record.Usn = fake().usn;
        return writeUsnRecord(record, out, outSize, returned);
    }
    USN_RECORD_V2 record{};
    record.RecordLength = sizeof(record);
    record.MajorVersion = fake().usnRecordVersion;
    record.Usn = fake().usn;
    return writeUsnRecord(record, out, outSize, returned);
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
        .deviceIoControl = &fakeDeviceIoControl,
        .wideToUtf8 = &fakeWideToUtf8,
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
    EXPECT_EQ(stamp->usn, 77777U) << "the change journal's USN (#3313)";
    EXPECT_EQ(fake().usnFile, handle()) << "read through the check's own handle, not another open";
    EXPECT_EQ(fake().usnRequestMin, 2U);
    EXPECT_EQ(fake().usnRequestMax, 3U) << "ReFS gives version 3 records";
}

TEST_F(WindowsSignatureCheckTest, TheUsnIsReadFromAVersion3Record)
{
    fake().usnRecordVersion = 3;
    fake().usn = 0x123456789A;
    const std::optional<WindowsSignature::FileStamp> stamp = readFileStamp(fakeApi(), handle());
    ASSERT_TRUE(stamp.has_value());
    EXPECT_EQ(stamp->usn, 0x123456789AU);
}

// #3313: a volume without a change journal (FAT, most network shares, a journal turned off), or a record
// the stamp can't read, gives USN 0: the stamp is the rest, as before, never a failed check.
TEST_F(WindowsSignatureCheckTest, AFileWithoutAUsnKeepsTheRestOfItsStamp)
{
    fake().noJournal = true;
    std::optional<WindowsSignature::FileStamp> stamp = readFileStamp(fakeApi(), handle());
    ASSERT_TRUE(stamp.has_value()) << "no journal is no reason to fail the check";
    EXPECT_EQ(stamp->usn, 0U);
    EXPECT_TRUE(stamp->fileId.has_value());
    EXPECT_EQ(stamp->changeTime, 1234U);
    EXPECT_EQ(stamp->size, 4096U);

    fake() = FakeTrust{};
    fake().usnRecordVersion = 4; // not a record FSCTL_READ_FILE_USN_DATA was asked for
    stamp = readFileStamp(fakeApi(), handle());
    ASSERT_TRUE(stamp.has_value());
    EXPECT_EQ(stamp->usn, 0U);

    fake() = FakeTrust{};
    fake().usn = -1; // never a USN: treated as none
    stamp = readFileStamp(fakeApi(), handle());
    ASSERT_TRUE(stamp.has_value());
    EXPECT_EQ(stamp->usn, 0U);

    closeHandle();
    fake() = FakeTrust{};
    fake().noJournal = true;
    const std::optional<WindowsSignature::OpenedImage> image = openImage(fakeApi(), file());
    ASSERT_TRUE(image.has_value()) << "opened and checked as before";
    closeImage(image->file);
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
    EXPECT_EQ(fake().openStates, 0) << "CLOSE was given the state VERIFY opened";
    EXPECT_EQ(fake().conversions, 1) << "the name is converted through the seam";
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

TEST_F(WindowsSignatureCheckTest, AThrowReadingTheSignerStillClosesTheState)
{
    // Between VERIFY and CLOSE the signer's name is read, which allocates: a throw at any step of that
    // must not skip CLOSE (#3339, #3342), on the embedded path and on the catalog one. CLOSE is given back
    // the state VERIFY opened, so none is left open.
    constexpr std::array points{SignerThrow::ProviderData, SignerThrow::Signer, SignerThrow::Name, SignerThrow::Conversion};
    for (const SignerThrow point : points)
    {
        SCOPED_TRACE(::testing::Message() << "throw at step " << static_cast<int>(point));
        fake() = FakeTrust{};
        fake().throwAt = point;
        EXPECT_THROW((void) verifyEmbeddedSignature(fakeApi(), file(), handle()), std::bad_alloc);
        EXPECT_EQ(fake().verifyCalls, 1);
        EXPECT_EQ(fake().closeCalls, 1);
        EXPECT_EQ(fake().openStates, 0);
        EXPECT_THROW((void) verifyCatalogSignature(fakeApi(), file(), handle()), std::bad_alloc);
        EXPECT_EQ(fake().verifyCalls, 2);
        EXPECT_EQ(fake().closeCalls, 2);
        EXPECT_EQ(fake().openStates, 0);
        EXPECT_EQ(fake().catalogsReleased, 1) << "the catalog context is released on the way out too";
        EXPECT_EQ(fake().released, fake().acquired);
    }
}

TEST_F(WindowsSignatureCheckTest, AThrowConvertingTheSignersNameAfterItWasReadStillClosesTheState)
{
    // #3339's own failure: CertGetNameStringW has written the name, and converting it to UTF-8 throws.
    fake().throwAt = SignerThrow::Conversion;
    EXPECT_THROW((void) verifyEmbeddedSignature(fakeApi(), file(), handle()), std::bad_alloc);
    EXPECT_EQ(fake().nameReads, 1) << "the throw came after the name was read";
    EXPECT_EQ(fake().conversions, 1);
    EXPECT_EQ(fake().closeCalls, 1);
    EXPECT_EQ(fake().openStates, 0);

    // The next check, with nothing thrown, names its signer and closes as usual.
    fake().throwAt = SignerThrow::None;
    const TrustCheck check = verifyEmbeddedSignature(fakeApi(), file(), handle());
    EXPECT_EQ(check.signer, "Fake Signer");
    EXPECT_EQ(fake().closeCalls, 2);
    EXPECT_EQ(fake().openStates, 0);
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

/// This test process's identity, as the probe has it: its PID and creation time.
WindowsSignature::ImageProcess thisProcess()
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    EXPECT_NE(GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user), 0);
    return {
        .pid = static_cast<std::int32_t>(GetCurrentProcessId()),
        .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
    };
}

/// The identity of the file at @p path, or nullopt when it can't be opened.
std::optional<FileIdentity> identityOf(const std::wstring& path)
{
    const UniqueHandle file(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    return file ? fileIdentity(file.get()) : std::nullopt;
}

// #4021, the real calls on this test process: its image binds to the file it was mapped from, held so it can't be
// renamed or deleted, and checks as that file does by path. A process identity that isn't this one's is refused.
TEST(WindowsSignatureCheckRealTest, ThisProcesssImageBindsToItsOwnFileAndChecksAsItDoes)
{
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    ASSERT_GT(length, 0U);
    module.resize(length);

    std::expected<WindowsSignature::BoundImage, WindowsSignature::BindFailure> bound = openProcessImageForCheck(thisProcess());
    ASSERT_TRUE(bound.has_value()) << bound.error().why;
    const std::optional<FileIdentity> held = fileIdentity(bound->image.file);
    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(held, identityOf(module)) << "the module's own file";
    EXPECT_EQ(identityOf(WinString::utf8ToWide(bound->path)), held) << bound->path << ": its name now names it";
    EXPECT_TRUE(bound->image.stamp.fileId.has_value());

    // Held without FILE_SHARE_DELETE: the file can't be renamed while the check holds it.
    const UniqueHandle forRename(
        CreateFileW(module.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    EXPECT_FALSE(forRename) << "a handle that could rename it was granted";
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    closeImage(bound->image.file);

    WindowsSignature::SignatureFunctions fns = systemSignatureFunctions();
    fns.openProcessImage = &openProcessImageForCheck;
    const std::string path = WinString::wideToUtf8(module);
    const WindowsSignature::ImageCheck viaBinding = WindowsSignature::checkImage(fns, nullptr, path, path, thisProcess());
    const SignatureCheck viaPath = WindowsSignature::checkSignature(fns, nullptr, path, path);
    EXPECT_EQ(viaBinding.check.signature, viaPath.signature) << viaBinding.check.note;
    EXPECT_EQ(viaBinding.check.signer, viaPath.signer);

    WindowsSignature::ImageProcess reused = thisProcess();
    reused.startTimeTicks += 1;
    const std::expected<WindowsSignature::BoundImage, WindowsSignature::BindFailure> refused = openProcessImageForCheck(reused);
    EXPECT_FALSE(refused.has_value()) << "another process's identity binds nothing";
}

// #4059, the real exit check on this test process: running, it hasn't exited; an identity that isn't this one's (its
// PID reused since) has, so the signature worker drops it unbound.
TEST(WindowsSignatureCheckRealTest, TheExitCheckSaysThisProcessIsRunningAndAReusedIdentityHasExited)
{
    EXPECT_FALSE(processExitedForCheck(thisProcess()));
    WindowsSignature::ImageProcess reused = thisProcess();
    reused.startTimeTicks += 1;
    EXPECT_TRUE(processExitedForCheck(reused));
}

/// @p path opened as the signature check opens an image: for read, shared for read only.
UniqueHandle openForRead(const std::wstring& path)
{
    return UniqueHandle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

/// A file in the temp folder, this process's and this test's own, holding @p bytes; removed with the object.
class TempFile
{
  public:
    TempFile(std::wstring_view suffix, const std::vector<std::byte>& bytes)
    {
        const auto* const info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_Path = std::filesystem::temp_directory_path() /
                 std::format("tasksmack-4057-{}-{}{}", GetCurrentProcessId(), info->name(), WinString::wideToUtf8(std::wstring(suffix)));
        std::ofstream out(m_Path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
    ~TempFile()
    {
        std::error_code ignored;
        std::filesystem::remove(m_Path, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

// #4057: the publisher is read out of the held file's own bytes. A file this test builds, with a version resource
// naming a company, reads as that company through its handle; one that isn't a PE file, is empty, or is cut short
// reads as none, and nothing is read by its name.
TEST(WindowsHeldFilePublisherTest, AFilesPublisherIsReadThroughItsHandle)
{
    const TestSupport::PeImage image = TestSupport::buildPeImage(u"TaskSmack Test Co");
    const TempFile pe(L"-pe.exe", image.bytes);
    const UniqueHandle file = openForRead(pe.path().wstring());
    ASSERT_TRUE(file) << GetLastError();
    EXPECT_EQ(heldFilePublisher(file.get()), "TaskSmack Test Co");

    // A PE32 file's, and the neutral language's ahead of another.
    const TestSupport::PeImage pe32Image = TestSupport::buildPeImage(
        {{0x0407U, TestSupport::versionInfoBlock(u"Deutsch")}, {0x0000U, TestSupport::versionInfoBlock(u"Neutral GmbH")}}, true);
    const TempFile pe32(L"-pe32.exe", pe32Image.bytes);
    const UniqueHandle pe32File = openForRead(pe32.path().wstring());
    ASSERT_TRUE(pe32File);
    EXPECT_EQ(heldFilePublisher(pe32File.get()), "Neutral GmbH");

    const std::string text = "MZ but not really a program";
    const TempFile notPe(L"-text.exe",
                         std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                                reinterpret_cast<const std::byte*>(text.data()) + text.size()));
    const UniqueHandle notPeFile = openForRead(notPe.path().wstring());
    ASSERT_TRUE(notPeFile);
    EXPECT_EQ(heldFilePublisher(notPeFile.get()), "");

    const TempFile empty(L"-empty.exe", {});
    const UniqueHandle emptyFile = openForRead(empty.path().wstring());
    ASSERT_TRUE(emptyFile);
    EXPECT_EQ(heldFilePublisher(emptyFile.get()), "") << "an empty file can't be mapped";

    std::vector<std::byte> cutBytes = image.bytes;
    cutBytes.resize(cutBytes.size() - 8);
    const TempFile cut(L"-cut.exe", cutBytes);
    const UniqueHandle cutFile = openForRead(cut.path().wstring());
    ASSERT_TRUE(cutFile);
    EXPECT_EQ(heldFilePublisher(cutFile.get()), "") << "the block runs past the end of the file";

    // A block that names no company: none.
    const TestSupport::PeImage noCompany = TestSupport::buildPeImage(u"");
    const TempFile none(L"-none.exe", noCompany.bytes);
    const UniqueHandle noneFile = openForRead(none.path().wstring());
    ASSERT_TRUE(noneFile);
    EXPECT_EQ(heldFilePublisher(noneFile.get()), "");

    EXPECT_EQ(heldFilePublisher(INVALID_HANDLE_VALUE), "") << "no file";
}

/// @p bytes at the very end of a committed page whose next page is PAGE_NOACCESS, so any read past them faults.
class GuardedBytes
{
  public:
    explicit GuardedBytes(const std::vector<std::byte>& bytes)
    {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        m_Page = system.dwPageSize;
        m_Size = ((bytes.size() + m_Page - 1) / m_Page) * m_Page;
        m_Base = static_cast<std::byte*>(VirtualAlloc(nullptr, m_Size + m_Page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (m_Base == nullptr)
        {
            return;
        }
        DWORD old = 0;
        VirtualProtect(m_Base + m_Size, m_Page, PAGE_NOACCESS, &old);
        m_Data = m_Base + (m_Size - bytes.size());
        std::memcpy(m_Data, bytes.data(), bytes.size());
    }
    GuardedBytes(const GuardedBytes&) = delete;
    GuardedBytes& operator=(const GuardedBytes&) = delete;
    GuardedBytes(GuardedBytes&&) = delete;
    GuardedBytes& operator=(GuardedBytes&&) = delete;
    ~GuardedBytes()
    {
        if (m_Base != nullptr)
        {
            VirtualFree(m_Base, 0, MEM_RELEASE);
        }
    }
    [[nodiscard]] std::byte* data() const noexcept
    {
        return m_Data;
    }

  private:
    std::byte* m_Base = nullptr;
    std::byte* m_Data = nullptr;
    std::size_t m_Size = 0;
    std::size_t m_Page = 0;
};

/// The offset in @p bytes of the version node whose key is @p key: its header, 6 bytes before the key.
std::size_t versionNodeAt(const std::vector<std::byte>& bytes, std::u16string_view key)
{
    std::vector<std::byte> pattern;
    for (const char16_t c : key)
    {
        TestSupport::put16(pattern, pattern.size(), c);
    }
    TestSupport::put16(pattern, pattern.size(), 0);
    const auto found = std::ranges::search(bytes, pattern);
    return found.empty() ? 0 : static_cast<std::size_t>(found.begin() - bytes.begin()) - 6;
}

// #4057 review: a Translation whose node says its value is 64 KB long, in a file. The walk refuses the block, so no
// publisher, and nothing is read past it.
TEST(WindowsHeldFilePublisherTest, AVersionNodeThatClaimsToRunPastItsBlockReadsAsNoPublisher)
{
    TestSupport::PeImage image = TestSupport::buildPeImage(u"TaskSmack Test Co");
    const std::vector<std::byte> block = TestSupport::versionInfoBlock(u"TaskSmack Test Co");
    TestSupport::put16(image.bytes, image.blocks[0] + versionNodeAt(block, u"Translation") + 2, 0xFFFF);
    const TempFile pe(L"-long-translation.exe", image.bytes);
    const UniqueHandle file = openForRead(pe.path().wstring());
    ASSERT_TRUE(file);
    EXPECT_EQ(heldFilePublisher(file.get()), "");
}

// #4057 review: versionInfoCompanyName() reads only the block it is given, whatever lengths VerQueryValueW hands back.
// The block's room ends at a page that can't be read, so a read past it would fault. GetFileVersionInfoW's by-path
// block takes the same path.
TEST(WindowsHeldFilePublisherTest, TheCompanyNameReadStaysInsideTheBlock)
{
    const std::vector<std::byte> bytes = TestSupport::versionInfoBlock(u"TaskSmack Test Co");
    const std::size_t translation = versionNodeAt(bytes, u"Translation");
    const std::size_t company = versionNodeAt(bytes, u"CompanyName");
    ASSERT_NE(translation, 0U);
    ASSERT_NE(company, 0U);

    // The block, then as much zeroed room again as GetFileVersionInfoSizeW would give it, then the unreadable page: a
    // value of 64 KB read to its declared end would fault.
    const auto read = [](std::vector<std::byte> block)
    {
        block.resize((2 * block.size()) + 16);
        const GuardedBytes guarded(block);
        EXPECT_NE(guarded.data(), nullptr);
        if (guarded.data() == nullptr)
        {
            return std::string{};
        }
        return versionInfoCompanyName(std::span<BYTE>(reinterpret_cast<BYTE*>(guarded.data()), block.size()));
    };
    EXPECT_EQ(read(bytes), "TaskSmack Test Co") << "as built";

    std::vector<std::byte> longTranslation = bytes;
    TestSupport::put16(longTranslation, translation + 2, 0xFFFF); // the reviewed case: 64 KB of translations
    EXPECT_EQ(read(longTranslation), "TaskSmack Test Co") << "the translations are read only to the block's end";

    std::vector<std::byte> longName = bytes;
    TestSupport::put16(longName, company + 2, 0xFFFF);
    // VerQueryValueW may refuse this one outright; either way only the block is read.
    const std::string name = read(longName);
    EXPECT_TRUE(name.empty() || name == "TaskSmack Test Co") << name;
}
// #4057: the held file's own bytes, not its name's. While the handle is held the file is renamed away (it was opened
// sharing delete here, as a held image's file never is, so the rename can happen) and another file put at its name:
// the publisher is still the held file's.
TEST(WindowsHeldFilePublisherTest, TheHeldFilesPublisherIsReadNotTheFileNowAtItsName)
{
    const TempFile original(L"-original.exe", TestSupport::buildPeImage(u"Held Co").bytes);
    const UniqueHandle file(CreateFileW(original.path().c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr));
    ASSERT_TRUE(file) << GetLastError();
    std::filesystem::path moved = original.path();
    moved += L".moved";
    std::error_code error;
    std::filesystem::rename(original.path(), moved, error);
    ASSERT_FALSE(error) << error.message();
    {
        const std::vector<std::byte> decoy = TestSupport::buildPeImage(u"Decoy Co").bytes;
        std::ofstream out(original.path(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(decoy.data()), static_cast<std::streamsize>(decoy.size()));
    }
    EXPECT_EQ(heldFilePublisher(file.get()), "Held Co");
    std::filesystem::remove(moved, error);
}

// #4057, the real calls: the publisher read through this test process's own bound and held image matches the read
// by its path, and so does notepad.exe's (Microsoft's) through a handle to it.
TEST(WindowsHeldFilePublisherTest, ThisProcesssAndNotepadsPublisherReadThroughTheHandleMatchTheReadByPath)
{
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    ASSERT_GT(length, 0U);
    module.resize(length);
    std::expected<WindowsSignature::BoundImage, WindowsSignature::BindFailure> bound = openProcessImageForCheck(thisProcess());
    ASSERT_TRUE(bound.has_value()) << bound.error().why;
    const std::string viaHandle = heldFilePublisher(bound->image.file);
    closeImage(bound->image.file);
    EXPECT_EQ(viaHandle, WindowsProcessDetailReads::publisher(module));

    const std::wstring notepad = WinString::utf8ToWide(systemBinary(L"notepad.exe"));
    const UniqueHandle file = openForRead(notepad);
    ASSERT_TRUE(file) << GetLastError();
    const std::string notepadPublisher = heldFilePublisher(file.get());
    EXPECT_EQ(notepadPublisher, WindowsProcessDetailReads::publisher(notepad));
    EXPECT_NE(notepadPublisher.find("Microsoft"), std::string::npos) << notepadPublisher;
}
/// The PID and identity of a running csrss.exe, or nullopt with @p why set when none can be opened for its start time.
/// Its path is System32's: a protected process's image name isn't always shown.
std::optional<WindowsSignature::ImageProcess> findCsrss(std::string& why)
{
    const UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot)
    {
        why = std::format("no process snapshot (error {})", GetLastError());
        return std::nullopt;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    why = "no csrss.exe is running";
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more != 0; more = Process32NextW(snapshot.get(), &entry))
    {
        if (_wcsicmp(entry.szExeFile, L"csrss.exe") != 0)
        {
            continue;
        }
        const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!process)
        {
            why = std::format("csrss.exe (PID {}) can't be opened (error {})", entry.th32ProcessID, GetLastError());
            continue;
        }
        FILETIME creation{};
        FILETIME exitTime{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetProcessTimes(process.get(), &creation, &exitTime, &kernel, &user) == 0)
        {
            why = std::format("csrss.exe's start time can't be read (error {})", GetLastError());
            continue;
        }
        return WindowsSignature::ImageProcess{
            .pid = static_cast<std::int32_t>(entry.th32ProcessID),
            .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
        };
    }
    return std::nullopt;
}

// #4021 review: csrss.exe is a protected process, whose memory map Windows doesn't show, so its image never binds.
// It is checked by its path instead (code integrity required a protected signer to start it), and gets a signature
// rather than staying Not checked. Read-only: it only queries the process.
TEST(WindowsSignatureCheckRealTest, AProtectedProcessIsCheckedByItsPathAndIsSigned)
{
    std::string why;
    const std::optional<WindowsSignature::ImageProcess> csrss = findCsrss(why);
    if (!csrss.has_value())
    {
        GTEST_SKIP() << "csrss.exe can't be identified in this session: " << why;
    }
    const std::expected<WindowsSignature::BoundImage, WindowsSignature::BindFailure> bound = openProcessImageForCheck(*csrss);
    if (bound.has_value())
    {
        closeImage(bound->image.file); // bound after all (a session that can see its map): checked as itself
    }
    else
    {
        EXPECT_TRUE(bound.error().protectedProcess) << bound.error().why;
    }
    WindowsSignature::SignatureFunctions fns = systemSignatureFunctions();
    fns.openProcessImage = &openProcessImageForCheck;
    const std::string path = systemBinary(L"csrss.exe");
    const WindowsSignature::ImageCheck check = WindowsSignature::checkImage(fns, nullptr, path, path, *csrss);
    EXPECT_TRUE(isVerifiedSignature(check.check.signature)) << path << ": " << check.check.note;
    EXPECT_NE(check.check.signer.find("Microsoft"), std::string::npos) << check.check.signer;
}

// #4021 on a CI runner: a process that renamed its image away and left a Microsoft-signed file at the old name. A
// copy of this test executable is started suspended (so nothing of it runs), renamed, and notepad.exe copied to its
// old name. The check binds to the renamed copy, never to the decoy: the copy reads Unsigned, not notepad's signer.
class CiRealSystemSignatureBindingTest : public TestSupport::CiRealSystemTest
{};

/// The suspended child and its folder, ended, waited for and removed however the test leaves.
class SuspendedChild
{
  public:
    explicit SuspendedChild(std::filesystem::path folder) : m_Folder(std::move(folder))
    {}
    SuspendedChild(const SuspendedChild&) = delete;
    SuspendedChild& operator=(const SuspendedChild&) = delete;
    SuspendedChild(SuspendedChild&&) = delete;
    SuspendedChild& operator=(SuspendedChild&&) = delete;
    ~SuspendedChild()
    {
        if (m_Info.hProcess != nullptr)
        {
            TerminateProcess(m_Info.hProcess, 1);
            WaitForSingleObject(m_Info.hProcess, 10'000);
            CloseHandle(m_Info.hProcess);
        }
        if (m_Info.hThread != nullptr)
        {
            CloseHandle(m_Info.hThread);
        }
        std::error_code ignored;
        std::filesystem::remove_all(m_Folder, ignored);
    }
    PROCESS_INFORMATION& info() noexcept
    {
        return m_Info;
    }

  private:
    std::filesystem::path m_Folder;
    PROCESS_INFORMATION m_Info{};
};

TEST_F(CiRealSystemSignatureBindingTest, ARenamedImageIsCheckedNotTheSignedDecoyAtItsOldName)
{
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    ASSERT_GT(length, 0U);
    module.resize(length);
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / std::format(L"tasksmack-4021-{}", GetCurrentProcessId());
    std::filesystem::create_directories(folder);
    SuspendedChild child(folder);
    const std::filesystem::path original = folder / L"started.exe";
    const std::filesystem::path renamed = folder / L"renamed.exe";
    ASSERT_TRUE(std::filesystem::copy_file(module, original, std::filesystem::copy_options::overwrite_existing));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring commandLine = L"\"" + original.wstring() + L"\" --gtest_list_tests";
    ASSERT_NE(
        CreateProcessW(
            original.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &child.info()),
        0)
        << GetLastError();

    // A real-time scan of the new file can hold it for a moment: retry the rename briefly.
    std::error_code error;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        std::filesystem::rename(original, renamed, error);
        if (!error)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_FALSE(error) << "a running image can be renamed: " << error.message();
    const std::filesystem::path notepad = std::filesystem::path(WinString::utf8ToWide(systemBinary(L"notepad.exe")));
    ASSERT_TRUE(std::filesystem::copy_file(notepad, original, std::filesystem::copy_options::overwrite_existing));

    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    ASSERT_NE(GetProcessTimes(child.info().hProcess, &creation, &exitTime, &kernel, &user), 0);
    const WindowsSignature::ImageProcess process{
        .pid = static_cast<std::int32_t>(child.info().dwProcessId),
        .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
    };

    const std::expected<WindowsSignature::BoundImage, WindowsSignature::BindFailure> bound = openProcessImageForCheck(process);
    ASSERT_TRUE(bound.has_value()) << bound.error().why;
    EXPECT_EQ(fileIdentity(bound->image.file), identityOf(renamed.wstring())) << bound->path;
    EXPECT_NE(fileIdentity(bound->image.file), identityOf(original.wstring())) << "not the decoy";
    closeImage(bound->image.file);

    WindowsSignature::SignatureFunctions fns = systemSignatureFunctions();
    fns.openProcessImage = &openProcessImageForCheck;
    const std::string startedPath = WinString::wideToUtf8(original.wstring());
    const WindowsSignature::ImageCheck check = WindowsSignature::checkImage(fns, nullptr, startedPath, startedPath, process);
    const SignatureCheck decoy = WindowsSignature::checkSignature(fns, nullptr, startedPath, startedPath);
    EXPECT_NE(decoy.signer.find("Microsoft"), std::string::npos) << "the decoy is signed: " << decoy.note;
    EXPECT_EQ(check.check.signature, ImageSignature::Unsigned)
        << "the renamed copy of this unsigned test executable, not the decoy: " << check.check.signer << " " << check.check.note;
    const UniqueHandle renamedFile = openForRead(renamed.wstring());
    ASSERT_TRUE(renamedFile) << GetLastError();
    const UniqueHandle decoyFile = openForRead(original.wstring());
    ASSERT_TRUE(decoyFile) << GetLastError();
    const std::string decoyPublisher = heldFilePublisher(decoyFile.get());
    EXPECT_NE(decoyPublisher.find("Microsoft"), std::string::npos) << "the decoy names Microsoft: " << decoyPublisher;
    EXPECT_EQ(check.publisher, heldFilePublisher(renamedFile.get())) << "the renamed copy's publisher";
    EXPECT_NE(check.publisher, decoyPublisher) << "never the decoy's";

    // #4057: Look up on VirusTotal hashes the renamed copy, never the decoy. The suspended child is a native image at
    // its entry point, so on the hosted runner it is hashed, not refused.
    const std::stop_source stop;
    const auto decoyHash = Sha256::hashFile(original, stop.get_token());
    const auto imageHash = Sha256::hashFile(renamed, stop.get_token());
    ASSERT_TRUE(decoyHash.has_value()) << decoyHash.error();
    ASSERT_TRUE(imageHash.has_value()) << imageHash.error();
    WindowsProcessActions actions(realWindowsProcessFunctions());
    const auto hash = actions.imageSha256({.pid = process.pid, .startTimeTicks = process.startTimeTicks}, stop.get_token());
    ASSERT_TRUE(hash.has_value()) << hash.error();
    EXPECT_EQ(*hash, Sha256::toHex(*imageHash)) << "the renamed copy's hash";
    EXPECT_NE(*hash, Sha256::toHex(*decoyHash)) << "never the decoy's";
}

// #4059: the signature worker's exit check, on a real child: this test executable started suspended, so nothing of it
// runs, as WindowsProcessIdentity's exited-child test starts one; it changes nothing on the machine. Running, it hasn't
// exited; ended while its handle is still held (as a build's parent holds a child it hasn't reaped), it has, though its
// PID and start time still name it.
TEST(WindowsSignatureCheckRealTest, TheExitCheckTellsARunningChildFromOneThatHasExited)
{
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    ASSERT_GT(length, 0U);
    module.resize(length);
    // Nothing is copied into it; the child's destructor removes it if it was made.
    SuspendedChild child(std::filesystem::temp_directory_path() / std::format(L"tasksmack-4059-{}", GetCurrentProcessId()));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring commandLine = L"\"" + module + L"\" --gtest_list_tests";
    ASSERT_NE(CreateProcessW(
                  module.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &child.info()),
              0)
        << GetLastError();
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    ASSERT_NE(GetProcessTimes(child.info().hProcess, &creation, &exitTime, &kernel, &user), 0);
    const WindowsSignature::ImageProcess process{
        .pid = static_cast<std::int32_t>(child.info().dwProcessId),
        .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
    };
    EXPECT_FALSE(processExitedForCheck(process)) << "running";

    ASSERT_NE(TerminateProcess(child.info().hProcess, 1), 0) << GetLastError();
    ASSERT_EQ(WaitForSingleObject(child.info().hProcess, 10'000), WAIT_OBJECT_0);
    EXPECT_TRUE(processExitedForCheck(process)) << "exited, its object held open by this test";
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

// #3313: the same, but its ChangeTime given back too, through SetFileInformationByHandle(FileBasicInfo).
// ID, ChangeTime and size all match; the USN, which the change journal moves on and user mode can't set,
// doesn't. Unelevated: the USN is read through the check's own read handle. Skipped where the temp
// folder's volume keeps no change journal.
TEST(WindowsSignatureCheckRealTest, AFileWrittenInPlaceAndItsChangeTimeRestoredHasANewUsn)
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / std::format(L"tasksmack-3313-{}-usn.exe", GetCurrentProcessId());
    std::ofstream(file, std::ios::binary) << "MZ one";
    const std::optional<WindowsSignature::OpenedImage> before = openImage(TrustApiFunctions{}, file.wstring());
    ASSERT_TRUE(before.has_value());
    closeImage(before->file);
    if (before->stamp.usn == 0)
    {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        GTEST_SKIP() << "the temp folder's volume keeps no change journal";
    }
    {
        const UniqueHandle writer(CreateFileW(file.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        ASSERT_TRUE(writer.valid());
        FILE_BASIC_INFO basic{};
        ASSERT_NE(GetFileInformationByHandleEx(writer.get(), FileBasicInfo, &basic, sizeof(basic)), FALSE);
        DWORD wrote = 0;
        ASSERT_NE(WriteFile(writer.get(), "MZ two", 6, &wrote, nullptr), FALSE); // the same size
        ASSERT_NE(SetFileInformationByHandle(writer.get(), FileBasicInfo, &basic, sizeof(basic)), FALSE);
    }
    const std::optional<WindowsSignature::OpenedImage> after = openImage(TrustApiFunctions{}, file.wstring());
    ASSERT_TRUE(after.has_value());
    closeImage(after->file);
    EXPECT_EQ(after->stamp.fileId, before->stamp.fileId);
    EXPECT_EQ(after->stamp.size, before->stamp.size);
    EXPECT_EQ(after->stamp.changeTime, before->stamp.changeTime) << "the ChangeTime given back";
    EXPECT_GT(after->stamp.usn, before->stamp.usn);
    EXPECT_NE(after->stamp, before->stamp) << "the cache would check it again";
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}

} // namespace
} // namespace Platform::Windows
