/// @file test_WindowsImageSha256.cpp
/// @brief Look up on VirusTotal on Windows (#3646): WindowsProcessActions::imageSha256() hashing the file the
/// process's image was mapped from (#3861), through the identity check. The process fakes hand out this test
/// process opened for query only, as test_WindowsFileLocation does, and the image fakes name a file the test
/// writes, by its NT name as GetMappedFileNameW gives one, so the hash is known; one test hashes this test
/// process's own image through the real calls. Nothing here is privileged and nothing changes the machine.

#include "Platform/IProcessActions.h"
#include "Platform/Sha256.h"
#include "Platform/Windows/WindowsProcessActions.h"

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
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>

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

/// The NT name (\Device\HarddiskVolumeN\...) of the file at @p path, as GetMappedFileNameW writes one.
[[nodiscard]] std::wstring ntNameOf(const std::filesystem::path& path)
{
    HANDLE file = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
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

/// OpenProcess, GetProcessTimes, QueryFullProcessImageNameW, NtQueryInformationProcess's ProcessImageInformation,
/// GetMappedFileNameW and GetExitCodeProcess, faked: the state the fakes share (function pointers can't capture),
/// reset by each test.
struct FakeImage
{
    static constexpr DWORD TARGET_TICKS = 4321;
    static inline std::wstring creationPath;         // what QueryFullProcessImageNameW answers: the name at creation
    static inline std::wstring mappedName;           // what GetMappedFileNameW answers: the image's file's NT name now
    static inline std::wstring mappedNameLater;      // when not empty, what it answers from its second call on
    static inline ULONG imageFileSize = 3;           // ProcessImageInformation's ImageFileSize
    static inline LONG imageStatus = 0;              // what ProcessImageInformation answers
    static inline DWORD openError = ERROR_SUCCESS;   // OpenProcess fails with this, when not 0
    static inline DWORD mappedError = ERROR_SUCCESS; // GetMappedFileNameW fails with this, when not 0
    static inline DWORD exitCode = STILL_ACTIVE;     // what GetExitCodeProcess answers
    static inline int entryPoint = 0;                // its address stands in for the image's entry point
    static inline int opens = 0;
    static inline int mappedQueries = 0;
    static inline int exitChecks = 0;

    static void reset(const std::filesystem::path& path)
    {
        creationPath = path.wstring();
        mappedName = ntNameOf(path);
        mappedNameLater.clear();
        imageFileSize = 3;
        imageStatus = 0;
        openError = ERROR_SUCCESS;
        mappedError = ERROR_SUCCESS;
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
    static BOOL WINAPI queryFullProcessImageName(HANDLE /*process*/, DWORD /*flags*/, LPWSTR path, PDWORD size)
    {
        if (*size <= creationPath.size())
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        std::ranges::copy(creationPath, path);
        path[creationPath.size()] = L'\0'; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the API's buffer
        *size = static_cast<DWORD>(creationPath.size());
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
        const FakeSectionImageInformation image{.transferAddress = &entryPoint, .imageFileSize = imageFileSize};
        std::memcpy(info, &image, sizeof(image));
        if (written != nullptr)
        {
            *written = sizeof(image);
        }
        return 0;
    }
    static DWORD WINAPI getMappedFileName(HANDLE /*process*/, LPVOID address, LPWSTR name, DWORD size)
    {
        ++mappedQueries;
        if (mappedError != ERROR_SUCCESS || address != &entryPoint)
        {
            SetLastError(mappedError != ERROR_SUCCESS ? mappedError : ERROR_INVALID_ADDRESS);
            return 0;
        }
        const std::wstring& answer = (mappedQueries > 1 && !mappedNameLater.empty()) ? mappedNameLater : mappedName;
        const DWORD count = std::min<DWORD>(size, static_cast<DWORD>(answer.size()));
        std::copy_n(answer.begin(), count, name);
        return count;
    }
    static BOOL WINAPI getExitCodeProcess(HANDLE /*process*/, LPDWORD code)
    {
        ++exitChecks;
        *code = exitCode;
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
        fns.ntQueryInformationProcess = &ntQueryInformationProcess;
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

    // Renamed between the name being read and the file being opened: the image's name is no longer the file's.
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

    // The name can't be read again once the file is held.
    FakeImage::reset(m_Image);
    FakeImage::mappedNameLater = L"\\Device\\NoSuchVolume\\gone.exe";
    const auto gone = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(gone.has_value());
    EXPECT_EQ(gone.error(), IMAGE_REPLACED);
    EXPECT_EQ(FakeImage::exitChecks, 0) << "nothing hashed, so no exit check";
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

} // namespace
} // namespace Platform
