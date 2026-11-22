/// @file test_ReadWholeFile.cpp
/// @brief Core::readWholeFile (#4315, #4652): the bytes of a file opened by its path, and nullopt, never a part of it as
/// if whole, when it can't be opened or a read fails (a folder's EISDIR stands in for EIO or ESTALE here).

#include "Core/ReadWholeFile.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <system_error>

#ifdef _WIN32
#include "Platform/Windows/WindowsHandles.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on
#endif

#ifdef __linux__
#include "Platform/Linux/UserPermissionsOnly.h"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace Core
{
namespace
{

/// A fresh folder of this test's own under the temp folder, removed afterwards (portable: ScopedTempDir is Linux's).
struct TestFolder
{
    explicit TestFolder(const std::string& name)
        : path(std::filesystem::temp_directory_path() / (name + "-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                                         "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name()))
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path);
    }
    // NOLINTNEXTLINE(bugprone-exception-escape) - only MSVC's debug path copy (its container proxy) can throw; a test failure
    ~TestFolder()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TestFolder(const TestFolder&) = delete;
    TestFolder& operator=(const TestFolder&) = delete;
    TestFolder(TestFolder&&) = delete;
    TestFolder& operator=(TestFolder&&) = delete;
    std::filesystem::path path;
};

TEST(ReadWholeFileTest, ReadsEveryByteExactly)
{
    const TestFolder dir("ts_read_whole_file");
    const std::filesystem::path path = dir.path / "config.toml";
    std::string bytes = "[ui]\r\nname = \"a\"\n";
    bytes.push_back('\0');
    bytes.append(40000, 'x'); // more than one read's worth
    {
        std::ofstream out(path, std::ios::binary);
        out << bytes;
    }
    EXPECT_EQ(readWholeFile(path), std::optional<std::string>(bytes));
}

TEST(ReadWholeFileTest, AnEmptyFileIsEmptyAndAMissingOneIsNothing)
{
    const TestFolder dir("ts_read_whole_file_empty");
    {
        const std::ofstream out(dir.path / "empty.toml");
    }
    EXPECT_EQ(readWholeFile(dir.path / "empty.toml"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(readWholeFile(dir.path / "missing.toml"), std::nullopt);
}

// #4652: a folder opens on POSIX and its read fails (EISDIR), as a read from a failing disk or an NFS home fails with EIO
// or ESTALE; std::ifstream's buffer took that for the end of the file and handed back "" as the whole of it.
TEST(ReadWholeFileTest, AReadThatFailsIsNothingNotAnEmptyFile)
{
    const TestFolder dir("ts_read_whole_file_dir");
    std::filesystem::create_directories(dir.path / "config.toml");
    EXPECT_EQ(readWholeFile(dir.path / "config.toml"), std::nullopt);
}

#ifdef _WIN32
// #4745: an editor or a sync tool holds config.toml open for writing, sharing read and write. _wfopen_s opened the file
// unshared (read sharing only), so the read failed with a sharing violation and the load fell back to defaults.
TEST(ReadWholeFileTest, AFileAnotherProgramHoldsOpenForWritingIsStillRead)
{
    const TestFolder dir("ts_read_whole_file_shared");
    const std::filesystem::path path = dir.path / "config.toml";
    const std::string bytes = "[ui]\nname = \"held\"\n";
    {
        std::ofstream out(path, std::ios::binary);
        out << bytes;
    }
    const Platform::Windows::UniqueHandle writer(CreateFileW(path.c_str(),
                                                             GENERIC_WRITE,
                                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                             nullptr,
                                                             OPEN_EXISTING,
                                                             FILE_ATTRIBUTE_NORMAL,
                                                             nullptr));
    ASSERT_TRUE(writer.valid()) << "CreateFileW failed: " << GetLastError();
    EXPECT_EQ(readWholeFile(path), std::optional<std::string>(bytes));
}
#endif

#ifdef __linux__
// #4802: the user's settings and themes are opened with the user's own permissions. Without capabilities (every test
// run but a setcap'd binary's) the guard changes nothing.
TEST(ReadWholeFileTest, WithoutCapabilitiesTheUserPermissionsGuardIsANoOp)
{
    const Platform::UserPermissionsOnly guard;
    EXPECT_TRUE(guard.ok());
    if (::geteuid() != 0 && !guard.lowered())
    {
        SUCCEED() << "no CAP_DAC_READ_SEARCH or CAP_DAC_OVERRIDE to lower";
    }
}

// The proof, when the test binary holds CAP_DAC_READ_SEARCH and isn't root (setcap
// cap_dac_read_search+ep on a copy, as the FAQ's setcap gives TaskSmack): a root-only file opens with the capability,
// not inside the guard, and does again after it. readWholeFile() reads it as the user would: not at all.
TEST(ReadWholeFileTest, ACapabilitiesSessionReadsOnlyWhatTheUserCould)
{
    const char* const rootOnly = "/etc/shadow"; // root:shadow 0640 (or 0600) on every Linux this runs on
    if (::geteuid() == 0 || ::access(rootOnly, R_OK) == 0)
    {
        GTEST_SKIP() << "root, or a user who may read " << rootOnly << " anyway";
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int withCapability = ::open(rootOnly, O_RDONLY | O_CLOEXEC);
    if (withCapability < 0)
    {
        GTEST_SKIP() << "this test binary holds no CAP_DAC_READ_SEARCH (setcap cap_dac_read_search+ep on a copy runs it)";
    }
    ::close(withCapability);
    {
        const Platform::UserPermissionsOnly guard;
        ASSERT_TRUE(guard.ok());
        EXPECT_TRUE(guard.lowered());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
        const int inside = ::open(rootOnly, O_RDONLY | O_CLOEXEC);
        const int error = errno;
        EXPECT_LT(inside, 0) << "the capability was still in effect";
        EXPECT_EQ(error, EACCES);
        if (inside >= 0)
        {
            ::close(inside);
        }
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int after = ::open(rootOnly, O_RDONLY | O_CLOEXEC);
    EXPECT_GE(after, 0) << "the capability came back for the rest of the thread";
    if (after >= 0)
    {
        ::close(after);
    }
    EXPECT_FALSE(readWholeFile(rootOnly).has_value()) << "settings are read with the user's permissions";
}
#endif

} // namespace
} // namespace Core
