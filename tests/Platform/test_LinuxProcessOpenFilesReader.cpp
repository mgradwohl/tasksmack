/// @file test_LinuxProcessOpenFilesReader.cpp
/// @brief Platform::LinuxProcessOpenFilesReader (#183): a synthetic /proc tree (fd symlinks with any
/// target text, fdinfo files) read through the procRoot seam -- typing, flags, sorting, the identity
/// check, a missing process, an fd/ directory that cannot be opened -- and the real /proc/self.

#include "Domain/HandleSearch.h"
#include "Domain/PublicationSlot.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/Linux/LinuxProcessModulesReader.h"
#include "Platform/Linux/LinuxProcessOpenFilesReader.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace Platform
{
namespace
{

constexpr ProcessTarget TARGET{.pid = 100, .startTimeTicks = 4242};

class LinuxProcessOpenFilesReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::filesystem::create_directories(m_Proc.path / "100" / "fd");
        std::filesystem::create_directories(m_Proc.path / "100" / "fdinfo");
        std::ofstream(m_Proc.path / "100" / "stat") << "100 (server) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 1 0 4242 1000 50\n";
        link("10", "/var/log/server log.txt (deleted)", "pos:\t0\nflags:\t02102001\n");
        link("2", "/dev/pts/1", "pos:\t0\nflags:\t02\n");
        link("3", "socket:[9001]", "pos:\t0\nflags:\t02000002\n");
    }

    /// fd/<name> pointing at @p target (a symlink may hold any text), with fdinfo/<name> holding @p fdinfo.
    void link(const char* name, const char* target, std::string_view fdinfo) const
    {
        std::filesystem::create_symlink(target, m_Proc.path / "100" / "fd" / name);
        std::ofstream(m_Proc.path / "100" / "fdinfo" / name) << fdinfo;
    }

    TestSupport::ScopedTempDir m_Proc{"ts_test_proc_open_files"};
};

TEST_F(LinuxProcessOpenFilesReaderTest, ReadsTypesFlagsAndSortsByDescriptor)
{
    LinuxProcessOpenFilesReader reader(m_Proc.path.string());
    EXPECT_TRUE(reader.hasOpenFiles());
    const OpenFilesReadResult result = reader.readOpenFiles(TARGET);
    ASSERT_EQ(result.status, OpenFilesReadStatus::Ok);
    ASSERT_EQ(result.files.size(), 3U);
    EXPECT_FALSE(result.hexDescriptors);
    EXPECT_EQ(result.files[0].descriptor, 2U);
    EXPECT_EQ(result.files[0].kind, OpenFileKind::Device);
    EXPECT_EQ(result.files[0].flags, std::optional<std::uint32_t>(02));
    EXPECT_EQ(result.files[1].kind, OpenFileKind::Socket);
    EXPECT_EQ(result.files[1].path, "socket:[9001]");
    EXPECT_EQ(result.files[2].descriptor, 10U);
    EXPECT_EQ(result.files[2].path, "/var/log/server log.txt");
    EXPECT_TRUE(result.files[2].deleted);
}

TEST_F(LinuxProcessOpenFilesReaderTest, RefusesAReusedPidAnUnknownIdentityAndAMissingProcess)
{
    LinuxProcessOpenFilesReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.readOpenFiles({.pid = 100, .startTimeTicks = 9999}).status, OpenFilesReadStatus::ProcessExited);
    EXPECT_EQ(reader.readOpenFiles({.pid = 100, .startTimeTicks = 0}).status, OpenFilesReadStatus::IdentityUnknown);
    EXPECT_EQ(reader.readOpenFiles({.pid = 101, .startTimeTicks = 4242}).status, OpenFilesReadStatus::ProcessExited);
}

TEST_F(LinuxProcessOpenFilesReaderTest, UnreadableFdDirectoryIsPermissionDenied)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root reads any directory";
    }
    std::filesystem::permissions(m_Proc.path / "100" / "fd", std::filesystem::perms::none);
    LinuxProcessOpenFilesReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.readOpenFiles(TARGET).status, OpenFilesReadStatus::PermissionDenied);
    std::filesystem::permissions(m_Proc.path / "100" / "fd", std::filesystem::perms::owner_all);
}

// #1395: an invalid PID is no process; a /proc entry that isn't a directory is a failed read that says why.
TEST_F(LinuxProcessOpenFilesReaderTest, AnInvalidPidOrABrokenProcEntryIsNotRead)
{
    LinuxProcessOpenFilesReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.readOpenFiles({.pid = 0, .startTimeTicks = 4242}).status, OpenFilesReadStatus::ProcessExited);
    EXPECT_EQ(reader.readOpenFiles({.pid = -3, .startTimeTicks = 4242}).status, OpenFilesReadStatus::ProcessExited);

    std::ofstream(m_Proc.path / "102") << "not a directory";
    const OpenFilesReadResult broken = reader.readOpenFiles({.pid = 102, .startTimeTicks = 4242});
    EXPECT_EQ(broken.status, OpenFilesReadStatus::Failed);
    EXPECT_FALSE(broken.detail.empty()); // the system's words for ENOTDIR
}

// #1395: without fdinfo/ the files are still listed, without their flags; an fd/ entry that isn't a link
// (it can't be read as one) is skipped.
TEST_F(LinuxProcessOpenFilesReaderTest, NoFdinfoMeansNoFlagsAndAnUnreadableEntryIsSkipped)
{
    std::filesystem::remove_all(m_Proc.path / "100" / "fdinfo");
    std::ofstream(m_Proc.path / "100" / "fd" / "7") << "a regular file, not a link";
    LinuxProcessOpenFilesReader reader(m_Proc.path.string());
    const OpenFilesReadResult result = reader.readOpenFiles(TARGET);
    ASSERT_EQ(result.status, OpenFilesReadStatus::Ok);
    ASSERT_EQ(result.files.size(), 3U); // 2, 3 and 10; not 7
    for (const OpenFile& file : result.files)
    {
        EXPECT_NE(file.descriptor, 7U);
        EXPECT_FALSE(file.flags.has_value()) << file.descriptor;
    }
}

// #1395: a process with nothing open -- its identity confirmed again -- reads as no open files.
TEST_F(LinuxProcessOpenFilesReaderTest, NothingOpenIsAnEmptyReading)
{
    std::filesystem::remove_all(m_Proc.path / "100" / "fd");
    std::filesystem::create_directories(m_Proc.path / "100" / "fd");
    LinuxProcessOpenFilesReader reader(m_Proc.path.string());
    const OpenFilesReadResult result = reader.readOpenFiles(TARGET);
    EXPECT_EQ(result.status, OpenFilesReadStatus::Ok);
    EXPECT_TRUE(result.files.empty());
    EXPECT_FALSE(result.truncated);
}

// #1395: past MAX_OPEN_FILES descriptors the reading stops and says it was cut short.
TEST_F(LinuxProcessOpenFilesReaderTest, MoreThanTheCapIsTruncated)
{
    for (std::size_t fd = 100; fd < 100 + MAX_OPEN_FILES; ++fd)
    {
        std::filesystem::create_symlink("/dev/null", m_Proc.path / "100" / "fd" / std::to_string(fd));
    }
    LinuxProcessOpenFilesReader reader(m_Proc.path.string());
    const OpenFilesReadResult result = reader.readOpenFiles(TARGET);
    ASSERT_EQ(result.status, OpenFilesReadStatus::Ok);
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(result.files.size(), MAX_OPEN_FILES);
}

TEST(LinuxProcessOpenFilesReaderRealTest, ReadsThisProcess)
{
    std::ifstream stat("/proc/self/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const std::optional<std::uint64_t> startTicks = ProcParsing::parseStatStartTime(line);
    ASSERT_TRUE(startTicks.has_value());

    LinuxProcessOpenFilesReader reader;
    const OpenFilesReadResult result =
        reader.readOpenFiles({.pid = static_cast<std::int32_t>(::getpid()), .startTimeTicks = startTicks.value_or(0)});
    ASSERT_EQ(result.status, OpenFilesReadStatus::Ok);
    // The stat stream above is still open.
    EXPECT_TRUE(
        std::ranges::any_of(result.files, [](const OpenFile& f) { return f.path == "/proc/" + std::to_string(::getpid()) + "/stat"; }));
}

// #3067: Find handles over the real readers finds this process holding a temp file it has open, and the C
// library it has loaded.
TEST(LinuxProcessOpenFilesReaderRealTest, FindHandlesFindsThisProcessHoldingAFile)
{
    std::ifstream stat("/proc/self/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const std::optional<std::uint64_t> startTicks = ProcParsing::parseStatStartTime(line);
    ASSERT_TRUE(startTicks.has_value());
    const TestSupport::ScopedTempDir dir{"ts_test_find_handles"};
    const std::filesystem::path held = dir.path / "Locked-By-FindHandles-3067.txt";
    std::ofstream file(held);
    ASSERT_TRUE(file.is_open());

    LinuxProcessOpenFilesReader openFiles;
    LinuxProcessModulesReader modules;
    const std::vector<Domain::HandleSearch::SearchProcess> self{
        {.target = {.pid = static_cast<std::int32_t>(::getpid()), .startTimeTicks = startTicks.value_or(0)}, .name = "TaskSmackTests"}};
    Domain::PublicationSlot<Domain::HandleSearch::SearchPublication> slot;
    const std::stop_source stop;
    Domain::HandleSearch::run(self,
                              {.openFiles = &openFiles, .modules = &modules},
                              false,
                              Domain::HandleSearch::startFor("locked-by-findhandles", self.size(), 1),
                              slot,
                              stop.get_token());
    std::shared_ptr<const Domain::HandleSearch::SearchPublication> found = slot.load();
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(found->done);
    ASSERT_EQ(found->matches.size(), 1U);
    EXPECT_EQ(found->matches[0].path, held.string());
    EXPECT_EQ(found->matches[0].kind, Domain::HandleSearch::HandleKind::File);

    Domain::HandleSearch::run(self,
                              {.openFiles = &openFiles, .modules = &modules},
                              false,
                              Domain::HandleSearch::startFor("libc.so", self.size(), 10),
                              slot,
                              stop.get_token());
    found = slot.load();
    EXPECT_TRUE(std::ranges::any_of(
        found->matches, [](const Domain::HandleSearch::HandleMatch& m) { return m.kind == Domain::HandleSearch::HandleKind::Library; }));
}

} // namespace
} // namespace Platform
