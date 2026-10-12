/// @file test_PlatformOpen.cpp
/// @brief App::PlatformOpen on Linux (#1395): openWithSystemHandler() hands its target to xdg-open,
/// byte for byte, from a detached grandchild. A fake xdg-open first on PATH records what it was given.

#include "App/PlatformOpen.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include <stdlib.h> // NOLINT(modernize-deprecated-headers) - POSIX setenv()/unsetenv(), which cstdlib does not declare
#include <unistd.h>

namespace App::PlatformOpen
{
namespace
{

/// A temporary directory holding a fake xdg-open that writes its one argument to `opened.txt`, put
/// first on PATH for the test's lifetime.
class PlatformOpenTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = std::filesystem::temp_directory_path() / ("tasksmack_platform_open_" + std::to_string(::getpid()));
        std::filesystem::create_directories(m_Dir);
        const std::filesystem::path script = m_Dir / "xdg-open";
        {
            std::ofstream out(script);
            out << "#!/bin/sh\nprintf '%s' \"$1\" > '" << (m_Dir / "opened.tmp").string() << "'\nmv '" << (m_Dir / "opened.tmp").string()
                << "' '" << (m_Dir / "opened.txt").string() << "'\n";
        }
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);

        if (const char* path = std::getenv("PATH"); path != nullptr) // NOLINT(concurrency-mt-unsafe) - single-threaded test setup
        {
            m_SavedPath = path;
        }
        const std::string path = m_Dir.string() + ":" + m_SavedPath.value_or("/usr/bin:/bin");
        ASSERT_EQ(::setenv("PATH", path.c_str(), 1), 0); // NOLINT(concurrency-mt-unsafe) - as above
    }

    void TearDown() override
    {
        if (m_SavedPath.has_value())
        {
            ::setenv("PATH", m_SavedPath->c_str(), 1); // NOLINT(concurrency-mt-unsafe) - single-threaded teardown
        }
        else
        {
            ::unsetenv("PATH"); // NOLINT(concurrency-mt-unsafe) - as above
        }
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    /// What the fake xdg-open was given, once it has run (it runs detached, so it is waited for).
    [[nodiscard]] std::optional<std::string> opened() const
    {
        const std::filesystem::path file = m_Dir / "opened.txt";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!std::filesystem::exists(file) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            return std::nullopt;
        }
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::filesystem::path m_Dir;

  private:
    std::optional<std::string> m_SavedPath;
};

TEST_F(PlatformOpenTest, AUrlIsHandedToXdgOpen)
{
    EXPECT_TRUE(openWithSystemHandler(std::string_view{"https://github.com/mgradwohl/tasksmack/issues"}));
    EXPECT_EQ(opened(), "https://github.com/mgradwohl/tasksmack/issues");
}

TEST_F(PlatformOpenTest, APathIsHandedOverByteForByte)
{
    // Non-ASCII and a space: the path's own bytes, no conversion on the way (#1648).
    const std::filesystem::path path = m_Dir / "R\xC3\xA9sum\xC3\xA9 notes.txt";
    EXPECT_TRUE(openWithSystemHandler(path));
    EXPECT_EQ(opened(), path.string());
}

TEST_F(PlatformOpenTest, NoXdgOpenStillReapsTheLauncher)
{
    // Success means only that the launcher was forked and reaped: a missing xdg-open fails silently in
    // the detached grandchild (see PlatformOpen.h), and leaves no zombie behind.
    ASSERT_EQ(::setenv("PATH", "/nonexistent-tasksmack-path", 1), 0); // NOLINT(concurrency-mt-unsafe) - single-threaded test
    EXPECT_TRUE(openWithSystemHandler(std::string_view{"https://example.invalid/"}));
}

TEST(PlatformOpenTargetTest, LinuxTargetsAreTheSameBytes)
{
    EXPECT_EQ(nativeTargetFromUtf8("caf\xC3\xA9"), "caf\xC3\xA9");
    EXPECT_EQ(nativeTargetFromPath(std::filesystem::path("/tmp/a b")), "/tmp/a b");
}

} // namespace
} // namespace App::PlatformOpen
