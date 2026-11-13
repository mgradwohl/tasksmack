/// @file test_WindowsImageSha256.cpp
/// @brief Look up on VirusTotal on Windows (#3646): WindowsProcessActions::imageSha256() hashing the executable
/// QueryFullProcessImageNameW names, through the identity check. The process fakes hand out this test process
/// opened for query only, as test_WindowsFileLocation does, and name a file the test writes, so the hash is
/// known; one test hashes this test process's own image through the real calls. Nothing here is privileged
/// and nothing changes the machine.

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
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform
{
namespace
{

/// OpenProcess, GetProcessTimes, QueryFullProcessImageNameW and GetExitCodeProcess, faked: the state the
/// fakes share (function pointers can't capture), reset by each test.
struct FakeImage
{
    static constexpr DWORD TARGET_TICKS = 4321;
    static inline std::wstring imagePath;
    static inline DWORD openError = ERROR_SUCCESS;  // OpenProcess fails with this, when not 0
    static inline DWORD queryError = ERROR_SUCCESS; // QueryFullProcessImageNameW fails with this, when not 0
    static inline DWORD exitCode = STILL_ACTIVE;    // what GetExitCodeProcess answers
    static inline int opens = 0;
    static inline int exitChecks = 0;

    static void reset(const std::filesystem::path& path)
    {
        imagePath = path.wstring();
        openError = ERROR_SUCCESS;
        queryError = ERROR_SUCCESS;
        exitCode = STILL_ACTIVE;
        opens = 0;
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
        if (queryError != ERROR_SUCCESS)
        {
            SetLastError(queryError);
            return FALSE;
        }
        if (*size <= imagePath.size())
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        std::ranges::copy(imagePath, path);
        path[imagePath.size()] = L'\0'; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the API's buffer
        *size = static_cast<DWORD>(imagePath.size());
        return TRUE;
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
        const auto* const info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_Image = std::filesystem::temp_directory_path() / std::format("tasksmack-image-sha256-{}-{}", GetCurrentProcessId(), info->name());
        m_Image += L"-Ünï.exe"; // wide, so the path isn't the narrow string's code page
        std::ofstream(m_Image, std::ios::binary) << "abc";
        FakeImage::reset(m_Image);
    }
    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(m_Image, ec);
    }

    std::filesystem::path m_Image;
};

TEST_F(WindowsImageSha256Test, TheImageThePathNamesIsHashedAsLowercaseHex)
{
    WindowsProcessActions actions(FakeImage::functions());
    EXPECT_TRUE(actions.actionCapabilities().canHashImage);

    const std::stop_source stop;
    const auto hash = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_TRUE(hash.has_value()) << hash.error();
    EXPECT_EQ(*hash, ABC_SHA256);
    EXPECT_EQ(FakeImage::opens, 1);
    EXPECT_EQ(FakeImage::exitChecks, 1);
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

    // Exited while its file was read: the file at that path may no longer be its image.
    FakeImage::openError = ERROR_SUCCESS;
    FakeImage::exitCode = 0;
    const auto exited = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(exited.has_value());
    EXPECT_EQ(exited.error(), "Process 4242 has exited");
}

TEST_F(WindowsImageSha256Test, AnUnreadablePathOrFileIsAnError)
{
    WindowsProcessActions actions(FakeImage::functions());
    const std::stop_source stop;

    FakeImage::queryError = ERROR_GEN_FAILURE; // a kernel process with no image file
    const auto noPath = actions.imageSha256(TARGET, stop.get_token());
    ASSERT_FALSE(noPath.has_value());
    EXPECT_TRUE(noPath.error().starts_with("Its executable can't be read: ")) << noPath.error();

    FakeImage::queryError = ERROR_SUCCESS;
    FakeImage::imagePath = (m_Image.parent_path() / "tasksmack-no-such-image.exe").wstring();
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

TEST_F(WindowsImageSha256Test, WithoutThePathQueryNothingIsHashed)
{
    WindowsProcessFunctions fns = FakeImage::functions();
    fns.queryFullProcessImageName = nullptr;
    WindowsProcessActions actions(fns);
    EXPECT_FALSE(actions.actionCapabilities().canHashImage);
    const std::stop_source stop;
    EXPECT_FALSE(actions.imageSha256(TARGET, stop.get_token()).has_value());
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
