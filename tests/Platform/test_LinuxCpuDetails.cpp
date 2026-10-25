/// @file test_LinuxCpuDetails.cpp
/// @brief Tests for Platform::LinuxCpuDetails (#809): /proc/cpuinfo topology, sysfs cache totals,
/// the base clock and virtualization status (#2592), against fixture proc/sys trees. LinuxCpuDetails.h uses only the standard library,
/// so these build and run on every platform.

#include "Platform/CpuDetails.h"
#include "Platform/Linux/LinuxCpuDetails.h"

#include <gtest/gtest.h>

#include <array>
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

    [[nodiscard]] std::filesystem::path root() const
    {
        return m_Path;
    }
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
    EXPECT_FALSE(details.virtualizationFirmwareEnabled.has_value()); // readVirtualization()'s, not read()'s (#2592)
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
    const std::string text = processorBlock(0, 0, 0) + "\n" + processorBlock(1, 0, 1) + "\n" + processorBlock(2, std::nullopt, 2) + "\n";
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

TEST(LinuxCpuDetailsTest, GigabyteCacheSizesAndTrailingJunkParse)
{
    EXPECT_EQ(LinuxCpuDetails::parseCacheSize("2G"), 2 * MIB * 1024U);
    EXPECT_EQ(LinuxCpuDetails::parseCacheSize("1g"), MIB * 1024U);
    EXPECT_FALSE(LinuxCpuDetails::Detail::parseUnsigned("12x").has_value());
    EXPECT_EQ(LinuxCpuDetails::Detail::parseUnsigned(" 12 \n"), 12U);
}

TEST(LinuxCpuDetailsTest, CpuInfoLinesBeforeTheFirstProcessorAreIgnored)
{
    // Some kernels (ARM) open /proc/cpuinfo with machine-wide lines before any "processor".
    const auto processors = LinuxCpuDetails::parseCpuInfoProcessors("Hardware\t: BCM2835\nphysical id\t: 7\n\nprocessor\t: 0\n");
    ASSERT_EQ(processors.size(), 1U);
    EXPECT_EQ(processors[0].processor, 0U);
    EXPECT_FALSE(processors[0].physicalId.has_value()); // the stray line before it isn't taken as its
}

TEST(LinuxCpuDetailsTest, AProcessorWithoutANumberMakesTheOnlineSetUnknown)
{
    const auto processors = LinuxCpuDetails::parseCpuInfoProcessors("processor\t: 0\n\nprocessor\t: ?\n");
    ASSERT_EQ(processors.size(), 2U);
    EXPECT_FALSE(LinuxCpuDetails::onlineCpus(processors).has_value()); // every cpuN is then taken
}

TEST(LinuxCpuDetailsTest, ZeroSizedAndUnreadableCacheEntriesAreSkipped)
{
    const std::vector<LinuxCpuDetails::SysfsCacheEntry> entries{
        {.cpu = 0, .level = 1, .type = "Data", .bytes = 0, .sharedCpuList = "0"},
        {.cpu = 0, .level = 2, .type = "Unified", .bytes = MIB, .sharedCpuList = "0"},
    };
    EXPECT_EQ(LinuxCpuDetails::distinctCacheInstances(entries).size(), 1U);

    // In sysfs: an index without a level, and a file beside the index directories, are passed over.
    const FixtureDir fixture;
    fixture.writeCache(0, 0, 2, "Unified", "1M", "0");
    FixtureDir::write(fixture.cpuSysfs() / "cpu0" / "cache" / "index1" / "size", "32K\n");
    FixtureDir::write(fixture.cpuSysfs() / "cpu0" / "cache" / "uevent", "\n");
    const auto read = LinuxCpuDetails::readCacheEntries(fixture.cpuSysfs(), std::nullopt);
    ASSERT_EQ(read.size(), 1U);
    EXPECT_EQ(read[0].level, 2U);
    EXPECT_EQ(read[0].bytes, MIB);
}

TEST(LinuxCpuDetailsTest, CpuModelIsTheX86ModelName)
{
    EXPECT_EQ(LinuxCpuDetails::cpuModelFromCpuInfo("processor\t: 0\nvendor_id\t: GenuineIntel\n"
                                                   "model name\t: Intel(R) Core(TM) i7-1185G7 @ 3.00GHz  \n\n"
                                                   "processor\t: 1\nmodel name\t: ignored\n"),
              "Intel(R) Core(TM) i7-1185G7 @ 3.00GHz");
}

TEST(LinuxCpuDetailsTest, Arm64CpuModelComesFromTheImplementerAndPart)
{
    // #2214: arm64 /proc/cpuinfo has no "model name" (this is an Azure Cobalt 100, Neoverse N2).
    constexpr std::string_view CPUINFO = "processor\t: 0\nBogoMIPS\t: 2000.00\nCPU implementer\t: 0x41\n"
                                         "CPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0xd49\nCPU revision\t: 0\n\n"
                                         "processor\t: 1\nCPU implementer\t: 0x41\nCPU part\t: 0xd49\n";
    EXPECT_EQ(LinuxCpuDetails::cpuModelFromCpuInfo(CPUINFO), "ARM Neoverse-N2");
}

TEST(LinuxCpuDetailsTest, Arm64BigLittleListsEachCoreTypeOnceAndUnknownCodesByNumber)
{
    constexpr std::string_view CPUINFO = "processor\t: 0\nCPU implementer\t: 0x41\nCPU part\t: 0xd05\n\n"
                                         "processor\t: 1\nCPU implementer\t: 0x41\nCPU part\t: 0xd05\n\n"
                                         "processor\t: 2\nCPU implementer\t: 0x41\nCPU part\t: 0xd0b\n\n"
                                         "processor\t: 3\nCPU implementer\t: 0x51\nCPU part\t: 0x001\n\n"
                                         "processor\t: 4\nCPU implementer\t: 0x99\nCPU part\t: 0xabc\n\n"
                                         "processor\t: 5\nCPU implementer\t: 0x41\nCPU part\t: 0xfff\n";
    EXPECT_EQ(LinuxCpuDetails::cpuModelFromCpuInfo(CPUINFO),
              "ARM Cortex-A55 + ARM Cortex-A76 + Qualcomm part 0x001 + Arm implementer 0x99 part 0xabc + ARM part 0xfff");
}

/// The model cpuModelFromCpuInfo() builds for one core with these implementer and part lines.
[[nodiscard]] std::string armModel(std::string_view implementer, std::string_view part)
{
    return LinuxCpuDetails::cpuModelFromCpuInfo(std::format("processor\t: 0\nCPU implementer\t: {}\nCPU part\t: {}\n", implementer, part));
}

TEST(LinuxCpuDetailsTest, EveryKnownArmImplementerIsNamed)
{
    struct Case
    {
        std::string_view code;
        std::string_view vendor;
    };
    constexpr auto CASES = std::to_array<Case>({
        {.code = "0x41", .vendor = "ARM"},
        {.code = "0x42", .vendor = "Broadcom"},
        {.code = "0x43", .vendor = "Cavium"},
        {.code = "0x46", .vendor = "Fujitsu"},
        {.code = "0x48", .vendor = "HiSilicon"},
        {.code = "0x4e", .vendor = "NVIDIA"},
        {.code = "0x50", .vendor = "APM"},
        {.code = "0x51", .vendor = "Qualcomm"},
        {.code = "0x53", .vendor = "Samsung"},
        {.code = "0x61", .vendor = "Apple"},
        {.code = "0x6d", .vendor = "Microsoft"},
        {.code = "0xc0", .vendor = "Ampere"},
    });
    for (const Case& c : CASES)
    {
        // A part code no table names, so only the vendor is looked up (Arm Ltd's own part 0xfff included).
        EXPECT_EQ(armModel(c.code, "0xfff"), std::format("{} part 0xfff", c.vendor)) << c.code;
    }
}

TEST(LinuxCpuDetailsTest, EveryKnownArmLtdCoreIsNamed)
{
    struct Case
    {
        std::string_view code;
        std::string_view core;
    };
    constexpr auto CASES = std::to_array<Case>({
        {.code = "0xd03", .core = "Cortex-A53"},  {.code = "0xd05", .core = "Cortex-A55"},  {.code = "0xd07", .core = "Cortex-A57"},
        {.code = "0xd08", .core = "Cortex-A72"},  {.code = "0xd0a", .core = "Cortex-A75"},  {.code = "0xd0b", .core = "Cortex-A76"},
        {.code = "0xd0c", .core = "Neoverse-N1"}, {.code = "0xd0d", .core = "Cortex-A77"},  {.code = "0xd40", .core = "Neoverse-V1"},
        {.code = "0xd41", .core = "Cortex-A78"},  {.code = "0xd44", .core = "Cortex-X1"},   {.code = "0xd46", .core = "Cortex-A510"},
        {.code = "0xd47", .core = "Cortex-A710"}, {.code = "0xd48", .core = "Cortex-X2"},   {.code = "0xd49", .core = "Neoverse-N2"},
        {.code = "0xd4f", .core = "Neoverse-V2"}, {.code = "0xd80", .core = "Cortex-A520"}, {.code = "0xd81", .core = "Cortex-A720"},
        {.code = "0xd82", .core = "Cortex-X4"},   {.code = "0xd84", .core = "Neoverse-V3"}, {.code = "0xd8e", .core = "Neoverse-N3"},
    });
    for (const Case& c : CASES)
    {
        EXPECT_EQ(armModel("0x41", c.code), std::format("ARM {}", c.core)) << c.code;
    }
    // The part table is Arm Ltd's: another vendor's same code is shown by number.
    EXPECT_EQ(armModel("0x51", "0xd49"), "Qualcomm part 0xd49");
}

TEST(LinuxCpuDetailsTest, AnUnreadableArmCodeNamesNoCore)
{
    // A part that isn't hex is skipped; the Hardware line is the fallback.
    EXPECT_EQ(LinuxCpuDetails::cpuModelFromCpuInfo("processor\t: 0\nCPU implementer\t: 0x41\nCPU part\t: zz\nHardware\t: SoC\n"), "SoC");
    // An implementer that isn't hex (or is empty) leaves no implementer, so the part line is ignored.
    EXPECT_EQ(armModel("0xqq", "0xd49"), "");
    EXPECT_EQ(armModel("0x", "0xd49"), "");
    EXPECT_EQ(armModel("0x41", "0xd49 trailing"), "");
    // An upper-case prefix reads the same.
    EXPECT_EQ(armModel("0X41", "0XD49"), "ARM Neoverse-N2");
}

TEST(LinuxCpuDetailsTest, CpuModelFallsBackToHardwareThenEmpty)
{
    EXPECT_EQ(LinuxCpuDetails::cpuModelFromCpuInfo("processor\t: 0\nHardware\t: BCM2835\n"), "BCM2835");
    EXPECT_EQ(LinuxCpuDetails::cpuModelFromCpuInfo("processor\t: 0\nCPU part\t: 0xd49\nmodel name\t:   \n"), "");
    EXPECT_EQ(LinuxCpuDetails::cpuModelFromCpuInfo(""), "");
}

// -----------------------------------------------------------------------------
// Virtualization status (#2592)
// -----------------------------------------------------------------------------

/// A /proc/cpuinfo with two processor blocks sharing `flags` (and `vmxFlags`, when not empty).
[[nodiscard]] std::string cpuInfoWithFlags(std::string_view flags, std::string_view vmxFlags = {})
{
    std::string text;
    for (int cpu = 0; cpu < 2; ++cpu)
    {
        text += std::format("processor\t: {}\nvendor_id\t: Test\nflags\t\t: {}\n", cpu, flags);
        if (!vmxFlags.empty())
        {
            text += std::format("vmx flags\t: {}\n", vmxFlags);
        }
        text += "bugs\t\t: spectre_v1\n\n";
    }
    return text;
}

TEST(LinuxCpuDetailsTest, VirtualizationFlagsWithVmx)
{
    // Kernel 5.8+: EPT on its own "vmx flags" line
    const auto flags = LinuxCpuDetails::parseCpuVirtualizationFlags(
        cpuInfoWithFlags("fpu vme de pse tsc msr vmx sse2 ht", "vnmi preemption_timer ept vpid"));
    EXPECT_TRUE(flags.hasFlags);
    EXPECT_TRUE(flags.vmx);
    EXPECT_FALSE(flags.svm);
    EXPECT_TRUE(flags.slat);
    EXPECT_FALSE(flags.hypervisor);

    // An older kernel lists ept on the "flags" line; vmx as the last flag, before a CR
    const auto older = LinuxCpuDetails::parseCpuVirtualizationFlags("processor\t: 0\nflags\t\t: fpu ept vmx\r\n");
    EXPECT_TRUE(older.vmx);
    EXPECT_TRUE(older.slat);

    // Bare metal: no hypervisor, so firmware-enabled VT-x and its EPT
    const auto summary = LinuxCpuDetails::summarizeVirtualization(flags, false, false);
    EXPECT_EQ(summary.hypervisorPresent, false);
    EXPECT_EQ(summary.firmwareEnabled, true);
    EXPECT_EQ(summary.slatSupported, true);
    EXPECT_EQ(summary.kvmAvailable, false);
}

TEST(LinuxCpuDetailsTest, VirtualizationFlagsWithSvm)
{
    const auto flags = LinuxCpuDetails::parseCpuVirtualizationFlags(cpuInfoWithFlags("fpu vme de svm extapic npt lbrv"));
    EXPECT_TRUE(flags.hasFlags);
    EXPECT_FALSE(flags.vmx);
    EXPECT_TRUE(flags.svm);
    EXPECT_TRUE(flags.slat);
    EXPECT_FALSE(flags.hypervisor);

    // svm is listed even when the firmware disables it: only /dev/kvm settles it
    const auto withoutKvm = LinuxCpuDetails::summarizeVirtualization(flags, false, false);
    EXPECT_FALSE(withoutKvm.firmwareEnabled.has_value());
    EXPECT_EQ(withoutKvm.slatSupported, true);
    EXPECT_EQ(withoutKvm.hypervisorPresent, false);
    const auto withKvm = LinuxCpuDetails::summarizeVirtualization(flags, false, true);
    EXPECT_EQ(withKvm.firmwareEnabled, true);
    EXPECT_EQ(withKvm.kvmAvailable, true);
}

TEST(LinuxCpuDetailsTest, VirtualizationFlagsWithNeither)
{
    // "vmxx", "svm_lock" and "nept" are not the flags themselves
    const auto flags = LinuxCpuDetails::parseCpuVirtualizationFlags(cpuInfoWithFlags("fpu vmxx svm_lock nept sse2"));
    EXPECT_TRUE(flags.hasFlags);
    EXPECT_FALSE(flags.vmx);
    EXPECT_FALSE(flags.svm);
    EXPECT_FALSE(flags.slat);

    // x86 bare metal: no VT-x/AMD-V (absent, or off in firmware); SLAT unknown
    const auto bareMetal = LinuxCpuDetails::summarizeVirtualization(flags, false, false);
    EXPECT_EQ(bareMetal.firmwareEnabled, false);
    EXPECT_FALSE(bareMetal.slatSupported.has_value());
    EXPECT_EQ(bareMetal.hypervisorPresent, false);

    // Arm: no x86 flags line, so neither the extensions nor a missing hypervisor bit say anything
    const auto arm = LinuxCpuDetails::parseCpuVirtualizationFlags("processor\t: 0\nFeatures\t: fp asimd evtstrm\n");
    EXPECT_FALSE(arm.hasFlags);
    const auto armSummary = LinuxCpuDetails::summarizeVirtualization(arm, false, false);
    EXPECT_FALSE(armSummary.firmwareEnabled.has_value());
    EXPECT_FALSE(armSummary.hypervisorPresent.has_value());
    // ...unless KVM registered /dev/kvm, which it does only with usable extensions
    EXPECT_EQ(LinuxCpuDetails::summarizeVirtualization(arm, false, true).firmwareEnabled, true);

    // Only the first processor block is read; nothing at all leaves everything but KVM unknown
    EXPECT_FALSE(LinuxCpuDetails::parseCpuVirtualizationFlags("processor\t: 0\n\nprocessor\t: 1\nflags\t: vmx\n").vmx);
    const auto empty = LinuxCpuDetails::summarizeVirtualization(LinuxCpuDetails::parseCpuVirtualizationFlags(""), false, false);
    EXPECT_FALSE(empty.firmwareEnabled.has_value());
    EXPECT_FALSE(empty.hypervisorPresent.has_value());
    EXPECT_FALSE(empty.slatSupported.has_value());
    EXPECT_EQ(empty.kvmAvailable, false);
}

TEST(LinuxCpuDetailsTest, VirtualizationUnderAHypervisor)
{
    // A guest without nested virtualization: the CPUID hypervisor bit, no vmx/svm
    const auto guest = LinuxCpuDetails::parseCpuVirtualizationFlags(cpuInfoWithFlags("fpu vme hypervisor sse2"));
    EXPECT_TRUE(guest.hypervisor);
    const auto summary = LinuxCpuDetails::summarizeVirtualization(guest, false, false);
    EXPECT_EQ(summary.hypervisorPresent, true);
    EXPECT_FALSE(summary.firmwareEnabled.has_value()) << "a guest's missing vmx/svm says nothing about the host's firmware";
    EXPECT_FALSE(summary.slatSupported.has_value());

    // A hypervisor named by DMI or WSL's kernel, without the CPUID bit
    const auto plain = LinuxCpuDetails::parseCpuVirtualizationFlags(cpuInfoWithFlags("fpu vme sse2"));
    EXPECT_EQ(LinuxCpuDetails::summarizeVirtualization(plain, true, false).hypervisorPresent, true);
    EXPECT_FALSE(LinuxCpuDetails::summarizeVirtualization(plain, true, false).firmwareEnabled.has_value());
    // WSL 1 (runsUnderHypervisor() unknown): neither present nor absent
    EXPECT_FALSE(LinuxCpuDetails::summarizeVirtualization(plain, std::nullopt, false).hypervisorPresent.has_value());
    EXPECT_FALSE(LinuxCpuDetails::summarizeVirtualization(plain, std::nullopt, false).firmwareEnabled.has_value());

    // Nested virtualization exposed: vmx, but no EPT for this guest
    const auto nested = LinuxCpuDetails::parseCpuVirtualizationFlags(cpuInfoWithFlags("fpu vmx hypervisor"));
    const auto nestedSummary = LinuxCpuDetails::summarizeVirtualization(nested, true, true);
    EXPECT_EQ(nestedSummary.hypervisorPresent, true);
    EXPECT_EQ(nestedSummary.firmwareEnabled, true);
    EXPECT_EQ(nestedSummary.slatSupported, false);
    EXPECT_EQ(nestedSummary.kvmAvailable, true);
}

TEST(LinuxCpuDetailsTest, ReadVirtualizationFindsDevKvm)
{
    const FixtureDir fixture;
    FixtureDir::write(fixture.proc() / "cpuinfo", cpuInfoWithFlags("fpu svm npt"));
    const auto absent = LinuxCpuDetails::readVirtualization(fixture.proc(), fixture.root());
    EXPECT_EQ(absent.kvmAvailable, false);
    EXPECT_FALSE(absent.firmwareEnabled.has_value()); // svm without kvm_amd loaded
    EXPECT_EQ(absent.hypervisorPresent, false);

    FixtureDir::write(fixture.root() / "dev" / "kvm", "");
    const auto present = LinuxCpuDetails::readVirtualization(fixture.proc(), fixture.root());
    EXPECT_EQ(present.kvmAvailable, true);
    EXPECT_EQ(present.firmwareEnabled, true);
    EXPECT_EQ(present.slatSupported, true);

    // applyVirtualization() copies all four onto the details read() fills
    CpuDetails details;
    LinuxCpuDetails::applyVirtualization(present, details);
    EXPECT_EQ(details.kvmAvailable, true);
    EXPECT_EQ(details.virtualizationFirmwareEnabled, true);
    EXPECT_EQ(details.slatSupported, true);
    EXPECT_EQ(details.hypervisorPresent, false);
    EXPECT_FALSE(details.vbsRunning.has_value()); // Windows only (#809)
    EXPECT_FALSE(details.hvciEnabled.has_value());
}

TEST(LinuxCpuDetailsTest, ReadVirtualizationReusesTheOsInfoHypervisorDetection)
{
    // LinuxOsInfo's DMI vendor names the hypervisor though this CPU lists no hypervisor bit
    const FixtureDir fixture;
    FixtureDir::write(fixture.proc() / "cpuinfo", cpuInfoWithFlags("fpu vme sse2"));
    EXPECT_EQ(LinuxCpuDetails::readVirtualization(fixture.proc(), fixture.root()).hypervisorPresent, false);
    FixtureDir::write(fixture.root() / "sys" / "class" / "dmi" / "id" / "sys_vendor", "QEMU\n");
    const auto vm = LinuxCpuDetails::readVirtualization(fixture.proc(), fixture.root());
    EXPECT_EQ(vm.hypervisorPresent, true);
    EXPECT_FALSE(vm.firmwareEnabled.has_value());
}

} // namespace
} // namespace Platform
