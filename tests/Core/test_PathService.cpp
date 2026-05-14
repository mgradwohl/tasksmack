/// @file test_PathService.cpp
/// @brief Tests for Core::PathService
///
/// Tests cover:
/// - executableDir() returns a non-empty, absolute path
/// - userConfigDir() returns a non-empty, absolute path
/// - Both accessors return stable values across repeated calls
/// - PathService is non-copyable and non-movable
///
/// @note The is_absolute() assertions assume a functioning OS (one where at
///       least std::filesystem::absolute() or std::filesystem::current_path()
///       succeeds). On a severely broken system where both calls fail
///       simultaneously, PathService logs a warning and may return a relative
///       path. That degenerate case is not tested here because the application
///       cannot function at all in that environment.

#include "Core/PathService.h"
#include "Platform/IPathProvider.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
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

// =============================================================================
// Injection constructor (testability)
// FakePathProvider is outside the anonymous namespace so std::make_unique works.
// =============================================================================

/// Minimal fake provider: returns caller-specified paths.
class FakePathProvider final : public Platform::IPathProvider
{
  public:
    FakePathProvider(std::filesystem::path execDir, std::filesystem::path configDir)
        : m_ExecDir(std::move(execDir)), m_ConfigDir(std::move(configDir))
    {}

    [[nodiscard]] std::filesystem::path getExecutableDir() const override
    {
        return m_ExecDir;
    }
    [[nodiscard]] std::filesystem::path getUserConfigDir() const override
    {
        return m_ConfigDir;
    }

  private:
    std::filesystem::path m_ExecDir;
    std::filesystem::path m_ConfigDir;
};

namespace Core
{
namespace
{

TEST(PathServiceTest, InjectionConstructorReturnsProvidedPaths)
{
    // Both inputs are already absolute — toAbsolute() returns them as-is (normalized).
    auto provider = std::make_unique<FakePathProvider>("/usr/local/bin", "/home/user/.config/app");
    Core::PathService svc(std::move(provider));

    EXPECT_EQ(svc.executableDir(), std::filesystem::path("/usr/local/bin"));
    EXPECT_EQ(svc.userConfigDir(), std::filesystem::path("/home/user/.config/app"));
}

TEST(PathServiceTest, InjectionConstructorNormalizesRedundantComponents)
{
    // Paths with '.' and '..' components should be normalized by lexically_normal().
    const std::filesystem::path rawExec = std::filesystem::path("/usr") / "local" / "." / "extra" / ".." / "bin";
    const std::filesystem::path rawConfig = std::filesystem::path("/home") / "user" / ".config" / "." / "app";
    auto provider = std::make_unique<FakePathProvider>(rawExec, rawConfig);
    Core::PathService svc(std::move(provider));

    EXPECT_EQ(svc.executableDir(), std::filesystem::path("/usr/local/bin"));
    EXPECT_EQ(svc.userConfigDir(), std::filesystem::path("/home/user/.config/app"));
    EXPECT_TRUE(svc.executableDir().is_absolute());
}

TEST(PathServiceTest, InjectionConstructorWithRelativePathBecomesAbsolute)
{
    // A relative path is made absolute by toAbsolute() via std::filesystem::absolute().
    auto provider = std::make_unique<FakePathProvider>("relative/bin", "relative/config");
    Core::PathService svc(std::move(provider));

    EXPECT_TRUE(svc.executableDir().is_absolute());
    EXPECT_TRUE(svc.userConfigDir().is_absolute());
    EXPECT_FALSE(svc.executableDir().empty());
    EXPECT_FALSE(svc.userConfigDir().empty());
}

} // namespace
} // namespace Core
