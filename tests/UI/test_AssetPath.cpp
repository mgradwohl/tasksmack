#include "Core/Application.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "UI/AssetPath.h"
#include "version.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace UI
{
namespace
{

// Mirrors the hasDisplay() helper in test_Application.cpp/test_Window.cpp (each test file
// keeps its own copy rather than sharing one, per this repo's existing convention).
bool hasDisplay()
{
#ifdef _WIN32
    char* ciEnv = nullptr;
    std::size_t len = 0;
    _dupenv_s(&ciEnv, &len, "CI");
    const bool isCI = (ciEnv != nullptr && std::string_view(ciEnv) == "true");
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) - _dupenv_s allocates with malloc; must free with free()
    free(ciEnv);
    if (isCI)
    {
        return false;
    }
    return true;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    if ((display != nullptr && display[0] != '\0') || (waylandDisplay != nullptr && waylandDisplay[0] != '\0'))
    {
        if (TestSupport::probeGLCapability())
        {
            return true;
        }
    }
    return TestSupport::tryEnableOffscreenVideoDriver();
#endif
}

// ========== Candidate priority ==========

TEST(AssetPathTest, ReturnsBinAssetsWhenAllCandidatesExist)
{
    // All candidates match: should return candidate 1 (bin/assets)
    const std::filesystem::path exeDir = "/fake/exe";
    const auto result = selectAssetsDir(exeDir, [](const std::filesystem::path& /*p*/) { return true; });
    EXPECT_EQ(result, exeDir / "bin" / "assets");
}

TEST(AssetPathTest, ReturnsBinAssetsWhenExeDirIsBuildRoot)
{
    // Root executable layout: only <exeDir>/bin/assets exists.
    const std::filesystem::path exeDir = "/fake/build-root";
    const std::filesystem::path expected = exeDir / "bin" / "assets";
    const auto result = selectAssetsDir(exeDir, [&](const std::filesystem::path& p) { return p == expected; });
    EXPECT_EQ(result, expected);
}

TEST(AssetPathTest, SkipsFirstAndReturnsSecondCandidateOnMatch)
{
    // Only candidate 2 (portable/build-dir assets) matches
    const std::filesystem::path exeDir = "/fake/prefix";
    const std::filesystem::path expected = exeDir / "assets";
    int callCount = 0;
    const auto result = selectAssetsDir(exeDir,
                                        [&](const std::filesystem::path& p)
                                        {
                                            ++callCount;
                                            return p == expected;
                                        });
    EXPECT_EQ(result, expected);
    EXPECT_EQ(callCount, 2); // candidate 1 was probed and missed
}

TEST(AssetPathTest, ReturnsThirdCandidateForFHSPrefixLayout)
{
    // Only candidate 3 (FHS, binary at prefix root) matches
    const std::filesystem::path exeDir = "/fake/prefix";
    const std::filesystem::path expected = exeDir / TASKSMACK_INSTALL_DATADIR / TASKSMACK_PROJECT_NAME_LOWER / "assets";
    int callCount = 0;
    const auto result = selectAssetsDir(exeDir,
                                        [&](const std::filesystem::path& p)
                                        {
                                            ++callCount;
                                            return p == expected;
                                        });
    EXPECT_EQ(result, expected);
    EXPECT_EQ(callCount, 3); // candidates 1 and 2 were probed and missed
}

TEST(AssetPathTest, ReturnsFourthCandidateForFHSBinLayout)
{
    // Only candidate 4 (FHS, binary in bin/) matches
    // exeDir = /usr/bin → candidate 4 = /usr/bin/../share/<app>/assets → /usr/share/<app>/assets
    const std::filesystem::path exeDir = "/usr/bin";
    const std::filesystem::path expected =
        std::filesystem::path("/usr") / TASKSMACK_INSTALL_DATADIR / TASKSMACK_PROJECT_NAME_LOWER / "assets";
    int callCount = 0;
    const auto result = selectAssetsDir(exeDir,
                                        [&](const std::filesystem::path& p)
                                        {
                                            ++callCount;
                                            return p == expected;
                                        });
    EXPECT_EQ(result, expected);
    EXPECT_EQ(callCount, 4); // candidates 1, 2 and 3 were probed and missed
}

// ========== Fallback behavior ==========

TEST(AssetPathTest, FallsBackToFirstCandidateWhenNoneExist)
{
    // No candidate matches: should return <exeDir>/assets fallback
    const std::filesystem::path exeDir = "/nonexistent/exe";
    const auto result = selectAssetsDir(exeDir, [](const std::filesystem::path& /*p*/) { return false; });
    EXPECT_EQ(result, exeDir / "assets");
}

// ========== findAssetsDir() (real Application, real filesystem) ==========

TEST(AssetPathTest, FindAssetsDirIsStableAcrossCalls)
{
    // findAssetsDir() caches its result in a function-local static (computed once per
    // process), so this is the only way to cover its body and executableDir()'s wrapper
    // around Core::Application::get() -- both need a live Application instance, which
    // selectAssetsDir()'s injectable-predicate tests above deliberately avoid depending on.
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "FindAssetsDirTest";

    // Isolate construction in its own try/catch: only a construction failure (SDL/GL
    // unavailable) should skip this test. An exception from findAssetsDir() itself is a
    // real regression and must fail the test, not be silently swallowed as a skip.
    std::optional<Core::Application> app;
    try
    {
        app.emplace(spec);
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
    }

    const auto first = findAssetsDir();
    const auto second = findAssetsDir();
    EXPECT_EQ(first, second);

    // Comparing against selectAssetsDir() run directly against the live app's real
    // executable directory and the real filesystem (the same "fonts" subdirectory probe
    // findAssetsDir() uses internally) proves the cached result actually resolves from
    // the live application, not just that the cache is internally consistent.
    const auto expected = selectAssetsDir(Core::Application::get().paths().executableDir(),
                                          [](const std::filesystem::path& p)
                                          {
                                              std::error_code ec;
                                              return std::filesystem::exists(p / "fonts", ec);
                                          });
    EXPECT_EQ(first, expected);
}

} // namespace
} // namespace UI
