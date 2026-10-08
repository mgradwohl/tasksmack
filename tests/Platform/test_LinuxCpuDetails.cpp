/// @file test_LinuxCpuDetails.cpp
/// @brief Tests for Platform::LinuxCpuDetails (#809): /proc/cpuinfo topology, sysfs cache totals and
/// the base clock, against fixture proc/sys trees. LinuxCpuDetails.h uses only the standard library,
/// so these build and run on every platform.

#include "Platform/CpuDetails.h"
#include "Platform/Linux/LinuxCpuDetails.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform
{
namespace
{

constexpr std::uint64_t KIB = 1024;
constexpr std::uint64_t MIB = 1024 * KIB;

/// A fresh directory under the temp directory, removed (best effort) when the test ends.
class FixtureDir
{
  public:
    FixtureDir()
    {
        static std::atomic<unsigned> counter{0};
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_Path = std::filesystem::temp_directory_path() / std::format("ts_cpudetails_{}_{}_{}",
                                                                      (info != nullptr) ? info->name() : "test",
                                                                      counter.fetch_add(1),
                                                                      std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(m_Path);
    }
    ~FixtureDir() noexcept
    {
        try
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Path, ec); // Best effort
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a destructor must not throw; the temp dir is left behind
        {}
    }
    FixtureDir(const FixtureDir&) = delete;
    FixtureDir& operator=(const FixtureDir&) = delete;
    FixtureDir(FixtureDir&&) = delete;
    FixtureDir& operator=(FixtureDir&&) = delete;

    [[nodiscard]] std::filesystem::path proc() const
    {
        return m_Path / "proc";
    }
    [[nodiscard]] std::filesystem::path cpuSysfs() const
    {
        return m_Path / "sys" / "devices" / "system" / "cpu";
    }

    static void write(const std::filesystem::path& file, std::string_view text)
    {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << text;
    }

    /// /proc/cpuinfo from one block per processor.
    void writeCpuInfo(const std::vector<std::string>& blocks) const
    {
        std::string text;
        for (const std::string& block : blocks)
        {
            text += block;
            text += '\n';
        }
        write(proc() / "cpuinfo", text);
    }

    /// cpu<cpu>/cache/index<index>/{level,type,size,shared_cpu_list}; an empty `shared` leaves the list out.
    void writeCache(std::size_t cpu, int index, int level, std::string_view type, std::string_view size, std::string_view shared) const
    {
        const auto dir = cpuSysfs() / std::format("cpu{}", cpu) / "cache" / std::format("index{}", index);
        write(dir / "level", std::format("{}\n", level));
        write(dir / "type", std::format("{}\n", type));
        write(dir / "size", std::format("{}\n", size));
        if (!shared.empty())
        {
            write(dir / "shared_cpu_list", std::format("{}\n", shared));
        }
    }

    void writeCpufreq(std::string_view file, std::string_view kHz) const
    {
        write(cpuSysfs() / "cpu0" / "cpufreq" / file, std::format("{}\n", kHz));
    }

  private:
    std::filesystem::path m_Path;
};

[[nodiscard]] std::string processorBlock(std::size_t processor,
                                         std::optional<std::size_t> physicalId,
                                         std::optional<std::size_t> coreId,
                                         std::optional<std::size_t> cpuCores = std::nullopt)
{
    std::string block = std::format("processor\t: {}\nvendor_id\t: GenuineIntel\nmodel name\t: Test CPU\n", processor);
    if (physicalId.has_value())
    {
        block += std::format("physical id\t: {}\n", *physicalId);
    }
    if (coreId.has_value())
    {
        block += std::format("core id\t\t: {}\n", *coreId);
    }
    if (cpuCores.has_value())
    {
        block += std::format("cpu cores\t: {}\n", *cpuCores);
    }
    return block;
}

// -----------------------------------------------------------------------------
// Pure parsing
// -----------------------------------------------------------------------------

TEST(LinuxCpuDetailsTest, CacheSizesParseWithTheirUnits)
{
    using LinuxCpuDetails::parseCacheSize;
    EXPECT_EQ(parseCacheSize("32K\n"), 32 * KIB);
    EXPECT_EQ(parseCacheSize("1024K"), 1 * MIB);
    EXPECT_EQ(parseCacheSize("8M"), 8 * MIB);
    EXPECT_EQ(parseCacheSize("16384 KB"), 16 * MIB);
    EXPECT_EQ(parseCacheSize("4096"), 4096U);
    EXPECT_FALSE(parseCacheSize("").has_value());
    EXPECT_FALSE(parseCacheSize("0K").has_value());
    EXPECT_FALSE(parseCacheSize("big").has_value());
    EXPECT_FALSE(parseCacheSize("12Q").has_value());
}

TEST(LinuxCpuDetailsTest, BaseSpeedComesOnlyFromBaseFrequency)
{
    using LinuxCpuDetails::baseSpeedMHzFromKHz;
    EXPECT_EQ(baseSpeedMHzFromKHz(3'600'000), 3600U);
    EXPECT_FALSE(baseSpeedMHzFromKHz(0).has_value());
    EXPECT_FALSE(baseSpeedMHzFromKHz(999).has_value());
    EXPECT_FALSE(baseSpeedMHzFromKHz(std::nullopt).has_value());
}

TEST(LinuxCpuDetailsTest, CpuDirectoryNamesAreRecognised)
{
    using LinuxCpuDetails::cpuDirectoryIndex;
    EXPECT_EQ(cpuDirectoryIndex("cpu0"), 0U);
    EXPECT_EQ(cpuDirectoryIndex("cpu127"), 127U);
    EXPECT_FALSE(cpuDirectoryIndex("cpu").has_value());
    EXPECT_FALSE(cpuDirectoryIndex("cpufreq").has_value());
    EXPECT_FALSE(cpuDirectoryIndex("cpuidle").has_value());
    EXPECT_FALSE(cpuDirectoryIndex("online").has_value());
}

TEST(LinuxCpuDetailsTest, ArmCpuInfoWithoutPhysicalIdsCountsOnlyLogicalProcessors)
{
    std::string text;
    for (std::size_t cpu = 0; cpu < 4; ++cpu)
    {
        text += std::format("processor\t: {}\nBogoMIPS\t: 108.00\nCPU implementer\t: 0x41\n\n", cpu);
    }
    CpuDetails details;
    LinuxCpuDetails::parseCpuInfoTopology(text, details);
    EXPECT_EQ(details.logicalProcessors, 4U);
    EXPECT_FALSE(details.sockets.has_value());
    EXPECT_FALSE(details.physicalCores.has_value());
}

TEST(LinuxCpuDetailsTest, CpuInfoWithoutCoreIdsFallsBackToCpuCoresPerSocket)
{
    // Some hypervisors list a socket and its "cpu cores" but no "core id"
    std::string text;
    for (std::size_t cpu = 0; cpu < 4; ++cpu)
    {
        text += processorBlock(cpu, 0, std::nullopt, 2) + "\n";
    }
    CpuDetails details;
    LinuxCpuDetails::parseCpuInfoTopology(text, details);
    EXPECT_EQ(details.logicalProcessors, 4U);
    EXPECT_EQ(details.sockets, 1U);
    EXPECT_EQ(details.physicalCores, 2U);
}

// -----------------------------------------------------------------------------
// Fixture trees
// -----------------------------------------------------------------------------

TEST(LinuxCpuDetailsTest, SingleSocketSmtSystem)
{
    // 4 cores x 2 threads: cpuN and cpuN+4 are siblings on core N
    const FixtureDir fixture;
    std::vector<std::string> blocks;
    for (std::size_t cpu = 0; cpu < 8; ++cpu)
    {
        blocks.push_back(processorBlock(cpu, 0, cpu % 4, 4));
        const std::string siblings = std::format("{},{}", cpu % 4, (cpu % 4) + 4);
        fixture.writeCache(cpu, 0, 1, "Data", "32K", siblings);
        fixture.writeCache(cpu, 1, 1, "Instruction", "32K", siblings);
        fixture.writeCache(cpu, 2, 2, "Unified", "512K", siblings);
        fixture.writeCache(cpu, 3, 3, "Unified", "8192K", "0-7");
    }
    fixture.writeCpuInfo(blocks);
    fixture.writeCpufreq("base_frequency", "3600000");
    fixture.writeCpufreq("cpuinfo_max_freq", "5000000");

    const CpuDetails details = LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs());
    EXPECT_EQ(details.sockets, 1U);
    EXPECT_EQ(details.physicalCores, 4U);
    EXPECT_EQ(details.logicalProcessors, 8U);
    EXPECT_EQ(details.l1CacheBytes, 256 * KIB); // 4 cores x (32K data + 32K instruction)
    EXPECT_EQ(details.l2CacheBytes, 2 * MIB);   // 4 x 512K, each listed by both siblings
    EXPECT_EQ(details.l3CacheBytes, 8 * MIB);   // One instance, listed by all eight
    EXPECT_EQ(details.baseSpeedMHz, 3600U);     // base_frequency, not the boost clock
    EXPECT_FALSE(details.performanceCores.has_value());
    EXPECT_TRUE(details.efficiencyClassByCoreId.empty());            // No cpu_capacity: not known as hybrid
    EXPECT_FALSE(details.virtualizationFirmwareEnabled.has_value()); // Not a Linux fact
}

TEST(LinuxCpuDetailsTest, DualSocketSystem)
{
    // 2 sockets x 2 cores x 2 threads; core ids repeat on each socket
    const FixtureDir fixture;
    std::vector<std::string> blocks;
    for (std::size_t cpu = 0; cpu < 8; ++cpu)
    {
        const std::size_t socket = cpu / 4;
        const std::size_t core = (cpu / 2) % 2;
        blocks.push_back(processorBlock(cpu, socket, core, 2));
        const std::string siblings = std::format("{}-{}", cpu - (cpu % 2), cpu - (cpu % 2) + 1);
        fixture.writeCache(cpu, 0, 1, "Data", "48K", siblings);
        fixture.writeCache(cpu, 1, 1, "Instruction", "32K", siblings);
        fixture.writeCache(cpu, 2, 2, "Unified", "2048K", siblings);
        fixture.writeCache(cpu, 3, 3, "Unified", "16M", (socket == 0) ? "0-3" : "4-7");
    }
    fixture.writeCpuInfo(blocks);
    fixture.writeCpufreq("bios_limit", "2400000");
    fixture.writeCpufreq("cpuinfo_max_freq", "3900000");

    const CpuDetails details = LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs());
    EXPECT_EQ(details.sockets, 2U);
    EXPECT_EQ(details.physicalCores, 4U); // (socket, core) pairs, not the 2 distinct core ids
    EXPECT_EQ(details.logicalProcessors, 8U);
    EXPECT_EQ(details.l1CacheBytes, KIB * 4 * 80);
    EXPECT_EQ(details.l2CacheBytes, 8 * MIB);
    EXPECT_EQ(details.l3CacheBytes, 32 * MIB);      // One 16M L3 per socket
    EXPECT_FALSE(details.baseSpeedMHz.has_value()); // No base_frequency: bios_limit is a maximum, not the base
}

TEST(LinuxCpuDetailsTest, MissingCacheAndCpufreqDirectoriesStayUnknown)
{
    const FixtureDir fixture;
    fixture.writeCpuInfo({processorBlock(0, 0, 0, 2), processorBlock(1, 0, 1, 2)});
    std::filesystem::create_directories(fixture.cpuSysfs() / "cpu0"); // No cache/ or cpufreq/
    std::filesystem::create_directories(fixture.cpuSysfs() / "cpu1");

    const CpuDetails details = LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs());
    EXPECT_EQ(details.sockets, 1U);
    EXPECT_EQ(details.physicalCores, 2U);
    EXPECT_EQ(details.logicalProcessors, 2U);
    EXPECT_FALSE(details.l1CacheBytes.has_value());
    EXPECT_FALSE(details.l2CacheBytes.has_value());
    EXPECT_FALSE(details.l3CacheBytes.has_value());
    EXPECT_FALSE(details.baseSpeedMHz.has_value());
}

TEST(LinuxCpuDetailsTest, NothingReadableLeavesEveryFieldUnknown)
{
    const FixtureDir fixture; // Neither cpuinfo nor the sysfs tree exists
    EXPECT_EQ(LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs()), CpuDetails{});
}

TEST(LinuxCpuDetailsTest, HybridCoresAreCountedAndTheirCachesSummed)
{
    // 2 P-cores with SMT (cpu0-3, core ids 0 and 4) and 4 E-cores (cpu4-7, core ids 8-11), one socket.
    // P-cores have their own L2; the E-cores share one L2 per four-core cluster.
    const FixtureDir fixture;
    std::vector<std::string> blocks;
    for (std::size_t cpu = 0; cpu < 4; ++cpu)
    {
        blocks.push_back(processorBlock(cpu, 0, (cpu / 2) * 4, 6));
        const std::string siblings = std::format("{}-{}", cpu - (cpu % 2), cpu - (cpu % 2) + 1);
        fixture.writeCache(cpu, 0, 1, "Data", "48K", siblings);
        fixture.writeCache(cpu, 1, 1, "Instruction", "32K", siblings);
        fixture.writeCache(cpu, 2, 2, "Unified", "1280K", siblings);
        fixture.writeCache(cpu, 3, 3, "Unified", "12M", "0-7");
    }
    for (std::size_t cpu = 4; cpu < 8; ++cpu)
    {
        blocks.push_back(processorBlock(cpu, 0, cpu + 4, 6));
        const std::string self = std::format("{}", cpu);
        fixture.writeCache(cpu, 0, 1, "Data", "32K", self);
        fixture.writeCache(cpu, 1, 1, "Instruction", "64K", self);
        fixture.writeCache(cpu, 2, 2, "Unified", "2M", "4-7");
        fixture.writeCache(cpu, 3, 3, "Unified", "12M", "0-7");
    }
    fixture.writeCpuInfo(blocks);
    fixture.writeCpufreq("cpuinfo_max_freq", "4700000");
    for (std::size_t cpu = 0; cpu < 8; ++cpu)
    {
        FixtureDir::write(fixture.cpuSysfs() / std::format("cpu{}", cpu) / "cpu_capacity", (cpu < 4) ? "1024\n" : "442\n");
    }

    const CpuDetails details = LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs());
    EXPECT_EQ(details.sockets, 1U);
    EXPECT_EQ(details.physicalCores, 6U);
    EXPECT_EQ(details.logicalProcessors, 8U);
    // /proc/cpuinfo does not say which cores are which: counted, not split
    EXPECT_FALSE(details.performanceCores.has_value());
    EXPECT_FALSE(details.efficiencyCores.has_value());
    EXPECT_EQ(details.l1CacheBytes, (KIB * 2 * 80) + (KIB * 4 * 96));
    EXPECT_EQ(details.l2CacheBytes, (KIB * 2 * 1280) + (2 * MIB));
    EXPECT_EQ(details.l3CacheBytes, 12 * MIB);
    EXPECT_FALSE(details.baseSpeedMHz.has_value()); // cpuinfo_max_freq is the boost clock, not the base
    // cpu_capacity ranks each logical processor: P-cores 1, E-cores 0
    EXPECT_EQ(details.efficiencyClassByCoreId, (std::vector<std::uint8_t>{1, 1, 1, 1, 0, 0, 0, 0}));
}

TEST(LinuxCpuDetailsTest, BaseSpeedIsTheHighestPolicysBaseFrequency)
{
    // A hybrid CPU: cpu0 sits in the E-core policy, whose base is lower than the P-cores'
    const FixtureDir fixture;
    FixtureDir::write(fixture.cpuSysfs() / "cpufreq" / "policy0" / "base_frequency", "1500000\n");
    FixtureDir::write(fixture.cpuSysfs() / "cpufreq" / "policy4" / "base_frequency", "2000000\n");
    fixture.writeCpufreq("base_frequency", "1500000");
    EXPECT_EQ(LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs()).baseSpeedMHz, 2000U);
}

TEST(LinuxCpuDetailsTest, BaseSpeedFromASinglePolicy)
{
    const FixtureDir fixture;
    FixtureDir::write(fixture.cpuSysfs() / "cpufreq" / "policy0" / "base_frequency", "3200000\n");
    EXPECT_EQ(LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs()).baseSpeedMHz, 3200U);
}

TEST(LinuxCpuDetailsTest, NoPolicyBaseFrequencyLeavesBaseSpeedUnknown)
{
    const FixtureDir fixture;
    FixtureDir::write(fixture.cpuSysfs() / "cpufreq" / "policy0" / "cpuinfo_max_freq", "4800000\n");
    FixtureDir::write(fixture.cpuSysfs() / "cpufreq" / "policy0" / "bios_limit", "4800000\n");
    EXPECT_FALSE(LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs()).baseSpeedMHz.has_value());
}

TEST(LinuxCpuDetailsTest, AProcessorWithoutAPhysicalIdLeavesSocketsAndCoresUnknown)
{
    // Counting only the processors that list one would undercount both (#809 review)
    std::string text = processorBlock(0, 0, 0) + "\n" + processorBlock(1, 0, 1) + "\n" + processorBlock(2, std::nullopt, 2) + "\n";
    CpuDetails details;
    LinuxCpuDetails::parseCpuInfoTopology(text, details);
    EXPECT_EQ(details.logicalProcessors, 3U);
    EXPECT_FALSE(details.sockets.has_value());
    EXPECT_FALSE(details.physicalCores.has_value());
}

TEST(LinuxCpuDetailsTest, OfflineCpusInSysfsAreLeftOut)
{
    // sysfs lists cpu3, but /proc/cpuinfo (the online CPUs) does not: its private caches and its
    // capacity describe a CPU outside the set the topology counts (#809 review)
    const FixtureDir fixture;
    fixture.writeCpuInfo({processorBlock(0, 0, 0), processorBlock(1, 0, 1), processorBlock(2, 0, 2)});
    for (std::size_t cpu = 0; cpu < 4; ++cpu)
    {
        fixture.writeCache(cpu, 0, 2, "Unified", "1M", std::format("{}", cpu));
        FixtureDir::write(fixture.cpuSysfs() / std::format("cpu{}", cpu) / "cpu_capacity", (cpu == 3) ? "512\n" : "1024\n");
    }
    std::vector<std::size_t> online;
    const CpuDetails details = LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs(), &online);
    EXPECT_EQ(online, (std::vector<std::size_t>{0, 1, 2})); // The set the details describe
    EXPECT_EQ(details.logicalProcessors, 3U);
    EXPECT_EQ(details.l2CacheBytes, 3 * MIB);             // cpu3's L2 is not counted
    EXPECT_TRUE(details.efficiencyClassByCoreId.empty()); // Without cpu3, one capacity: not hybrid
}

TEST(LinuxCpuDetailsTest, CachesWithoutASharedListAreTakenAsPrivate)
{
    const FixtureDir fixture;
    fixture.writeCpuInfo({processorBlock(0, 0, 0), processorBlock(1, 0, 1)});
    fixture.writeCache(0, 0, 2, "Unified", "1M", "");
    fixture.writeCache(1, 0, 2, "Unified", "1M", "");
    const CpuDetails details = LinuxCpuDetails::read(fixture.proc(), fixture.cpuSysfs());
    EXPECT_EQ(details.l2CacheBytes, 2 * MIB);
}

} // namespace
} // namespace Platform
