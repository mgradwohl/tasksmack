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
    Broker::HiddenProcessesReply reply = listHiddenProcessesAt(proc.path, visible);
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
    const Broker::HiddenProcessesReply reply = listHiddenProcessesAt(proc.path, {});
    ASSERT_EQ(reply.processes.size(), 1U);
    EXPECT_FALSE(reply.processes.front().cmdline.has_value()) << "unread, so not taken for a kernel thread";
    EXPECT_TRUE(reply.processes.front().io.empty());

    EXPECT_TRUE(listHiddenProcessesAt(proc.path / "missing", {}).processes.empty());
}

} // namespace
} // namespace Platform::Linux
