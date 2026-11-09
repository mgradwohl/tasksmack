/// @file test_HiddenProcessesRead.cpp
/// @brief The processes hidepid hides from TaskSmack, as tasksmack-helper lists them (#2728): every one
/// under the proc root whose PID isn't among the visible ones, with its fixed set of files' text and the
/// start time its stat names; a directory without a readable stat (exited) is left out.

#include "Platform/BrokerProtocol.h"
#include "Platform/Linux/HiddenProcessesRead.h"
#include "ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Linux
{
namespace
{

using TestSupport::ScopedTempDir;

/// A fake /proc's I/O priority reader: never ioprio_get(2), which would ask the real processes that happen to have
/// its PIDs. Fails for every process, as for one that exited.
int noIoprio(std::int32_t /*pid*/)
{
    return -1;
}

void writeFile(const std::filesystem::path& path, std::string_view contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/// A process directory with every file the helper reads, and a stat naming @p startTime.
void writeProcess(const std::filesystem::path& procRoot, std::int32_t pid, std::uint64_t startTime)
{
    const auto dir = procRoot / std::to_string(pid);
    writeFile(dir / "stat", std::to_string(pid) + " (svc) S 1 0 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 " + std::to_string(startTime) + " 0 0\n");
    writeFile(dir / "statm", "1 2 3 0 0 0 0\n");
    writeFile(dir / "status", "Name:\tsvc\nUid:\t0\t0\t0\t0\n");
    writeFile(dir / "cmdline", std::string("/usr/sbin/svc\0", 14));
    writeFile(dir / "io", "read_bytes: 1\nwrite_bytes: 2\n");
    writeFile(dir / "cgroup", "0::/system.slice/svc.service\n");
}

TEST(HiddenProcessesReadTest, ListsEveryProcessNotAmongTheVisibleWithItsFilesText)
{
    const ScopedTempDir proc("hidden_processes");
    writeProcess(proc.path, 100, 11);
    writeProcess(proc.path, 200, 22);
    writeProcess(proc.path, 300, 33);
    std::filesystem::create_directories(proc.path / "400"); // exited: no stat
    std::filesystem::create_directories(proc.path / "self");
    std::filesystem::create_directories(proc.path / "sys");

    const std::vector<std::int32_t> visible{300, 1};
    Broker::HiddenProcessesReply reply = listHiddenProcessesAt(proc.path, visible, noIoprio);
    std::ranges::sort(reply.processes, {}, [](const Broker::HiddenProcess& process) { return process.target.pid; });
    ASSERT_EQ(reply.processes.size(), 2U);
    EXPECT_FALSE(reply.truncated);
    const Broker::HiddenProcess& first = reply.processes.front();
    EXPECT_EQ(first.target.pid, 100);
    EXPECT_EQ(first.target.startTimeTicks, 11U);
    EXPECT_TRUE(first.stat.starts_with("100 (svc) S"));
    EXPECT_EQ(first.statm, "1 2 3 0 0 0 0\n");
    EXPECT_EQ(first.status, "Name:\tsvc\nUid:\t0\t0\t0\t0\n");
    ASSERT_TRUE(first.cmdline.has_value());
    EXPECT_EQ(*first.cmdline, std::string("/usr/sbin/svc\0", 14));
    EXPECT_EQ(first.io, "read_bytes: 1\nwrite_bytes: 2\n");
    EXPECT_EQ(first.cgroup, "0::/system.slice/svc.service\n");
    EXPECT_EQ(reply.processes.back().target.pid, 200);
    EXPECT_EQ(reply.processes.back().target.startTimeTicks, 22U);
}

TEST(HiddenProcessesReadTest, AnUnreadableCommandLineIsUnreadNotEmptyAndAMissingRootListsNothing)
{
    const ScopedTempDir proc("hidden_processes_partial");
    writeProcess(proc.path, 100, 11);
    std::filesystem::remove(proc.path / "100" / "cmdline");
    std::filesystem::remove(proc.path / "100" / "io");
    const Broker::HiddenProcessesReply reply = listHiddenProcessesAt(proc.path, {}, noIoprio);
    ASSERT_EQ(reply.processes.size(), 1U);
    EXPECT_FALSE(reply.processes.front().cmdline.has_value()) << "unread, so not taken for a kernel thread";
    EXPECT_TRUE(reply.processes.front().io.empty());

    EXPECT_TRUE(listHiddenProcessesAt(proc.path / "missing", {}, noIoprio).processes.empty());
}

// #3572: each hidden process's I/O priority, read by PID, kept only while its directory still resolves after the
// read: a process reaped meanwhile could have handed its PID on. A failed read is no value either.
TEST(HiddenProcessesReadTest, CarriesEachProcessIoPriorityReadWhileItsDirectoryResolves)
{
    const ScopedTempDir proc("hidden_processes_ioprio");
    writeProcess(proc.path, 100, 11);
    writeProcess(proc.path, 200, 22);
    writeProcess(proc.path, 300, 33);
    constexpr int BEST_EFFORT_4 = (2 << 13) | 4;
    std::vector<std::int32_t> asked;
    Broker::HiddenProcessesReply reply = listHiddenProcessesAt(proc.path,
                                                               {},
                                                               [&proc, &asked](std::int32_t pid)
                                                               {
                                                                   asked.push_back(pid);
                                                                   if (pid == 200)
                                                                   {
                                                                       std::filesystem::remove(proc.path / "200" / "stat"); // reaped
                                                                   }
                                                                   return pid == 300 ? -1 : BEST_EFFORT_4;
                                                               });
    std::ranges::sort(reply.processes, {}, [](const Broker::HiddenProcess& process) { return process.target.pid; });
    ASSERT_EQ(reply.processes.size(), 3U);
    EXPECT_EQ(reply.processes[0].ioprio, std::optional<std::int32_t>{BEST_EFFORT_4});
    EXPECT_FALSE(reply.processes[1].ioprio.has_value()) << "read after its directory stopped resolving";
    EXPECT_FALSE(reply.processes[2].ioprio.has_value()) << "the read failed";
    std::ranges::sort(asked);
    EXPECT_EQ(asked, (std::vector<std::int32_t>{100, 200, 300}));
}

} // namespace
} // namespace Platform::Linux
