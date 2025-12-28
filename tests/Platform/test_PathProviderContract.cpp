/// @file test_PathProviderContract.cpp
/// @brief Contract tests for IPathProvider interface
///
/// These tests verify that all IPathProvider implementations adhere to the same contract.

#include "Platform/Factory.h"
#include "Platform/IPathProvider.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace Platform
{
namespace
{

// =============================================================================
// Factory Tests
// =============================================================================

TEST(PathProviderContractTest, FactoryReturnsNonNull)
{
    auto provider = makePathProvider();
    EXPECT_NE(provider, nullptr);
}

TEST(PathProviderContractTest, FactoryReturnsValidImplementation)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    // Should be able to call methods without throwing
    EXPECT_NO_THROW({
        [[maybe_unused]] auto exeDir = provider->getExecutableDir();
        [[maybe_unused]] auto configDir = provider->getUserConfigDir();
    });
}

// =============================================================================
// Interface Contract Tests
// =============================================================================

TEST(PathProviderContractTest, GetExecutableDirNeverReturnsEmpty)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto dir = provider->getExecutableDir();
    EXPECT_FALSE(dir.empty()) << "getExecutableDir() must never return an empty path";
}

TEST(PathProviderContractTest, GetExecutableDirReturnsAbsolutePath)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto dir = provider->getExecutableDir();
    EXPECT_TRUE(dir.is_absolute()) << "getExecutableDir() must return an absolute path";
}

TEST(PathProviderContractTest, GetExecutableDirReturnsDirectory)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto dir = provider->getExecutableDir();

    // The directory should exist (it contains the running executable)
    EXPECT_TRUE(std::filesystem::exists(dir)) << "Executable directory must exist: " << dir.string();

    if (std::filesystem::exists(dir))
    {
        EXPECT_TRUE(std::filesystem::is_directory(dir)) << "getExecutableDir() must return a directory, not a file";
    }
}

TEST(PathProviderContractTest, GetUserConfigDirNeverReturnsEmpty)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto dir = provider->getUserConfigDir();
    EXPECT_FALSE(dir.empty()) << "getUserConfigDir() must never return an empty path";
}

TEST(PathProviderContractTest, GetUserConfigDirReturnsAbsolutePath)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto dir = provider->getUserConfigDir();
    EXPECT_TRUE(dir.is_absolute()) << "getUserConfigDir() must return an absolute path";
}

TEST(PathProviderContractTest, GetUserConfigDirEndsWithAppName)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto dir = provider->getUserConfigDir();
    const auto dirname = dir.filename().string();

    // Should end with application name (case-insensitive check)
    std::string lowerDirname = dirname;
    for (char& c : lowerDirname)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    EXPECT_TRUE(lowerDirname == "tasksmack") << "Config directory should end with 'tasksmack', got: " << dirname;
}

// =============================================================================
// Consistency Tests
// =============================================================================

TEST(PathProviderContractTest, ConsecutiveCallsReturnSameExecutableDir)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto dir1 = provider->getExecutableDir();
    const auto dir2 = provider->getExecutableDir();
    const auto dir3 = provider->getExecutableDir();

    EXPECT_EQ(dir1, dir2) << "Consecutive calls must return identical paths";
    EXPECT_EQ(dir2, dir3) << "Consecutive calls must return identical paths";
}

TEST(PathProviderContractTest, ConsecutiveCallsReturnSameConfigDir)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto dir1 = provider->getUserConfigDir();
    const auto dir2 = provider->getUserConfigDir();
    const auto dir3 = provider->getUserConfigDir();

    EXPECT_EQ(dir1, dir2) << "Consecutive calls must return identical paths";
    EXPECT_EQ(dir2, dir3) << "Consecutive calls must return identical paths";
}

TEST(PathProviderContractTest, MultipleInstancesReturnSamePaths)
{
    auto provider1 = makePathProvider();
    auto provider2 = makePathProvider();
    ASSERT_NE(provider1, nullptr);
    ASSERT_NE(provider2, nullptr);

    const auto exeDir1 = provider1->getExecutableDir();
    const auto exeDir2 = provider2->getExecutableDir();
    EXPECT_EQ(exeDir1, exeDir2) << "Different instances must return the same executable directory";

    const auto configDir1 = provider1->getUserConfigDir();
    const auto configDir2 = provider2->getUserConfigDir();
    EXPECT_EQ(configDir1, configDir2) << "Different instances must return the same config directory";
}

// =============================================================================
// Safety Tests
// =============================================================================

TEST(PathProviderContractTest, PathsDoNotContainInvalidCharacters)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto exeDir = provider->getExecutableDir();
    const auto configDir = provider->getUserConfigDir();

    // Paths should not contain null bytes
    const auto exeStr = exeDir.string();
    const auto configStr = configDir.string();

    EXPECT_EQ(exeStr.find('\0'), std::string::npos) << "Path should not contain null bytes";
    EXPECT_EQ(configStr.find('\0'), std::string::npos) << "Path should not contain null bytes";
}

TEST(PathProviderContractTest, PathsAreNotJustCurrentDirectory)
{
    auto provider = makePathProvider();
    ASSERT_NE(provider, nullptr);

    const auto exeDir = provider->getExecutableDir();
    const auto configDir = provider->getUserConfigDir();

    // Neither should be just "." (unless we're actually running from current dir)
    // This tests that we get meaningful paths, not just lazy fallbacks
    EXPECT_NE(exeDir.string(), ".");
    EXPECT_NE(configDir.string(), ".");
}

} // namespace
} // namespace Platform
