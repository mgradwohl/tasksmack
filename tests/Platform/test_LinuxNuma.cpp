/// @file test_LinuxNuma.cpp
/// @brief The Linux NUMA node parsers and readers (#3088), on text and on a fixture node root.

#include <gtest/gtest.h>

#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/CpuAffinity.h"
#include "Platform/Linux/LinuxNuma.h"
#include "Platform/ScopedTempDir.h"
#include "Platform/SystemTypes.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

using TestSupport::ScopedTempDir;

constexpr std::uint64_t KIB = 1024;

/// A node's meminfo as the kernel prints it: the "Node <id>" prefix, aligned values, more lines after.
std::string meminfoText(int id, std::uint64_t totalKib, std::uint64_t freeKib)
{
    const std::string prefix = "Node " + std::to_string(id) + " ";
    return prefix + "MemTotal:       " + std::to_string(totalKib) + " kB\n" + prefix + "MemFree:        " + std::to_string(freeKib) +
           " kB\n" + prefix + "MemUsed:        " + std::to_string(totalKib - freeKib) + " kB\n" + prefix + "SwapCached:         0 kB\n" +
           prefix + "HugePages_Total:     0\n";
}

/// Writes <root>/node<id>/{cpulist,meminfo}; a nullopt file is left out.
void writeNode(const std::filesystem::path& root,
               int id,
               const std::optional<std::string>& cpulist,
               const std::optional<std::string>& meminfo)
{
    const std::filesystem::path dir = root / ("node" + std::to_string(id));
    std::filesystem::create_directories(dir);
    if (cpulist.has_value())
    {
        std::ofstream(dir / "cpulist") << *cpulist;
    }
    if (meminfo.has_value())
    {
        std::ofstream(dir / "meminfo") << *meminfo;
    }
}

/// The node root's other entries, which aren't nodes.
void writeNodeRootFiles(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "power");
    std::ofstream(root / "online") << "0-1\n";
    std::ofstream(root / "possible") << "0-3\n";
    std::ofstream(root / "has_cpu") << "0-1\n";
    std::ofstream(root / "has_memory") << "0-1\n";
}

CpuAffinity cpus(const char* list)
{
    return CpuAffinity::fromCpuList(list).value_or(CpuAffinity{});
}

// =============================================================================
// meminfo
// =============================================================================

TEST(LinuxNumaTest, MeminfoReadsTotalAndFree)
{
    const auto memory = LinuxNuma::parseNodeMeminfo(meminfoText(0, 16'316'412, 2'335'000));
    ASSERT_TRUE(memory.has_value());
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{}).totalBytes, 16'316'412 * KIB);
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{}).freeBytes, 2'335'000 * KIB);
}

TEST(LinuxNumaTest, MeminfoFallsBackToMemUsedWithoutMemFree)
{
    const auto memory = LinuxNuma::parseNodeMeminfo("Node 1 MemTotal: 1000 kB\nNode 1 MemUsed: 600 kB\n");
    ASSERT_TRUE(memory.has_value());
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{}).totalBytes, 1000 * KIB);
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{}).freeBytes, 400 * KIB);
}

TEST(LinuxNumaTest, MeminfoOfAMemorylessNodeIsZero)
{
    // A node with CPUs and no memory still has a meminfo, all zeros.
    const auto memory = LinuxNuma::parseNodeMeminfo(meminfoText(2, 0, 0));
    ASSERT_TRUE(memory.has_value());
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{.totalBytes = 1}).totalBytes, 0U);
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{.freeBytes = 1}).freeBytes, 0U);
}

TEST(LinuxNumaTest, MeminfoSkipsMalformedLines)
{
    // Each bad MemTotal/MemFree line is skipped; the well-formed ones after them still count.
    const auto memory = LinuxNuma::parseNodeMeminfo("garbage\n"
                                                    "Node x MemTotal: 5 kB\n"
                                                    "Node 0 MemTotal 5 kB\n"
                                                    "Node 0 MemTotal: -5 kB\n"
                                                    "Node 0 MemTotal: 5 MB\n"
                                                    "Node 0 MemTotal: 5\n" // no unit: not taken as 5 KiB
                                                    "Node 0 MemFree: 3\n"
                                                    "Node 0 MemUsed: 1\n"
                                                    "Node 0 MemTotal: 5 kB extra\n"
                                                    "Node 0 MemTotal: 99999999999999999999 kB\n"
                                                    "Node 0 MemTotal: 18446744073709551615 kB\n" // overflows as bytes
                                                    "Node 0 MemFree: 1x kB\n"
                                                    "\n"
                                                    "Node 0 MemTotal: 2048 kB\n"
                                                    "Node 0 MemFree:\t1024 kB\r\n"
                                                    "Node 0 MemTotal: 9 kB\n"); // a repeat: the first counts
    ASSERT_TRUE(memory.has_value());
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{}).totalBytes, 2048 * KIB);
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{}).freeBytes, 1024 * KIB);
}

TEST(LinuxNumaTest, MeminfoValuesWithoutTheKbUnitAreSkipped)
{
    // A unitless value first doesn't win as the first occurrence: the valid line after it counts.
    const auto memory = LinuxNuma::parseNodeMeminfo("Node 0 MemTotal: 5\n"
                                                    "Node 0 MemTotal: 2048 kB\n"
                                                    "Node 0 MemFree: 7\n"
                                                    "Node 0 MemFree: 512 kB\n");
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{}).totalBytes, 2048 * KIB);
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{}).freeBytes, 512 * KIB);

    // MemUsed without a unit isn't used for the fallback either; with only unitless values, nothing reads.
    EXPECT_FALSE(LinuxNuma::parseNodeMeminfo("Node 0 MemTotal: 2048 kB\nNode 0 MemUsed: 1024\n").has_value());
    EXPECT_FALSE(LinuxNuma::parseNodeMeminfo("Node 0 MemTotal: 2048\nNode 0 MemFree: 1024\n").has_value());
}

TEST(LinuxNumaTest, MeminfoWithoutTotalOrFreeIsUnread)
{
    EXPECT_FALSE(LinuxNuma::parseNodeMeminfo("").has_value());
    EXPECT_FALSE(LinuxNuma::parseNodeMeminfo("Node 0 MemFree: 10 kB\n").has_value());
    EXPECT_FALSE(LinuxNuma::parseNodeMeminfo("Node 0 MemTotal: 10 kB\nNode 0 Active: 3 kB\n").has_value());
    EXPECT_FALSE(LinuxNuma::parseNodeMeminfo("Node 0 MemTotal: bad kB\nNode 0 MemFree: 1 kB\n").has_value());
}

TEST(LinuxNumaTest, MeminfoFreeNeverExceedsTotal)
{
    const auto memory = LinuxNuma::parseNodeMeminfo("Node 0 MemTotal: 10 kB\nNode 0 MemFree: 20 kB\n");
    EXPECT_EQ(memory.value_or(LinuxNuma::NodeMemory{}).freeBytes, 10 * KIB);
    const auto overUsed = LinuxNuma::parseNodeMeminfo("Node 0 MemTotal: 10 kB\nNode 0 MemUsed: 20 kB\n");
    EXPECT_EQ(overUsed.value_or(LinuxNuma::NodeMemory{.freeBytes = 1}).freeBytes, 0U);
}

// =============================================================================
// cpulist and node names
// =============================================================================

TEST(LinuxNumaTest, CpuListParses)
{
    EXPECT_EQ(LinuxNuma::parseNodeCpuList("0-7,16-23\n"), cpus("0-7,16-23"));
    EXPECT_EQ(LinuxNuma::parseNodeCpuList("64-127\n"), cpus("64-127"));
}

TEST(LinuxNumaTest, CpuListOfACpulessNodeIsEmpty)
{
    const auto none = LinuxNuma::parseNodeCpuList("\n");
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none.value_or(cpus("0")).empty());
}

TEST(LinuxNumaTest, MalformedCpuListIsUnread)
{
    for (const char* text : {"0-", "3-1", "a", "0,,1", "0-3 junk"})
    {
        SCOPED_TRACE(text);
        EXPECT_FALSE(LinuxNuma::parseNodeCpuList(text).has_value());
    }
}

TEST(LinuxNumaTest, NodeIdsComeFromNodeDirectoryNames)
{
    EXPECT_EQ(LinuxNuma::nodeIdFromName("node0"), 0);
    EXPECT_EQ(LinuxNuma::nodeIdFromName("node17"), 17);
    for (const char* name : {"node", "node-1", "node+1", "nodeX", "node1a", "online", "has_cpu", "Node1", "node99999999999"})
    {
        SCOPED_TRACE(name);
        EXPECT_FALSE(LinuxNuma::nodeIdFromName(name).has_value());
    }
}

// =============================================================================
// Fixture node root
// =============================================================================

TEST(LinuxNumaTest, ReadsATwoNodeMachine)
{
    ScopedTempDir root("ts_test_numa_two");
    writeNodeRootFiles(root.path);
    writeNode(root.path, 1, "8-15,24-31\n", meminfoText(1, 8'000'000, 3'000'000));
    writeNode(root.path, 0, "0-7,16-23\n", meminfoText(0, 16'000'000, 5'000'000));

    const std::vector<NumaNode> nodes = LinuxNuma::readNodes(root.path);
    ASSERT_EQ(nodes.size(), 2U);
    EXPECT_EQ(nodes[0].id, 0); // sorted by id, whatever the listing order
    EXPECT_EQ(nodes[0].cpus, cpus("0-7,16-23"));
    EXPECT_EQ(nodes[0].memTotalBytes, 16'000'000 * KIB);
    EXPECT_EQ(nodes[0].memFreeBytes, 5'000'000 * KIB);
    EXPECT_EQ(nodes[1].id, 1);
    EXPECT_EQ(nodes[1].cpus, cpus("8-15,24-31"));
    EXPECT_EQ(nodes[1].cpus.count(), 16U);
    EXPECT_EQ(nodes[1].memTotalBytes, 8'000'000 * KIB);
    EXPECT_EQ(nodes[1].memFreeBytes, 3'000'000 * KIB);
}

TEST(LinuxNumaTest, MemorylessAndCpulessNodesAreKept)
{
    ScopedTempDir root("ts_test_numa_odd");
    writeNode(root.path, 0, "0-3\n", meminfoText(0, 4'000'000, 1'000'000));
    writeNode(root.path, 1, "4-7\n", meminfoText(1, 0, 0));              // CPUs, no memory
    writeNode(root.path, 2, "\n", meminfoText(2, 2'000'000, 2'000'000)); // memory only (CXL, HBM)

    const std::vector<NumaNode> nodes = LinuxNuma::readNodes(root.path);
    ASSERT_EQ(nodes.size(), 3U);
    EXPECT_EQ(nodes[1].cpus, cpus("4-7"));
    EXPECT_EQ(nodes[1].memTotalBytes, 0U);
    EXPECT_TRUE(nodes[2].cpus.empty());
    EXPECT_EQ(nodes[2].memTotalBytes, 2'000'000 * KIB);
}

TEST(LinuxNumaTest, MissingNodeRootOrNodeFilesAreLeftOut)
{
    // No node root at all: a kernel without CONFIG_NUMA.
    ScopedTempDir root("ts_test_numa_missing");
    EXPECT_TRUE(LinuxNuma::readNodes(root.path / "no-such-dir").empty());
    EXPECT_TRUE(LinuxNuma::listNodeIds(root.path / "no-such-dir").empty());
    EXPECT_FALSE(LinuxNuma::readNodeMemory(root.path, 5).has_value());
    EXPECT_FALSE(LinuxNuma::readNodeCpus(root.path, 5).has_value());

    // A node without meminfo (gone offline mid-read) is left out; one without a readable cpulist keeps
    // its memory, with no CPUs.
    writeNode(root.path, 0, "0-1\n", meminfoText(0, 1000, 500));
    writeNode(root.path, 1, "2-3\n", std::nullopt);
    writeNode(root.path, 2, std::nullopt, meminfoText(2, 2000, 1000));
    writeNode(root.path, 3, "bogus\n", meminfoText(3, 3000, 1500));
    const std::vector<NumaNode> nodes = LinuxNuma::readNodes(root.path);
    ASSERT_EQ(nodes.size(), 3U);
    EXPECT_EQ(nodes[0].id, 0);
    EXPECT_EQ(nodes[1].id, 2);
    EXPECT_TRUE(nodes[1].cpus.empty());
    EXPECT_EQ(nodes[1].memTotalBytes, 2000 * KIB);
    EXPECT_EQ(nodes[2].id, 3);
    EXPECT_TRUE(nodes[2].cpus.empty());
}

TEST(LinuxNumaTest, TheRealNodeRootReads)
{
    // Smoke: every Linux kernel built with CONFIG_NUMA has the node root, with at least one node.
    const std::filesystem::path real("/sys/devices/system/node");
    std::error_code ec;
    if (!std::filesystem::is_directory(real, ec))
    {
        GTEST_SKIP() << "no " << real << " (kernel without CONFIG_NUMA)";
    }
    const std::vector<NumaNode> nodes = LinuxNuma::readNodes(real);
    ASSERT_FALSE(nodes.empty());
    std::uint64_t cpuCount = 0;
    for (const NumaNode& node : nodes)
    {
        EXPECT_LE(node.memFreeBytes, node.memTotalBytes);
        cpuCount += node.cpus.count();
    }
    EXPECT_GT(cpuCount, 0U);
}

} // namespace
} // namespace Platform

#endif
