/// @file test_PathService.cpp
/// @brief Tests for Core::PathService
///
/// Tests cover:
/// - executableDir() returns a non-empty, absolute path
/// - userConfigDir() returns a non-empty, absolute path
/// - Both accessors return stable values across repeated calls
/// - PathService is non-copyable and non-movable

#include "Core/PathService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <type_traits>

namespace Core
{
namespace
{

// =============================================================================
// Construction and Traits
// =============================================================================

TEST(PathServiceTest, IsNotCopyable)
{
    EXPECT_FALSE(std::is_copy_constructible_v<PathService>);
    EXPECT_FALSE(std::is_copy_assignable_v<PathService>);
}

TEST(PathServiceTest, IsNotMovable)
{
    EXPECT_FALSE(std::is_move_constructible_v<PathService>);
    EXPECT_FALSE(std::is_move_assignable_v<PathService>);
}

// =============================================================================
// executableDir()
// =============================================================================

TEST(PathServiceTest, ExecutableDirIsNonEmpty)
{
    PathService svc;
    EXPECT_FALSE(svc.executableDir().empty());
}

TEST(PathServiceTest, ExecutableDirIsAbsolute)
{
    PathService svc;
    EXPECT_TRUE(svc.executableDir().is_absolute());
}

TEST(PathServiceTest, ExecutableDirIsStable)
{
    PathService svc;
    const std::filesystem::path first = svc.executableDir();
    const std::filesystem::path second = svc.executableDir();
    EXPECT_EQ(first, second);
}

// =============================================================================
// userConfigDir()
// =============================================================================

TEST(PathServiceTest, UserConfigDirIsNonEmpty)
{
    PathService svc;
    EXPECT_FALSE(svc.userConfigDir().empty());
}

TEST(PathServiceTest, UserConfigDirIsAbsolute)
{
    PathService svc;
    EXPECT_TRUE(svc.userConfigDir().is_absolute());
}

TEST(PathServiceTest, UserConfigDirIsStable)
{
    PathService svc;
    const std::filesystem::path first = svc.userConfigDir();
    const std::filesystem::path second = svc.userConfigDir();
    EXPECT_EQ(first, second);
}

// =============================================================================
// Independence between instances
// =============================================================================

TEST(PathServiceTest, TwoInstancesReturnSamePaths)
{
    PathService svc1;
    PathService svc2;
    EXPECT_EQ(svc1.executableDir(), svc2.executableDir());
    EXPECT_EQ(svc1.userConfigDir(), svc2.userConfigDir());
}

} // namespace
} // namespace Core
