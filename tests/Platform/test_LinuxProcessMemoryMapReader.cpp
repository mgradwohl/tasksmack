/// @file test_LinuxProcessMemoryMapReader.cpp
/// @brief Platform::LinuxProcessMemoryMapReader (#3089): a synthetic /proc tree read through the procRoot
/// seam (the identity check, a reused PID, a missing process, the size cap), and the real /proc/self.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/Linux/LinuxProcessMemoryMapReader.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

namespace Platform
{
namespace
{

constexpr ProcessTarget TARGET{.pid = 100, .startTimeTicks = 4242};

constexpr std::string_view SERVER = "55d0c1a00000-55d0c1a02000 r-xp 00000000 08:01 1311 /usr/bin/server\n"
                                    "Rss:                   8 kB\n"
                                    "Pss:                   8 kB\n";
constexpr std::string_view HEAP = "55d0c2000000-55d0c2021000 rw-p 00000000 00:00 0 [heap]\n"
                                  "Rss:                  64 kB\n"
                                  "Pss:                  64 kB\n";

class LinuxProcessMemoryMapReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::filesystem::create_directories(m_Proc.path / "100");
        write("stat", "100 (server) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 1 0 4242 1000 50\n");
        write("smaps", std::string(SERVER) + std::string(HEAP));
    }

    void write(const char* name, std::string_view contents) const
    {
        std::ofstream(m_Proc.path / "100" / name) << contents;
    }

    TestSupport::ScopedTempDir m_Proc{"ts_test_proc_memory_map"};
};

TEST_F(LinuxProcessMemoryMapReaderTest, ReadsTheProcessesMappings)
{
    LinuxProcessMemoryMapReader reader(m_Proc.path.string());
    EXPECT_TRUE(reader.hasMemoryMap());
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    ASSERT_EQ(result.mappings.size(), 2U);
    EXPECT_EQ(result.mappings[0].path, "/usr/bin/server");
    EXPECT_EQ(result.mappings[0].kind, MemoryMappingKind::Image);
    EXPECT_EQ(result.mappings[1].kind, MemoryMappingKind::Heap);
    EXPECT_EQ(result.mappings[1].pssBytes, 64U * 1024U);
    EXPECT_FALSE(result.truncated);
}

// The process decides how big its smaps is, so the read stops at a cap: the mappings before it are kept, the
// one the cap falls in (whose sizes may be cut off) dropped, and the result marked truncated.
TEST_F(LinuxProcessMemoryMapReaderTest, AnSmapsPastTheCapIsReadThatFarAndMarkedTruncated)
{
    // The cap falls inside the heap's entry: only the server's is whole.
    LinuxProcessMemoryMapReader reader(m_Proc.path.string(), SERVER.size() + 70);
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    ASSERT_EQ(result.mappings.size(), 1U);
    EXPECT_EQ(result.mappings[0].path, "/usr/bin/server");
    EXPECT_EQ(result.mappings[0].pssBytes, 8U * 1024U);
    EXPECT_TRUE(result.truncated);

    // A cap inside the first line leaves nothing whole: no mappings, still truncated.
    LinuxProcessMemoryMapReader tiny(m_Proc.path.string(), 10);
    const MemoryMapReadResult none = tiny.readMemoryMap(TARGET);
    ASSERT_EQ(none.status, MemoryMapReadStatus::Ok);
    EXPECT_TRUE(none.mappings.empty());
    EXPECT_TRUE(none.truncated);
}

TEST_F(LinuxProcessMemoryMapReaderTest, AnSmapsExactlyTheCapLongIsWholeNotTruncated)
{
    LinuxProcessMemoryMapReader reader(m_Proc.path.string(), SERVER.size() + HEAP.size());
    const MemoryMapReadResult result = reader.readMemoryMap(TARGET);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    EXPECT_EQ(result.mappings.size(), 2U);
    EXPECT_FALSE(result.truncated);
}

TEST_F(LinuxProcessMemoryMapReaderTest, RefusesAReusedPidAnUnknownIdentityAndAMissingProcess)
{
    LinuxProcessMemoryMapReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.readMemoryMap({.pid = 100, .startTimeTicks = 9999}).status, MemoryMapReadStatus::ProcessExited);
    EXPECT_EQ(reader.readMemoryMap({.pid = 100, .startTimeTicks = 0}).status, MemoryMapReadStatus::IdentityUnknown);
    EXPECT_EQ(reader.readMemoryMap({.pid = 101, .startTimeTicks = 4242}).status, MemoryMapReadStatus::ProcessExited);
    EXPECT_EQ(reader.readMemoryMap({.pid = 0, .startTimeTicks = 4242}).status, MemoryMapReadStatus::ProcessExited);
}

// No smaps is a process that has gone; one that can't be read as a file is a failed read that says why; an
// empty one, the identity confirmed again, is no mappings (a kernel thread's).
TEST_F(LinuxProcessMemoryMapReaderTest, WhatEachKindOfMissingOrBrokenSmapsReadsAs)
{
    LinuxProcessMemoryMapReader reader(m_Proc.path.string());
    std::filesystem::remove(m_Proc.path / "100" / "smaps");
    EXPECT_EQ(reader.readMemoryMap(TARGET).status, MemoryMapReadStatus::ProcessExited);

    std::filesystem::create_directory(m_Proc.path / "100" / "smaps"); // read() on it fails (EISDIR)
    const MemoryMapReadResult broken = reader.readMemoryMap(TARGET);
    EXPECT_EQ(broken.status, MemoryMapReadStatus::Failed);
    EXPECT_FALSE(broken.detail.empty());

    std::filesystem::remove(m_Proc.path / "100" / "smaps");
    write("smaps", "");
    const MemoryMapReadResult empty = reader.readMemoryMap(TARGET);
    EXPECT_EQ(empty.status, MemoryMapReadStatus::Ok);
    EXPECT_TRUE(empty.mappings.empty());
}

// An smaps this user can't read is permission denied (another user's process), which the brokered reader
// takes to the helper.
TEST_F(LinuxProcessMemoryMapReaderTest, AnUnreadableSmapsIsPermissionDenied)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root reads any file";
    }
    std::filesystem::permissions(m_Proc.path / "100" / "smaps", std::filesystem::perms::none);
    LinuxProcessMemoryMapReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.readMemoryMap(TARGET).status, MemoryMapReadStatus::PermissionDenied);
    std::filesystem::permissions(m_Proc.path / "100" / "smaps", std::filesystem::perms::owner_all);
}

TEST(LinuxProcessMemoryMapReaderRealTest, ReadsThisProcess)
{
    std::ifstream stat("/proc/self/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const std::optional<std::uint64_t> startTicks = ProcParsing::parseStatStartTime(line);
    ASSERT_TRUE(startTicks.has_value());

    LinuxProcessMemoryMapReader reader;
    const ProcessTarget self{.pid = static_cast<std::int32_t>(::getpid()), .startTimeTicks = startTicks.value_or(0)};
    const MemoryMapReadResult result = reader.readMemoryMap(self);
    ASSERT_EQ(result.status, MemoryMapReadStatus::Ok);
    EXPECT_FALSE(result.truncated);
    ASSERT_FALSE(result.mappings.empty());

    // The test binary is an image, and the stack is here; every mapping is measured and in address order.
    const std::string exe = std::filesystem::read_symlink("/proc/self/exe").string();
    EXPECT_TRUE(std::ranges::any_of(result.mappings,
                                    [&exe](const MemoryMapping& m) { return m.path == exe && m.kind == MemoryMappingKind::Image; }))
        << exe;
    EXPECT_TRUE(std::ranges::any_of(result.mappings, [](const MemoryMapping& m) { return m.kind == MemoryMappingKind::Stack; }));
    EXPECT_TRUE(
        std::ranges::all_of(result.mappings, [](const MemoryMapping& m) { return m.residentBytes.has_value() && m.pssBytes.has_value(); }));
    EXPECT_TRUE(std::ranges::is_sorted(result.mappings, {}, &MemoryMapping::start));

    // A start time that isn't ours is refused as a reused PID.
    EXPECT_EQ(reader.readMemoryMap({.pid = self.pid, .startTimeTicks = self.startTimeTicks + 1}).status,
              MemoryMapReadStatus::ProcessExited);
}

} // namespace
} // namespace Platform
