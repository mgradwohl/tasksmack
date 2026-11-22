/// @file test_WindowsImageSha256.cpp
/// @brief Look up on VirusTotal on Windows (#3646): WindowsProcessActions::imageSha256() hashing the file the
/// process's image was mapped from (#3861), through the identity check. The process fakes hand out this test
/// process opened for query only, as test_WindowsFileLocation does, and the image fakes name a file the test
/// writes, by its NT name as GetMappedFileNameW gives one, so the hash is known; one test hashes this test
/// process's own image through the real calls. Nothing here is privileged and nothing changes the machine.

#include "Mocks/WindowsFakeLocalNtFile.h"
#include "Platform/IProcessActions.h"
#include "Platform/Sha256.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsImageFile.h"
#include "Platform/Windows/WindowsLocalNtFile.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Support/Junction.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform
{
namespace
{

/// ntdll's 64-bit SECTION_IMAGE_INFORMATION, as ProcessImageInformation (class 37) fills it: the fake writes the
/// entry point and the image's size where Windows does.
struct FakeSectionImageInformation
{
    void* transferAddress = nullptr;
    ULONG zeroBits = 0;
    SIZE_T maximumStackSize = 0;
    SIZE_T committedStackSize = 0;
    ULONG subSystemType = 0;
    ULONG subSystemVersion = 0;
    ULONG operatingSystemVersion = 0;
    USHORT imageCharacteristics = 0;
    USHORT dllCharacteristics = 0;
    USHORT machine = 0;
    BOOLEAN imageContainsCode = 0;
    UCHAR imageFlags = 0;
    ULONG loaderFlags = 0;
    ULONG imageFileSize = 0;
    ULONG checkSum = 0;
};
static_assert(sizeof(void*) != 8 || sizeof(FakeSectionImageInformation) == 64);

constexpr ULONG PROCESS_IMAGE_INFORMATION_CLASS = 37;
constexpr LONG STATUS_INVALID_INFO_CLASS_VALUE = static_cast<LONG>(0xC0000003UL);
constexpr LONG STATUS_ACCESS_DENIED_VALUE = static_cast<LONG>(0xC0000022UL);

/// The refusal for a file that isn't the running image (#3861), as imageSha256() words it.
constexpr std::string_view IMAGE_REPLACED =
    "Its executable was renamed or replaced while it was read, so the file there isn't the image it runs; it wasn't hashed";
/// The refusal for a protected process's image (#3861, #4057), as imageSha256() words it.
constexpr std::string_view PROTECTED_REFUSAL =
    "Its executable can't be read: Windows doesn't show which file a protected process runs; it wasn't hashed";
/// The NT name (\Device\HarddiskVolumeN\...) of the file or folder at @p path, as GetMappedFileNameW writes one.
[[nodiscard]] std::wstring ntNameOf(const std::filesystem::path& path)
{
    HANDLE file = CreateFileW(path.c_str(),
                              FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return {};
    }
    std::wstring name(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file, name.data(), static_cast<DWORD>(name.size()), VOLUME_NAME_NT);
    CloseHandle(file);
    name.resize(length);
    return name;
}

/// A fake address in the faked process: integers, never dereferenced.
[[nodiscard]] void* fakeAddress(std::uintptr_t value)
{
    return reinterpret_cast<void*>(value); // NOLINT(performance-no-int-to-ptr) - a fake address
}

/// The faked process's address space: free below DLL_BASE, a DLL's image, the
/// executable's image, then free up to SPACE_END. RELOCATED_ENTRY, in the free
/// part, is where the section says the entry point is when the loader mapped
/// the image elsewhere (a .NET executable, a relocated one).
constexpr std::uintptr_t DLL_BASE = 0x10000;
constexpr std::uintptr_t EXE_BASE = 0x20000;
constexpr std::uintptr_t EXE_END = 0x30000;
constexpr std::uintptr_t ENTRY = EXE_BASE + 0x1234;
constexpr std::uintptr_t RELOCATED_ENTRY = 0x50000;
constexpr std::uintptr_t SPACE_END = 0x60000;

/// OpenProcess, GetProcessTimes, QueryFullProcessImageNameW,
/// NtQueryInformationProcess's ProcessImageInformation, GetMappedFileNameW,
/// VirtualQueryEx, GetExitCodeProcess and GetProcessInformation, faked: the state the fakes share
/// (function pointers can't capture), reset by each test.
struct FakeImage
{
    static constexpr DWORD TARGET_TICKS = 4321;
    static inline std::wstring creationPath;              // what QueryFullProcessImageNameW
                                                          // answers: the name at creation
    static inline std::wstring startedNativeName;         // what it answers with PROCESS_NAME_NATIVE
    static inline std::wstring mappedName;                // what GetMappedFileNameW answers for
                                                          // the image: its NT name now
    static inline std::wstring mappedNameLater;           // when not empty, what it answers
                                                          // from its second call on
    static inline std::wstring dllName;                   // what it answers for the DLL
    static inline ULONG imageFileSize = 3;                // ProcessImageInformation's ImageFileSize
    static inline LONG imageStatus = 0;                   // what ProcessImageInformation answers
    static inline bool noEntryPoint = false;              // it succeeds with a null TransferAddress
    static inline bool relocated = false;                 // the image isn't at the section's entry point
    static inline bool memoryMapHidden = false;           // VirtualQueryEx is refused, as for a protected process
    static inline bool protectedProcess = false;          // GetProcessInformation says it is one
    static inline DWORD openError = ERROR_SUCCESS;        // OpenProcess fails with this, when not 0
    static inline DWORD mappedError = ERROR_SUCCESS;      // GetMappedFileNameW fails with this, when not 0
    static inline DWORD mappedErrorLater = ERROR_SUCCESS; // the same, from its second call on the image
    static inline DWORD exitCode = STILL_ACTIVE;          // what GetExitCodeProcess answers
    static inline int opens = 0;
    static inline int mappedQueries = 0; // GetMappedFileNameW calls on the executable's image
    static inline int exitChecks = 0;

    static void reset(const std::filesystem::path& path)
    {
        creationPath = path.wstring();
        mappedName = ntNameOf(path);
        startedNativeName = mappedName;
        mappedNameLater.clear();
        dllName = L"\\Device\\HarddiskVolume1\\Windows\\System32\\fake.dll";
        imageFileSize = 3;
        imageStatus = 0;
        noEntryPoint = false;
        relocated = false;
        memoryMapHidden = false;
        protectedProcess = false;
        openError = ERROR_SUCCESS;
        mappedError = ERROR_SUCCESS;
        mappedErrorLater = ERROR_SUCCESS;
        exitCode = STILL_ACTIVE;
        opens = 0;
        mappedQueries = 0;
        exitChecks = 0;
    }

    static HANDLE WINAPI openProcess(DWORD /*access*/, BOOL /*inherit*/, DWORD /*pid*/)
    {
        ++opens;
        if (openError != ERROR_SUCCESS)
        {
            SetLastError(openError);
            return nullptr;
        }
        return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    }
    static BOOL WINAPI
    getProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
    {
        creation->dwLowDateTime = TARGET_TICKS;
        creation->dwHighDateTime = 0;
        return TRUE;
    }
    static BOOL WINAPI queryFullProcessImageName(HANDLE /*process*/, DWORD flags, LPWSTR path, PDWORD size)
    {
        const std::wstring& answer = (flags & PROCESS_NAME_NATIVE) != 0 ? startedNativeName : creationPath;
        if (*size <= answer.size())
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        std::ranges::copy(answer, path);
        path[answer.size()] = L'\0'; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                                     // - the API's buffer
        *size = static_cast<DWORD>(answer.size());
        return TRUE;
    }
    static LONG NTAPI ntQueryInformationProcess(HANDLE /*process*/, ULONG infoClass, PVOID info, ULONG length, PULONG written)
    {
        if (infoClass != PROCESS_IMAGE_INFORMATION_CLASS || length < sizeof(FakeSectionImageInformation))
        {
            return STATUS_INVALID_INFO_CLASS_VALUE;
        }
        if (imageStatus != 0)
        {
            return imageStatus;
        }
        void* const entry = noEntryPoint ? nullptr : fakeAddress(relocated ? RELOCATED_ENTRY : ENTRY);
        const FakeSectionImageInformation image{.transferAddress = entry, .imageFileSize = imageFileSize};
        std::memcpy(info, &image, sizeof(image));
        if (written != nullptr)
        {
            *written = sizeof(image);
        }
        return 0;
    }
    static DWORD WINAPI getMappedFileName(HANDLE /*process*/, LPVOID address, LPWSTR name, DWORD size)
    {
        const auto at = reinterpret_cast<std::uintptr_t>(address);
        const std::wstring* answer = nullptr;
        if (at >= DLL_BASE && at < EXE_BASE)
        {
            answer = &dllName;
        }
        else if (at >= EXE_BASE && at < EXE_END)
        {
            ++mappedQueries;
            const DWORD error = mappedQueries > 1 && mappedErrorLater != ERROR_SUCCESS ? mappedErrorLater : mappedError;
            if (error != ERROR_SUCCESS)
            {
                SetLastError(error);
                return 0;
            }
            answer = (mappedQueries > 1 && !mappedNameLater.empty()) ? &mappedNameLater : &mappedName;
        }
        if (answer == nullptr)
        {
            SetLastError(ERROR_INVALID_ADDRESS);
            return 0;
        }
        const DWORD count = std::min<DWORD>(size, static_cast<DWORD>(answer->size()));
        std::copy_n(answer->begin(), count, name);
        return count;
    }
    static SIZE_T WINAPI virtualQueryEx(HANDLE /*process*/, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size)
    {
        const auto at = reinterpret_cast<std::uintptr_t>(address);
        if (memoryMapHidden)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return 0;
        }
        if (at >= SPACE_END || size < sizeof(MEMORY_BASIC_INFORMATION))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
        std::uintptr_t base = 0;
        std::uintptr_t end = DLL_BASE;
        DWORD type = 0;
        if (at >= DLL_BASE && at < EXE_END)
        {
            base = at < EXE_BASE ? DLL_BASE : EXE_BASE;
            end = base + (EXE_BASE - DLL_BASE);
            type = MEM_IMAGE;
        }
        else if (at >= EXE_END)
        {
            base = EXE_END;
            end = SPACE_END;
        }
        *info = MEMORY_BASIC_INFORMATION{};
        info->BaseAddress = fakeAddress(base);
        info->AllocationBase = type == MEM_IMAGE ? fakeAddress(base) : nullptr;
        info->RegionSize = end - base;
        info->State = type == MEM_IMAGE ? MEM_COMMIT : MEM_FREE;
        info->Type = type;
        return sizeof(MEMORY_BASIC_INFORMATION);
    }
    static BOOL WINAPI getExitCodeProcess(HANDLE /*process*/, LPDWORD code)
    {
        ++exitChecks;
        *code = exitCode;
        return TRUE;
    }
    static BOOL WINAPI getProcessInformation(HANDLE /*process*/, PROCESS_INFORMATION_CLASS infoClass, LPVOID info, DWORD size)
    {
        if (infoClass != ProcessProtectionLevelInfo || size < sizeof(PROCESS_PROTECTION_LEVEL_INFORMATION))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        static_cast<PROCESS_PROTECTION_LEVEL_INFORMATION*>(info)->ProtectionLevel =
            protectedProcess ? PROTECTION_LEVEL_ANTIMALWARE_LIGHT : PROTECTION_LEVEL_NONE;
        return TRUE;
    }

    [[nodiscard]] static WindowsProcessFunctions functions()
    {
        WindowsProcessFunctions fns{};
        fns.openProcess = &openProcess;
        fns.getProcessTimes = &getProcessTimes;
        fns.queryFullProcessImageName = &queryFullProcessImageName;
        fns.getExitCodeProcess = &getExitCodeProcess;
        fns.getMappedFileName = &getMappedFileName;
        fns.virtualQueryEx = &virtualQueryEx;
        fns.ntQueryInformationProcess = &ntQueryInformationProcess;
        fns.getProcessInformation = &getProcessInformation;
        fns.localFile = Windows::systemLocalFileFunctions();
        return fns;
    }
};

constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = FakeImage::TARGET_TICKS};

/// SHA-256("abc"), FIPS 180-4's first example.
constexpr std::string_view ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

class WindowsImageSha256Test : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Image = fileNamed(L"-Ünï.exe"); // wide, so the path isn't the narrow string's code page
        std::ofstream(m_Image, std::ios::binary) << "abc";
        FakeImage::reset(m_Image);
        ASSERT_FALSE(FakeImage::mappedName.empty());
    }
    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(m_Image, ec);
        std::filesystem::remove(m_Other, ec);
    }

    /// A path in the temp folder, this process's and this test's own, ending in @p suffix.
    [[nodiscard]] static std::filesystem::path fileNamed(std::wstring_view suffix)
    {
        const auto* const info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::filesystem::path path =
            std::filesystem::temp_directory_path() / std::format("tasksmack-image-sha256-{}-{}", GetCurrentProcessId(), info->name());
        path += suffix;
        return path;
    }

    /// A second file, holding @p content: the decoy at the image's old name, or the file that isn't the image.
    void writeOther(std::string_view content)
    {
        m_Other = fileNamed(L"-other.exe");
        std::ofstream(m_Other, std::ios::binary) << content;
    }

    std::filesystem::path m_Image;
    std::filesystem::path m_Other;
};

TEST_F(WindowsImageSha256Test, TheFileTheImageWasMappedFromIsHashedAsLowercaseHex)
{
    WindowsProcessActions actions(FakeImage::functions());
    EXPECT_TRUE(actions.actionCapabilities().canHashImage);

    const std::stop_source stop;
    const auto hash = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_TRUE(hash.has_value()) << hash.error();
    EXPECT_EQ(*hash, ABC_SHA256);
    EXPECT_EQ(FakeImage::opens, 1);
    EXPECT_EQ(FakeImage::mappedQueries, 2) << "once to find the file, once more to confirm it once it is held";
    EXPECT_EQ(FakeImage::exitChecks, 1);
}

// #3861: a running image renamed, and another file put at its old name. The hash is the image's, not the decoy's.
TEST_F(WindowsImageSha256Test, ARenamedImageIsHashedNotTheFileNowAtItsOldName)
{
    writeOther("a benign decoy");
    FakeImage::creationPath = m_Other.wstring(); // QueryFullProcessImageNameW's name, now the decoy's

    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;
    const auto hash = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_TRUE(hash.has_value()) << hash.error();
    EXPECT_EQ(*hash, ABC_SHA256);
}

TEST_F(WindowsImageSha256Test, AFileThatIsntTheRunningImageIsRefusedAndSaysSo)
{
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;

    // Renamed between the name being read and the file being held: the file under the image's name now is another one,
    // by volume serial and file ID (the same content and size, so only identity tells them apart).
    writeOther("abc");
    FakeImage::mappedNameLater = ntNameOf(m_Other);
    const auto renamed = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(renamed.has_value());
    EXPECT_EQ(renamed.error(), IMAGE_REPLACED);

    // The right name, but not the image's size.
    FakeImage::reset(m_Image);
    FakeImage::imageFileSize = 4;
    const auto resized = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(resized.has_value());
    EXPECT_EQ(resized.error(), IMAGE_REPLACED);

    EXPECT_EQ(FakeImage::exitChecks, 0) << "nothing hashed, so no exit check";
}

// A check that can't be made isn't called tampering: it says it couldn't confirm the file, and why.
TEST_F(WindowsImageSha256Test, ACheckThatCantBeMadeIsUnconfirmedNotReplaced)
{
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;

    // The image's name now opens nothing (a volume gone, say).
    FakeImage::mappedNameLater = L"\\Device\\NoSuchVolume\\gone.exe";
    const auto gone = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(gone.has_value());
    EXPECT_TRUE(gone.error().starts_with(
        "Its executable couldn't be confirmed as the image it runs (the file under its name now can't be opened: "))
        << gone.error();
    EXPECT_TRUE(gone.error().ends_with("), so it wasn't hashed")) << gone.error();

    // The name can't be read again once the file is held.
    FakeImage::reset(m_Image);
    FakeImage::mappedErrorLater = ERROR_INVALID_ADDRESS;
    const auto unread = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(unread.has_value());
    EXPECT_TRUE(unread.error().starts_with("Its executable couldn't be confirmed as the image it runs (its name can't be read again: "))
        << unread.error();

    // ...because the process has exited: said so.
    FakeImage::reset(m_Image);
    FakeImage::mappedErrorLater = ERROR_INVALID_ADDRESS;
    FakeImage::exitCode = 0;
    const auto exited = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(exited.has_value());
    EXPECT_EQ(exited.error(), "Process 4242 has exited");
}

// The held file and the file under the image's name are compared by identity, never by name, so another form of
// the same name (another case, a hard link, an 8.3 short name) matches.
TEST_F(WindowsImageSha256Test, AnotherFormOfTheImagesNameIsTheSameFile)
{
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;

    std::wstring upper = FakeImage::mappedName;
    std::ranges::transform(upper, upper.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    FakeImage::mappedNameLater = upper;
    const auto otherCase = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_TRUE(otherCase.has_value()) << otherCase.error();
    EXPECT_EQ(*otherCase, ABC_SHA256);

    const std::filesystem::path link = fileNamed(L"-link.exe");
    ASSERT_NE(CreateHardLinkW(link.c_str(), m_Image.c_str(), nullptr), 0);
    FakeImage::reset(m_Image);
    FakeImage::mappedName = ntNameOf(link); // the name it was opened by
    const auto linked = actions.imageSha256(TARGET, stop.get_token());
    std::error_code ec;
    std::filesystem::remove(link, ec);
    ASSERT_TRUE(linked.has_value()) << linked.error();
    EXPECT_EQ(*linked, ABC_SHA256);

    // An 8.3 short name, where the volume makes them.
    std::wstring shortPath(32768, L'\0');
    const DWORD length = GetShortPathNameW(m_Image.c_str(), shortPath.data(), static_cast<DWORD>(shortPath.size()));
    shortPath.resize(length);
    const std::wstring shortLeaf = std::filesystem::path(shortPath).filename().wstring();
    if (length != 0 && shortLeaf != m_Image.filename().wstring())
    {
        FakeImage::reset(m_Image);
        FakeImage::mappedNameLater = ntNameOf(m_Image.parent_path()) + L"\\" + shortLeaf;
        const auto shortName = actions.imageSha256(TARGET, stop.get_token());
        ASSERT_TRUE(shortName.has_value()) << shortName.error();
        EXPECT_EQ(*shortName, ABC_SHA256);
    }
}

/// The refusal for an image that isn't at its entry point (#4057), as imageSha256() words it.
constexpr std::string_view NOT_AT_ENTRY_POINT =
    "Its executable's image isn't where Windows says it is, so it couldn't be confirmed; it wasn't hashed";

// #4057: an image the loader mapped away from its entry point (a .NET executable, a relocated one) isn't hashed,
// saying why, and nothing is looked for by the name it started with.
TEST_F(WindowsImageSha256Test, AnImageNotAtItsEntryPointIsntHashed)
{
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;

    FakeImage::relocated = true;
    const auto refused = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), NOT_AT_ENTRY_POINT);
    EXPECT_EQ(FakeImage::mappedQueries, 0) << "no mapping looked up by name";

    // Without VirtualQueryEx the entry point is taken as the image, and nothing is there.
    WindowsProcessFunctions fns = FakeImage::functions();
    fns.virtualQueryEx = nullptr;
    WindowsProcessActions noQuery(fns);
    FakeImage::reset(m_Image);
    FakeImage::relocated = true;
    const auto unread = noQuery.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(unread.has_value());
    EXPECT_TRUE(unread.error().starts_with("Its executable can't be read: ")) << unread.error();
}

// #4057, the attack the name scan let through: a relocated image renames itself away, a signed file is copied to its
// old name and mapped as a resource (an image mapping under the name it started with), and the image is padded to
// the decoy's size beforehand. The decoy is never hashed: the image isn't at its entry point, so nothing is.
TEST_F(WindowsImageSha256Test, ADecoyMappedUnderTheNameItStartedWithIsNeverHashed)
{
    writeOther("xyz"); // the decoy, the image's size (3), so a size check alone would pass it
    FakeImage::relocated = true;
    FakeImage::creationPath = m_Other.wstring();
    FakeImage::startedNativeName = ntNameOf(m_Other);
    FakeImage::mappedName = FakeImage::startedNativeName; // the image mapping at EXE_BASE is the decoy's

    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;
    const auto hash = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(hash.has_value()) << "hashed " << hash.value_or("") << ", the decoy's";
    EXPECT_EQ(hash.error(), NOT_AT_ENTRY_POINT);
}

// The signature check uses the same binding with nothing to add: a refusal ends at its reason.
TEST_F(WindowsImageSha256Test, WithoutASuffixARefusalEndsAtItsReason)
{
    const WindowsProcessFunctions process = FakeImage::functions();
    const Windows::ImageFileFunctions fns{
        .ntQueryInformationProcess = process.ntQueryInformationProcess,
        .getMappedFileName = process.getMappedFileName,
        .virtualQueryEx = process.virtualQueryEx,
        .getExitCodeProcess = process.getExitCodeProcess,
        .getProcessInformation = process.getProcessInformation,
        .localFile = process.localFile,
    };
    FakeImage::relocated = true;
    const auto refused = Windows::openProcessImageFile(fns, GetCurrentProcess(), TARGET.pid, {});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), "Its executable's image isn't where Windows says it is, so it couldn't be confirmed");
    EXPECT_EQ(FakeImage::mappedQueries, 0) << "nothing looked for by name";

    FakeImage::reset(m_Image);
    const auto atEntryPoint = Windows::openProcessImageFile(fns, GetCurrentProcess(), TARGET.pid, {});
    EXPECT_TRUE(atEntryPoint.has_value()) << atEntryPoint.error();

    writeOther("abc");
    FakeImage::reset(m_Image);
    FakeImage::mappedNameLater = ntNameOf(m_Other);
    const auto replaced = Windows::openProcessImageFile(fns, GetCurrentProcess(), TARGET.pid, {});
    ASSERT_FALSE(replaced.has_value());
    EXPECT_FALSE(replaced.error().contains("hashed")) << replaced.error();
    EXPECT_FALSE(replaced.error().ends_with("; ")) << replaced.error();

    FakeImage::reset(m_Image);
    FakeImage::memoryMapHidden = true;
    FakeImage::protectedProcess = true;
    const auto hidden = Windows::openProcessImageFile(fns, GetCurrentProcess(), TARGET.pid, {});
    ASSERT_FALSE(hidden.has_value());
    EXPECT_EQ(hidden.error(), "Its executable can't be read: Windows doesn't show which file a protected process runs");
}
/// openProcessImageFile()'s calls from FakeImage's, with the image's file opened through @p localFile.
[[nodiscard]] Windows::ImageFileFunctions imageFunctions(const Windows::LocalFileFunctions& localFile)
{
    const WindowsProcessFunctions process = FakeImage::functions();
    return {
        .ntQueryInformationProcess = process.ntQueryInformationProcess,
        .getMappedFileName = process.getMappedFileName,
        .virtualQueryEx = process.virtualQueryEx,
        .getExitCodeProcess = process.getExitCodeProcess,
        .getProcessInformation = process.getProcessInformation,
        .localFile = localFile,
    };
}

// #4482: both opens of the image's file, the one held and hashed and the one under its name again, go through
// Windows::openLocalNtFile(): the NT name itself, no link on the way or at the name followed, identification-level
// security. The held one is shared for read only, so the file can't be renamed or written while it is held.
TEST_F(WindowsImageSha256Test, BothOpensOfTheImagesFileGoThroughTheSharedOpen)
{
    TestMocks::localNtFile() = {};
    TestMocks::localNtFile().forward = true; // recorded, then the real open, so the real file is held and compared
    const auto file = Windows::openProcessImageFile(imageFunctions(TestMocks::FAKE_LOCAL_NT_FILE), GetCurrentProcess(), TARGET.pid, {});
    ASSERT_TRUE(file.has_value()) << file.error();
    const std::vector<TestMocks::LocalNtOpen>& opens = TestMocks::localNtFile().opens;
    ASSERT_EQ(opens.size(), 2U);
    for (const TestMocks::LocalNtOpen& open : opens)
    {
        EXPECT_EQ(open.name, FakeImage::mappedName) << R"(the NT name itself, not through \\?\GLOBALROOT)";
        EXPECT_NE(open.objectAttributes & TestMocks::OBJ_DONT_REPARSE_FLAG, 0U);
        EXPECT_NE(open.options & static_cast<ULONG>(FILE_OPEN_REPARSE_POINT), 0U);
        EXPECT_EQ(open.impersonation, SecurityIdentification);
    }
    EXPECT_EQ(opens[0].access, static_cast<ACCESS_MASK>(GENERIC_READ | SYNCHRONIZE));
    EXPECT_EQ(opens[0].share, static_cast<ULONG>(FILE_SHARE_READ));
    EXPECT_EQ(opens[1].access, static_cast<ACCESS_MASK>(FILE_READ_ATTRIBUTES | SYNCHRONIZE));
    EXPECT_EQ(opens[1].share, static_cast<ULONG>(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
    TestMocks::localNtFile() = {};
}

// #4482: a refused open is the same refusal it always was ("Its executable can't be read: ..."), now saying why: a
// link at the name, or a network share's image, which is never opened, so Look up on VirusTotal doesn't hash it and
// the Publisher column doesn't read it.
TEST_F(WindowsImageSha256Test, ALinkOrAShareImageIsRefusedAndSaysWhy)
{
    TestMocks::localNtFile() = {};
    TestMocks::localNtFile().attributes = FILE_ATTRIBUTE_REPARSE_POINT;
    TestMocks::localNtFile().reparseTag = IO_REPARSE_TAG_SYMLINK;
    const auto link = Windows::openProcessImageFile(imageFunctions(TestMocks::FAKE_LOCAL_NT_FILE), GetCurrentProcess(), TARGET.pid, {});
    ASSERT_FALSE(link.has_value());
    EXPECT_EQ(link.error(), "Its executable can't be read: it is a symbolic link or a junction, which TaskSmack doesn't follow");

    WindowsProcessFunctions process = FakeImage::functions();
    process.localFile = TestMocks::FAKE_LOCAL_NT_FILE;
    WindowsProcessActions actions(process);
    const std::stop_source stop;
    const auto unhashed = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(unhashed.has_value());
    EXPECT_EQ(unhashed.error(), link.error());

    TestMocks::localNtFile() = {};
    FakeImage::mappedName = L"\\Device\\Mup\\attacker\\share\\x.exe";
    const auto share = Windows::openProcessImageFile(imageFunctions(TestMocks::FAKE_LOCAL_NT_FILE), GetCurrentProcess(), TARGET.pid, {});
    ASSERT_FALSE(share.has_value());
    EXPECT_EQ(share.error(), "Its executable can't be read: it isn't on a drive TaskSmack can check");
    EXPECT_TRUE(TestMocks::localNtFile().opens.empty()) << "a share's file is never opened";

    // The image's name, read again once its file is held, now a share's: never opened, and unconfirmed.
    FakeImage::reset(m_Image);
    FakeImage::mappedNameLater = L"\\Device\\Mup\\attacker\\share\\x.exe";
    WindowsProcessActions realOpen(FakeImage::functions()); // the held file opened for real, so its identity reads
    const auto later = realOpen.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(later.has_value());
    EXPECT_EQ(later.error(),
              "Its executable couldn't be confirmed as the image it runs (the file under its name now can't be opened: it isn't on a "
              "drive TaskSmack can check), so it wasn't hashed");
    TestMocks::localNtFile() = {};
}

// #4482, on the real system with no network: the image's name, or its name read again, through a junction (one the
// process could have swapped in on the way since its name was read, to \\attacker\share say) fails the open and
// is never followed, so nothing is hashed; the same file by its own name is. The junction is removed before the folder.
TEST_F(WindowsImageSha256Test, AnImageNameThroughAJunctionIsNotFollowed)
{
    const std::filesystem::path folder = fileNamed(L"-junction");
    const std::filesystem::path target = folder / L"target";
    const std::filesystem::path link = folder / L"link";
    std::filesystem::create_directories(target);
    std::filesystem::create_directory(link);
    const std::filesystem::path image = target / L"image.exe";
    std::ofstream(image, std::ios::binary) << "abc";
    const std::optional<std::string> notMade = ::TestSupport::makeJunction(link, target);
    ASSERT_FALSE(notMade.has_value()) << notMade.value_or(std::string{});
    const std::wstring folderName = ntNameOf(folder);
    ASSERT_FALSE(folderName.empty());

    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;
    FakeImage::reset(image);
    const auto direct = actions.imageSha256(TARGET, stop.get_token());
    EXPECT_EQ(direct.value_or(""), ABC_SHA256) << direct.error_or("");

    FakeImage::reset(image);
    FakeImage::mappedName = folderName + L"\\link\\image.exe";
    const auto through = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(through.has_value()) << "hashed through the junction";
    EXPECT_TRUE(through.error().starts_with("Its executable can't be read: ")) << through.error();

    FakeImage::reset(image);
    FakeImage::mappedNameLater = folderName + L"\\link\\image.exe";
    const auto laterThrough = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(laterThrough.has_value());
    EXPECT_TRUE(laterThrough.error().starts_with(
        "Its executable couldn't be confirmed as the image it runs (the file under its name now can't be opened: "))
        << laterThrough.error();
    EXPECT_TRUE(std::filesystem::exists(link / L"image.exe")) << "a path through it that a by-name open would have followed";

    const bool removed = ::TestSupport::removeJunction(link);
    std::error_code ec;
    if (removed)
    {
        std::filesystem::remove_all(folder, ec);
    }
    ASSERT_TRUE(removed) << "an empty folder again, so removing the folder removes nothing behind it";
}

TEST_F(WindowsImageSha256Test, AProtectedProcessOrOneWithNoImageSaysSo)
{
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;

    FakeImage::mappedError = ERROR_ACCESS_DENIED;
    FakeImage::protectedProcess = true;
    const auto denied = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error(), PROTECTED_REFUSAL);

    // #4057 review: access denied from a process that isn't protected isn't called protected.
    FakeImage::reset(m_Image);
    FakeImage::mappedError = ERROR_ACCESS_DENIED;
    const auto deniedUnprotected = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(deniedUnprotected.has_value());
    EXPECT_TRUE(deniedUnprotected.error().starts_with("Its executable can't be read: ")) << deniedUnprotected.error();
    EXPECT_FALSE(deniedUnprotected.error().contains("protected")) << deniedUnprotected.error();

    // #4057: a protected process whose memory map isn't shown at all (VirtualQueryEx refused) is refused as protected,
    // not as an image that isn't where Windows says, and never hashed by its path instead.
    FakeImage::reset(m_Image);
    FakeImage::memoryMapHidden = true;
    FakeImage::protectedProcess = true;
    const auto hidden = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(hidden.has_value());
    EXPECT_EQ(hidden.error(), PROTECTED_REFUSAL);
    EXPECT_EQ(FakeImage::mappedQueries, 0);

    // #4057 review: the same, for a process that isn't protected, is a query that failed, worded by its error, not a
    // claim that the image is elsewhere.
    FakeImage::reset(m_Image);
    FakeImage::memoryMapHidden = true;
    const auto unprotected = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(unprotected.has_value());
    EXPECT_EQ(unprotected.error(), "Its executable can't be read: " + Platform::WinString::win32ErrorText(ERROR_ACCESS_DENIED));

    // ...and for one that has exited, says so.
    FakeImage::reset(m_Image);
    FakeImage::memoryMapHidden = true;
    FakeImage::protectedProcess = true;
    FakeImage::exitCode = 0;
    const auto exited = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(exited.has_value());
    EXPECT_EQ(exited.error(), "Process 4242 has exited");

    FakeImage::reset(m_Image);
    FakeImage::noEntryPoint = true;
    const auto none = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error(), "Its executable can't be read: Windows reports no image for it");
}

// The file is opened without FILE_SHARE_DELETE, so it can't be renamed while it is read; one someone holds open to
// rename (with DELETE access) can't be opened at all, and is an error, never read.
TEST_F(WindowsImageSha256Test, AFileHeldOpenForARenameIsntRead)
{
    HANDLE renaming = CreateFileW(m_Image.c_str(),
                                  DELETE | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  0,
                                  nullptr);
    ASSERT_NE(renaming, INVALID_HANDLE_VALUE);
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;
    const auto hash = actions.imageSha256(TARGET, stop.get_token());
    CloseHandle(renaming);
    ASSERT_FALSE(hash.has_value());
    EXPECT_TRUE(hash.error().starts_with("Its executable can't be read: ")) << hash.error();
}

TEST_F(WindowsImageSha256Test, TheSystemProcessAndPidZeroAreRefusedBeforeAnythingIsOpened)
{
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;

    const auto system = actions.imageSha256({.pid = 4, .startTimeTicks = FakeImage::TARGET_TICKS}, stop.get_token());
    ASSERT_FALSE(system.has_value());
    EXPECT_EQ(system.error(), "The System process has no executable file");

    const auto idle = actions.imageSha256({.pid = 0, .startTimeTicks = FakeImage::TARGET_TICKS}, stop.get_token());
    ASSERT_FALSE(idle.has_value());
    EXPECT_EQ(idle.error(), "Invalid PID");
    EXPECT_EQ(FakeImage::opens, 0);
}

TEST_F(WindowsImageSha256Test, AVanishedOrReusedProcessIsRefused)
{
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;

    // A PID now another process's: the start time isn't the target's.
    EXPECT_FALSE(actions.imageSha256({.pid = 4242, .startTimeTicks = FakeImage::TARGET_TICKS + 1}, stop.get_token()).has_value());

    // Gone before it could be opened: OpenProcess's ERROR_INVALID_PARAMETER.
    FakeImage::openError = ERROR_INVALID_PARAMETER;
    const auto gone = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(gone.has_value());
    EXPECT_EQ(gone.error(), "Process not found - may have already exited");

    // Exited while its file was read: its image is gone.
    FakeImage::openError = ERROR_SUCCESS;
    FakeImage::exitCode = 0;
    const auto exited = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(exited.has_value());
    EXPECT_EQ(exited.error(), "Process 4242 has exited");
}

TEST_F(WindowsImageSha256Test, AnUnreadableImageOrFileIsAnError)
{
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;

    FakeImage::imageStatus = STATUS_ACCESS_DENIED_VALUE; // a kernel process with no image, or one that can't be asked
    const auto noImage = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(noImage.has_value());
    EXPECT_EQ(noImage.error(), "Its executable can't be read: its image can't be found (NTSTATUS 0xC0000022)");

    FakeImage::imageStatus = 0;
    FakeImage::mappedError = ERROR_ACCESS_DENIED;
    const auto noName = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(noName.has_value());
    EXPECT_TRUE(noName.error().starts_with("Its executable can't be read: ")) << noName.error();

    FakeImage::mappedError = ERROR_SUCCESS;
    FakeImage::mappedName = ntNameOf(m_Image.parent_path()) + L"\\tasksmack-no-such-image.exe";
    const auto noFile = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(noFile.has_value());
    EXPECT_TRUE(noFile.error().starts_with("Its executable can't be read: ")) << noFile.error();
}

TEST_F(WindowsImageSha256Test, AStopAlreadyRequestedAbandonsTheHash)
{
    WindowsProcessActions actions(FakeImage::functions());
    std::stop_source stop;
    static_cast<void>(stop.request_stop());
    const auto hash = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(hash.has_value());
    EXPECT_EQ(hash.error(), "Stopped");
}

TEST_F(WindowsImageSha256Test, WithoutTheMappedNameOrTheImageQueryNothingIsHashed)
{
    const std::stop_source stop;
    {
        WindowsProcessFunctions fns = FakeImage::functions();
        fns.getMappedFileName = nullptr;
        WindowsProcessActions actions(fns);
        EXPECT_FALSE(actions.actionCapabilities().canHashImage);
        EXPECT_FALSE(actions.imageSha256(TARGET, stop.get_token()).has_value());
    }
    {
        WindowsProcessFunctions fns = FakeImage::functions();
        fns.ntQueryInformationProcess = nullptr;
        WindowsProcessActions actions(fns);
        EXPECT_FALSE(actions.actionCapabilities().canHashImage);
        EXPECT_FALSE(actions.imageSha256(TARGET, stop.get_token()).has_value());
    }
    EXPECT_EQ(FakeImage::opens, 0);
}

// The real calls on this test process: its own image, which needs no privilege to open or read.
TEST(WindowsImageSha256RealTest, TheTestProcesssOwnImageHashesAsItsFileDoes)
{
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    ASSERT_GT(length, 0U);
    module.resize(length);
    const std::stop_source stop;
    const auto expected = Sha256::hashFile(std::filesystem::path(module), stop.get_token());
    ASSERT_TRUE(expected.has_value()) << expected.error();

    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    ASSERT_NE(GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user), 0);
    const ProcessTarget self{
        .pid = static_cast<std::int32_t>(GetCurrentProcessId()),
        .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime,
    };

    WindowsProcessActions actions(realWindowsProcessFunctions());
    EXPECT_TRUE(actions.actionCapabilities().canHashImage);
    const auto hash = actions.imageSha256(self, stop.get_token());
    ASSERT_TRUE(hash.has_value()) << hash.error();
    EXPECT_EQ(*hash, Sha256::toHex(*expected));
    EXPECT_EQ(hash->size(), 64U);
}

/// GetMappedFileNameW answering with a name that fills the whole buffer: it truncates silently, so that reads as an
/// error, never as the name.
DWORD WINAPI fullBufferMappedName(HANDLE /*process*/, LPVOID /*address*/, LPWSTR name, DWORD size)
{
    std::fill_n(name, size - 1, L'a');
    return size - 1;
}

// A mapped name that may have been cut short isn't opened: the refusal says Windows' "file name is too long".
TEST_F(WindowsImageSha256Test, AMappedNameThatFillsTheBufferIsRefusedNotTruncated)
{
    Windows::ImageFileFunctions fns = imageFunctions(TestMocks::FAKE_LOCAL_NT_FILE);
    fns.getMappedFileName = &fullBufferMappedName;
    TestMocks::localNtFile() = {};
    const auto file = Windows::openProcessImageFile(fns, GetCurrentProcess(), TARGET.pid, {});
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error(), "Its executable can't be read: " + WinString::win32ErrorText(ERROR_FILENAME_EXCED_RANGE));
    EXPECT_TRUE(TestMocks::localNtFile().opens.empty()) << "a name that may be cut short is never opened";
    TestMocks::localNtFile() = {};
}

BOOL WINAPI protectionCallFails(HANDLE /*process*/, PROCESS_INFORMATION_CLASS /*infoClass*/, LPVOID /*info*/, DWORD /*size*/)
{
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

// Whether a process is protected is asked of GetProcessInformation only: without the call, or when it fails, the
// answer is "not protected", so a refusal never claims protection it didn't see.
TEST_F(WindowsImageSha256Test, ProtectionReadsAsNoneWithoutAnAnswer)
{
    Windows::ImageFileFunctions fns = imageFunctions(TestMocks::FAKE_LOCAL_NT_FILE);

    FakeImage::protectedProcess = true;
    EXPECT_TRUE(Windows::isProtectedProcess(fns, GetCurrentProcess()));
    FakeImage::protectedProcess = false;
    EXPECT_FALSE(Windows::isProtectedProcess(fns, GetCurrentProcess()));

    fns.getProcessInformation = &protectionCallFails;
    FakeImage::protectedProcess = true; // never reached: the call fails
    EXPECT_FALSE(Windows::isProtectedProcess(fns, GetCurrentProcess()));

    fns.getProcessInformation = nullptr;
    EXPECT_FALSE(Windows::isProtectedProcess(fns, GetCurrentProcess()));

    // And the refusal for a process whose memory map is hidden is then the generic one, not "protected".
    FakeImage::memoryMapHidden = true;
    const auto hidden = Windows::openProcessImageFile(fns, GetCurrentProcess(), TARGET.pid, {});
    ASSERT_FALSE(hidden.has_value());
    EXPECT_TRUE(hidden.error().starts_with("Its executable can't be read: ")) << hidden.error();
    EXPECT_FALSE(hidden.error().contains("protected process")) << hidden.error();
}

// A handle that can't be asked has no identity and no name: both read as nothing, never as a stale answer.
TEST_F(WindowsImageSha256Test, ABadHandleHasNoIdentityAndNoName)
{
    EXPECT_FALSE(Windows::fileIdentity(INVALID_HANDLE_VALUE).has_value());
    EXPECT_TRUE(Windows::heldFileName(INVALID_HANDLE_VALUE).empty());
}

// Two files differ in identity and one equals itself, by the volume and file ID the file system gives.
TEST_F(WindowsImageSha256Test, FileIdentitySeparatesTwoFilesAndMatchesOneThroughTwoHandles)
{
    writeOther("abc");
    const auto open = [](const std::filesystem::path& path)
    {
        return Windows::UniqueHandle(CreateFileW(path.c_str(),
                                                 FILE_READ_ATTRIBUTES,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 nullptr,
                                                 OPEN_EXISTING,
                                                 FILE_ATTRIBUTE_NORMAL,
                                                 nullptr));
    };
    const Windows::UniqueHandle first = open(m_Image);
    const Windows::UniqueHandle again = open(m_Image);
    const Windows::UniqueHandle other = open(m_Other);
    ASSERT_TRUE(first && again && other);
    const auto identity = Windows::fileIdentity(first.get());
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(identity, Windows::fileIdentity(again.get()));
    EXPECT_NE(identity, Windows::fileIdentity(other.get()));
    EXPECT_FALSE(Windows::heldFileName(first.get()).empty());
}

} // namespace
} // namespace Platform
