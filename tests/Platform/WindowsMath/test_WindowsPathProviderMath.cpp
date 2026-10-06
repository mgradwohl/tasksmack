/// @file test_WindowsPathProviderMath.cpp
/// @brief Unit tests for WindowsPathProviderMath.h's pure executable/config directory fallbacks
///
/// WindowsPathProviderMath.h includes no Windows header, so these tests build and run on every
/// platform, including Linux CI's sanitizer and coverage jobs (#1133). Tests of the real provider
/// stay in test_WindowsPathProvider.cpp.

#include "Platform/Windows/WindowsPathProviderMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace Platform
{
namespace
{

// =============================================================================
// resolveExecutableDir / resolveUserConfigDir: pure logic extracted from
// WindowsPathProvider.cpp, with injected OS-primitive predicates so the "keeps failing"
// fallback branches - which need a genuinely broken environment to reach for real - can be
// exercised directly.
// =============================================================================

namespace
{
// Function-local static (lazily initialized on first call, inside a test body where gtest can
// still catch an exception) rather than a namespace-scope global, which clang-tidy flags as
// unsafe static storage duration: std::function's converting constructor is not guaranteed
// noexcept, and an exception thrown during static initialization before main() cannot be caught.
const PathProviderCurrentPathFn& kFailingCurrentPath()
{
    static const PathProviderCurrentPathFn fn = [](std::error_code& ec)
    {
        ec = std::make_error_code(std::errc::io_error);
        return std::filesystem::path{};
    };
    return fn;
}
} // namespace

TEST(ResolveExecutableDirTest, SucceedsOnFirstCallReturnsParentPath)
{
    const std::filesystem::path fakeExe = std::filesystem::temp_directory_path() / "sub" / "TaskSmack.exe";
    const auto fakeExeStr = fakeExe.wstring();

    const auto result = resolveExecutableDir(
        [&fakeExeStr](wchar_t* buffer, std::uint32_t size) -> std::uint32_t
        {
            if (fakeExeStr.size() >= size)
            {
                return size; // simulate truncation, matching real GetModuleFileNameW semantics
            }
            std::ranges::copy(fakeExeStr, buffer);
            return static_cast<std::uint32_t>(fakeExeStr.size());
        },
        kFailingCurrentPath());

    EXPECT_EQ(result, fakeExe.parent_path());
}

TEST(ResolveExecutableDirTest, ImmediateFailureFallsBackToCurrentPath)
{
    std::filesystem::path cwd = std::filesystem::temp_directory_path();
    const auto result = resolveExecutableDir([](wchar_t*, std::uint32_t) -> std::uint32_t { return 0; },
                                             [&cwd](std::error_code& ec)
                                             {
                                                 ec.clear();
                                                 return cwd;
                                             });

    EXPECT_EQ(result, cwd);
}

TEST(ResolveExecutableDirTest, GrowsBufferOnTruncationThenSucceeds)
{
    // Path longer than the initial 260-char buffer, forcing at least one growth iteration.
    const std::wstring longSegment(300, L'a');
    const std::filesystem::path fakeExe = std::filesystem::temp_directory_path() / longSegment / "TaskSmack.exe";
    const auto fakeExeStr = fakeExe.wstring();
    int callCount = 0;

    const auto result = resolveExecutableDir(
        [&fakeExeStr, &callCount](wchar_t* buffer, std::uint32_t size) -> std::uint32_t
        {
            ++callCount;
            if (fakeExeStr.size() >= size)
            {
                return size; // too small; caller must grow and retry
            }
            std::ranges::copy(fakeExeStr, buffer);
            return static_cast<std::uint32_t>(fakeExeStr.size());
        },
        kFailingCurrentPath());

    EXPECT_GT(callCount, 1) << "the 260-char initial buffer must have been too small for this path";
    EXPECT_EQ(result, fakeExe.parent_path());
}

TEST(ResolveExecutableDirTest, NeverFitsEvenAtLongPathLimitFallsBackToCurrentPath)
{
    std::filesystem::path cwd = std::filesystem::temp_directory_path();
    // Always report truncation, regardless of buffer size, forcing growth all the way to the
    // 32767-char long-path limit before giving up.
    const auto result = resolveExecutableDir([](wchar_t*, std::uint32_t size) -> std::uint32_t { return size; },
                                             [&cwd](std::error_code& ec)
                                             {
                                                 ec.clear();
                                                 return cwd;
                                             });

    EXPECT_EQ(result, cwd);
}

TEST(ResolveExecutableDirTest, BothPrimitivesFailingReturnsEmptyPath)
{
    const auto result = resolveExecutableDir([](wchar_t*, std::uint32_t) -> std::uint32_t { return 0; }, kFailingCurrentPath());

    EXPECT_TRUE(result.empty());
}

TEST(ResolveUserConfigDirTest, AppDataPresentAppendsTaskSmack)
{
    const auto result =
        resolveUserConfigDir([]() -> std::optional<std::string> { return R"(C:\Users\test\AppData\Roaming)"; }, kFailingCurrentPath());

    EXPECT_EQ(result, std::filesystem::path(R"(C:\Users\test\AppData\Roaming)") / "TaskSmack");
}

TEST(ResolveUserConfigDirTest, AppDataMissingFallsBackToCurrentPath)
{
    std::filesystem::path cwd = std::filesystem::temp_directory_path();
    const auto result = resolveUserConfigDir([]() -> std::optional<std::string> { return std::nullopt; },
                                             [&cwd](std::error_code& ec)
                                             {
                                                 ec.clear();
                                                 return cwd;
                                             });

    EXPECT_EQ(result, cwd);
}

TEST(ResolveUserConfigDirTest, AppDataEmptyStringFallsBackToCurrentPath)
{
    std::filesystem::path cwd = std::filesystem::temp_directory_path();
    const auto result = resolveUserConfigDir([]() -> std::optional<std::string> { return std::string{}; },
                                             [&cwd](std::error_code& ec)
                                             {
                                                 ec.clear();
                                                 return cwd;
                                             });

    EXPECT_EQ(result, cwd);
}

TEST(ResolveUserConfigDirTest, BothPrimitivesFailingReturnsEmptyPath)
{
    const auto result = resolveUserConfigDir([]() -> std::optional<std::string> { return std::nullopt; }, kFailingCurrentPath());

    EXPECT_TRUE(result.empty());
}

} // namespace
} // namespace Platform
