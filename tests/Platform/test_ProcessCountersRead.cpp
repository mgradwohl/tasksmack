/// @file test_ProcessCountersRead.cpp
/// @brief /proc/[pid]/io's parsing and one process's FD count and I/O as tasksmack-helper reads them for
/// the sampler (#2728): both figures from this test's own process, checked against its start time.

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/Linux/ProcIoParser.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/Linux/ProcessCountersRead.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace Platform::Linux
{
namespace
{

TEST(ProcIoParserTest, ReadsReadAndWriteBytes)
{
    const auto io = ProcIoParser::parse("rchar: 1\nwchar: 2\nsyscr: 3\nsyscw: 4\nread_bytes: 4096\nwrite_bytes:\t8192\n"
                                        "cancelled_write_bytes: 0\n");
    ASSERT_TRUE(io.has_value());
    EXPECT_EQ(io.value_or(ProcIoParser::ProcIo{}).readBytes, 4096U);
    EXPECT_EQ(io.value_or(ProcIoParser::ProcIo{}).writeBytes, 8192U);
}

TEST(ProcIoParserTest, NeedsBothFigures)
{
    EXPECT_FALSE(ProcIoParser::parse("read_bytes: 1\n").has_value());
    EXPECT_FALSE(ProcIoParser::parse("write_bytes: 1\n").has_value());
    EXPECT_FALSE(ProcIoParser::parse("read_bytes: x\nwrite_bytes: 1\n").has_value());
    EXPECT_FALSE(ProcIoParser::parse("").has_value());
}

/// This process's start time, as /proc/self/stat gives it.
[[nodiscard]] std::uint64_t ownStartTime()
{
    std::array<char, 1024> stat{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int self = ::open("/proc/self", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const std::size_t length = ProcParsing::readProcFileOnceAt(self, "stat", stat.data(), stat.size());
    ::close(self);
    return ProcParsing::parseStatStartTime(std::string_view(stat.data(), length)).value_or(0);
}

TEST(ProcessCountersReadTest, ReadsItsOwnProcesssFdCountAndIo)
{
    const ProcessTarget self{.pid = ::getpid(), .startTimeTicks = ownStartTime()};
    ASSERT_NE(self.startTimeTicks, 0U);
    const std::optional<Broker::ProcessCountersEntry> entry = readProcessCountersAt("/proc", self);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->target, self);
    EXPECT_TRUE(entry->handleCountRead);
    EXPECT_GE(entry->handleCount, 3);
    // A kernel without task I/O accounting (CONFIG_TASK_IO_ACCOUNTING, absent from WSL's) has no io file.
    EXPECT_EQ(entry->ioRead, std::filesystem::exists("/proc/self/io"));
}

TEST(ProcessCountersReadTest, AStartTimeThatDoesntMatchOrAGonePidReadsNothing)
{
    EXPECT_FALSE(readProcessCountersAt("/proc", {.pid = ::getpid(), .startTimeTicks = ownStartTime() + 1}).has_value());
    EXPECT_FALSE(readProcessCountersAt("/proc", {.pid = 0, .startTimeTicks = 0}).has_value());
    EXPECT_FALSE(readProcessCountersAt("/nonexistent", {.pid = ::getpid(), .startTimeTicks = ownStartTime()}).has_value());
}

} // namespace
} // namespace Platform::Linux
