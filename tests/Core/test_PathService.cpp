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
#include "Core/PathServiceMath.h"
#include "Platform/IPathProvider.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>
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

// =============================================================================
// resolveAbsolutePath(): the toAbsolute() fallback logic, with injected
// absolute()/current_path() predicates so the failure branches - which need a
// genuinely broken filesystem to reach for real - can be exercised directly.
// =============================================================================

// Function-local statics (lazily initialized on first call, inside a test body where gtest can
// still catch an exception) rather than namespace-scope globals, which clang-tidy flags as unsafe
// static storage duration: std::function's converting constructor is not guaranteed noexcept, and
// an exception thrown during static initialization before main() cannot be caught.
const AbsolutePathFn& kFailingAbsolute()
{
    static const AbsolutePathFn fn = [](const std::filesystem::path&, std::error_code& ec)
    {
        ec = std::make_error_code(std::errc::io_error);
        return std::filesystem::path{};
    };
    return fn;
}
const CurrentPathFn& kFailingCurrentPath()
{
    static const CurrentPathFn fn = [](std::error_code& ec)
    {
        ec = std::make_error_code(std::errc::io_error);
        return std::filesystem::path{};
    };
    return fn;
}

TEST(ResolveAbsolutePathTest, SucceedingAbsoluteFnReturnsNormalizedResult)
{
    const std::filesystem::path raw = "/tmp/./a/../b";
    const AbsolutePathFn succeedingAbsolute = [](const std::filesystem::path& p, std::error_code& ec)
    {
        ec.clear();
        return p;
    };

    const auto result = resolveAbsolutePath(raw, succeedingAbsolute, kFailingCurrentPath());

    EXPECT_EQ(result, raw.lexically_normal());
}

TEST(ResolveAbsolutePathTest, FailingAbsoluteWithRelativeRawComposesWithCurrentPath)
{
    const std::filesystem::path raw = "sub/dir";
    std::filesystem::path cwd = std::filesystem::temp_directory_path();
    const CurrentPathFn succeedingCurrentPath = [&cwd](std::error_code& ec)
    {
        ec.clear();
        return cwd;
    };

    const auto result = resolveAbsolutePath(raw, kFailingAbsolute(), succeedingCurrentPath);

    EXPECT_EQ(result, (cwd / raw).lexically_normal());
}

TEST(ResolveAbsolutePathTest, FailingAbsoluteWithAbsoluteRawSkipsCurrentPathFallback)
{
    // raw is already absolute, so the relative-composition branch must not be taken even
    // though current_path() would also fail here - the function should fall straight through
    // to the last-resort lexically_normal(raw) return.
    const std::filesystem::path raw = std::filesystem::temp_directory_path() / "already_absolute";

    const auto result = resolveAbsolutePath(raw, kFailingAbsolute(), kFailingCurrentPath());

    EXPECT_EQ(result, raw.lexically_normal());
}

TEST(ResolveAbsolutePathTest, BothPrimitivesFailingWithRelativeRawReturnsLexicallyNormalizedRaw)
{
    const std::filesystem::path raw = "relative/only";

    const auto result = resolveAbsolutePath(raw, kFailingAbsolute(), kFailingCurrentPath());

    EXPECT_EQ(result, raw.lexically_normal());
    EXPECT_TRUE(result.is_relative()) << "documents the degenerate case: the contract is relaxed, not upheld, "
                                         "when both filesystem primitives fail";
}

TEST(ResolveAbsolutePathTest, BothPrimitivesFailingWithEmptyRawDoesNotThrow)
{
    // Exercises the raw.empty() branch of the best-effort logging path.
    EXPECT_NO_THROW({
        const auto result = resolveAbsolutePath(std::filesystem::path{}, kFailingAbsolute(), kFailingCurrentPath());
        EXPECT_TRUE(result.empty());
    });
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
    // Use temp_directory_path() as the absolute base so the test is
    // platform-neutral (avoids POSIX-only roots like /usr that are invalid on
    // Windows).
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    const std::filesystem::path execDir = base / "tasksmack_test_exec";
    const std::filesystem::path configDir = base / "tasksmack_test_config";

    auto provider = std::make_unique<FakePathProvider>(execDir, configDir);
    Core::PathService svc(std::move(provider));

    // toAbsolute() calls lexically_normal() on already-absolute paths, which
    // leaves them unchanged when there are no redundant components.
    EXPECT_EQ(svc.executableDir(), execDir.lexically_normal());
    EXPECT_EQ(svc.userConfigDir(), configDir.lexically_normal());
    EXPECT_TRUE(svc.executableDir().is_absolute());
    EXPECT_TRUE(svc.userConfigDir().is_absolute());
}

TEST(PathServiceTest, InjectionConstructorNormalizesRedundantComponents)
{
    // Build an absolute base from temp_directory_path() and insert redundant
    // '.' and '..' components to verify lexically_normal() is applied.
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    const std::filesystem::path rawExec = base / "a" / "." / "extra" / ".." / "bin";
    const std::filesystem::path rawConfig = base / "cfg" / "." / "app";
    const std::filesystem::path expectedExec = (base / "a" / "bin").lexically_normal();
    const std::filesystem::path expectedConfig = (base / "cfg" / "app").lexically_normal();

    auto provider = std::make_unique<FakePathProvider>(rawExec, rawConfig);
    Core::PathService svc(std::move(provider));

    EXPECT_EQ(svc.executableDir(), expectedExec);
    EXPECT_EQ(svc.userConfigDir(), expectedConfig);
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

TEST(PathServiceTest, InjectionConstructorWithNullProviderThrows)
{
    // Passing nullptr must throw std::invalid_argument rather than crashing.
    EXPECT_THROW(Core::PathService(nullptr), std::invalid_argument);
}

} // namespace
} // namespace Core
