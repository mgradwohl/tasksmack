// The Linux Threads reader (#3069): a fake /proc tree, and this test process itself.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"
#include "Platform/Linux/LinuxProcessThreadsReader.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <latch>
#include <string>
#include <string_view>
#include <thread>

#include <pthread.h>
#include <unistd.h>

namespace Platform
{
namespace
{

constexpr ProcessTarget TARGET{.pid = 100, .startTimeTicks = 4242};

class LinuxProcessThreadsReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::filesystem::create_directories(m_Proc.path / "100" / "task" / "100");
        std::filesystem::create_directories(m_Proc.path / "100" / "task" / "105");
        write("100/stat", "100 (server) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 2 0 4242 1000 50\n");
        write("100/task/100/stat", "100 (server) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 2 0 4242 1000 50\n");
        // A worker with a name holding a space and a ')', running, at nice 5.
        write("100/task/105/stat", "105 (pool) worker) R 1 100 100 0 -1 4194560 10 0 0 0 300 40 0 0 25 5 2 0 5000 1000 50\n");
    }

    void write(const std::string& path, std::string_view contents) const
    {
        std::ofstream(m_Proc.path / path) << contents;
    }

    TestSupport::ScopedTempDir m_Proc{"ts_test_proc_threads"};
};

TEST_F(LinuxProcessThreadsReaderTest, ReadsEachThreadsStat)
{
    LinuxProcessThreadsReader reader(m_Proc.path.string());
    EXPECT_TRUE(reader.hasThreads());
    const ThreadsReadResult result = reader.readThreads(TARGET);
    ASSERT_EQ(result.status, ThreadsReadStatus::Ok);
    EXPECT_GT(result.ticksPerSecond, 0);
    ASSERT_EQ(result.threads.size(), 2U);
    EXPECT_EQ(result.threads[0].tid, 100);
    EXPECT_EQ(result.threads[0].name, "server");
    EXPECT_EQ(result.threads[1].tid, 105);
    EXPECT_EQ(result.threads[1].name, "pool) worker");
    EXPECT_EQ(result.threads[1].state, 'R');
    EXPECT_EQ(result.threads[1].userTicks, 300U);
    EXPECT_EQ(result.threads[1].systemTicks, 40U);
    EXPECT_EQ(result.threads[1].nice, 5);
    EXPECT_EQ(result.threads[1].startTicks, 5000U);
}

TEST_F(LinuxProcessThreadsReaderTest, AThreadWhoseStatCantBeReadIsLeftOut)
{
    std::filesystem::create_directories(m_Proc.path / "100" / "task" / "106"); // exited: no stat
    write("100/task/not-a-tid", "");
    LinuxProcessThreadsReader reader(m_Proc.path.string());
    const ThreadsReadResult result = reader.readThreads(TARGET);
    ASSERT_EQ(result.status, ThreadsReadStatus::Ok);
    EXPECT_EQ(result.threads.size(), 2U);
}

TEST_F(LinuxProcessThreadsReaderTest, AnotherProcessOnThePidOrAnUnknownStartTimeIsNotRead)
{
    LinuxProcessThreadsReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.readThreads({.pid = 100, .startTimeTicks = 9999}).status, ThreadsReadStatus::ProcessExited);
    EXPECT_EQ(reader.readThreads({.pid = 100, .startTimeTicks = 0}).status, ThreadsReadStatus::IdentityUnknown);
    EXPECT_EQ(reader.readThreads({.pid = 200, .startTimeTicks = 1}).status, ThreadsReadStatus::ProcessExited);
    EXPECT_EQ(reader.readThreads({.pid = 0, .startTimeTicks = 1}).status, ThreadsReadStatus::ProcessExited);
}

TEST_F(LinuxProcessThreadsReaderTest, AnEmptyTaskDirectoryIsAnExitedProcess)
{
    std::filesystem::remove_all(m_Proc.path / "100" / "task");
    std::filesystem::create_directories(m_Proc.path / "100" / "task");
    LinuxProcessThreadsReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.readThreads(TARGET).status, ThreadsReadStatus::ProcessExited);
}

// The real /proc: this test process, with a thread of its own named while it waits.
TEST(LinuxProcessThreadsReaderRealTest, ListsThisProcessesThreadsByName)
{
    std::array<char, 1024> stat{};
    const std::size_t len = ProcParsing::readProcFile("/proc/self/stat", stat.data(), stat.size());
    const std::uint64_t start = ProcParsing::parseStatStartTime(std::string_view(stat.data(), len)).value_or(0);
    ASSERT_NE(start, 0U);

    std::latch named(1);
    std::latch done(1);
    const std::jthread worker(
        [&named, &done]
        {
            ::pthread_setname_np(::pthread_self(), "ts-thread-test");
            named.count_down();
            done.wait();
        });
    named.wait();

    LinuxProcessThreadsReader reader;
    const ThreadsReadResult result = reader.readThreads({.pid = static_cast<std::int32_t>(::getpid()), .startTimeTicks = start});
    done.count_down();

    ASSERT_EQ(result.status, ThreadsReadStatus::Ok) << result.detail;
    EXPECT_TRUE(std::ranges::any_of(result.threads, [](const ProcessThread& t) { return t.tid == ::getpid(); }));
    EXPECT_TRUE(std::ranges::any_of(result.threads, [](const ProcessThread& t) { return t.name == "ts-thread-test"; }));
}

} // namespace
} // namespace Platform
