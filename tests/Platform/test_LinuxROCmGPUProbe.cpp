#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/AmdApu.h"
#include "Platform/Linux/ROCmGPUProbe.h"
#include "Platform/Linux/ROCmGPUProbeMath.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <vector>

#include <dlfcn.h>

namespace Platform
{
namespace
{

// ROCmGPUProbeMath: pure logic exercised with fake function pointers, covering error and
// missing-symbol branches the fixed-behavior mock library (tests/Mocks/ROCmMock.cpp) never
// takes -- it always succeeds, so these paths are otherwise unreachable via ROCmGPUProbe itself.

TEST(ROCmGPUProbeMathTest, ResolveErrorStringReturnsLibraryStringWhenAvailable)
{
    const auto result = ROCmGPUProbeMath::resolveErrorString(7, [](std::uint32_t) -> const char* { return "Input out of bounds"; });
    EXPECT_EQ(result, "Input out of bounds");
}

TEST(ROCmGPUProbeMathTest, ResolveErrorStringFallsBackWhenFunctionPointerIsNull)
{
    EXPECT_EQ(ROCmGPUProbeMath::resolveErrorString(3, nullptr), "Unknown ROCm error 3");
}

TEST(ROCmGPUProbeMathTest, ResolveErrorStringFallsBackWhenLibraryReturnsNull)
{
    const auto result = ROCmGPUProbeMath::resolveErrorString(3, [](std::uint32_t) -> const char* { return nullptr; });
    EXPECT_EQ(result, "Unknown ROCm error 3");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdPrefersUniqueId)
{
    const auto uniqueIdFn = [](std::uint32_t, std::uint64_t* out) -> std::uint32_t
    {
        *out = 4001;
        return ROCmGPUProbeMath::kRsmiStatusSuccess;
    };
    const auto pciIdFn = [](std::uint32_t, std::uint64_t* out) -> std::uint32_t
    {
        *out = 9001;
        return ROCmGPUProbeMath::kRsmiStatusSuccess;
    };
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(0, uniqueIdFn, pciIdFn), "4001");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdFallsBackToPciIdWhenUniqueIdUnavailable)
{
    const auto pciIdFn = [](std::uint32_t, std::uint64_t* out) -> std::uint32_t
    {
        *out = 9001;
        return ROCmGPUProbeMath::kRsmiStatusSuccess;
    };
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(1, nullptr, pciIdFn), "9001");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdFallsBackToPciIdWhenUniqueIdFails)
{
    const auto uniqueIdFn = [](std::uint32_t, std::uint64_t*) -> std::uint32_t
    {
        return ROCmGPUProbeMath::kRsmiStatusSuccess + 1; // any non-success status
    };
    const auto pciIdFn = [](std::uint32_t, std::uint64_t* out) -> std::uint32_t
    {
        *out = 9001;
        return ROCmGPUProbeMath::kRsmiStatusSuccess;
    };
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(1, uniqueIdFn, pciIdFn), "9001");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdFallsBackToAmdIndexWhenBothLookupsUnavailable)
{
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(2, nullptr, nullptr), "amd_2");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdFallsBackToAmdIndexWhenBothLookupsFail)
{
    const auto alwaysFails = [](std::uint32_t, std::uint64_t*) -> std::uint32_t
    {
        return ROCmGPUProbeMath::kRsmiStatusSuccess + 1;
    };
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(2, alwaysFails, alwaysFails), "amd_2");
}

// rsmi_frequencies_t layouts (#1088): fill the buffer exactly as each library version writes it.

template<typename Layout> ROCmGPUProbeMath::RsmiFrequenciesBuffer bufferFrom(const Layout& layout)
{
    ROCmGPUProbeMath::RsmiFrequenciesBuffer buffer;
    std::memcpy(buffer.bytes.data(), &layout, sizeof(layout));
    return buffer;
}

TEST(ROCmGPUProbeMathTest, FrequenciesLayoutFollowsLibraryMajorVersion)
{
    using ROCmGPUProbeMath::FrequenciesLayout;
    EXPECT_EQ(ROCmGPUProbeMath::frequenciesLayoutFor(5U), FrequenciesLayout::V5);
    EXPECT_EQ(ROCmGPUProbeMath::frequenciesLayoutFor(6U), FrequenciesLayout::V6);
    EXPECT_EQ(ROCmGPUProbeMath::frequenciesLayoutFor(7U), FrequenciesLayout::V6);
    // Unknown version: decode as the current layout (the buffer is oversized either way).
    EXPECT_EQ(ROCmGPUProbeMath::frequenciesLayoutFor(std::nullopt), FrequenciesLayout::V6);
}

TEST(ROCmGPUProbeMathTest, DecodesRocm6LayoutWithDeepSleepFlag)
{
    ROCmGPUProbeMath::RsmiFrequenciesV6 v6{};
    v6.has_deep_sleep = true;
    v6.num_supported = 33;
    v6.current = 32; // the deep-sleep slot, beyond the old 32-entry array
    v6.frequency[32] = 400'000'000ULL;
    const auto buffer = bufferFrom(v6);

    EXPECT_EQ(ROCmGPUProbeMath::currentFrequencyHz(buffer, ROCmGPUProbeMath::FrequenciesLayout::V6), 400'000'000ULL);
}

TEST(ROCmGPUProbeMathTest, DecodesRocm5Layout)
{
    ROCmGPUProbeMath::RsmiFrequenciesV5 v5{};
    v5.num_supported = 3;
    v5.current = 2;
    v5.frequency[2] = 2'100'000'000ULL;
    const auto buffer = bufferFrom(v5);

    EXPECT_EQ(ROCmGPUProbeMath::currentFrequencyHz(buffer, ROCmGPUProbeMath::FrequenciesLayout::V5), 2'100'000'000ULL);
}

// The reported version is only a preference: a library built without git-tag metadata reports a
// generated 1.0.0 while writing the ROCm 6 layout, and the clocks must still decode (#1189 review).
TEST(ROCmGPUProbeMathTest, DecodesRocm6LayoutWhenVersionSuggestsRocm5)
{
    ROCmGPUProbeMath::RsmiFrequenciesV6 v6{};
    v6.has_deep_sleep = true;
    v6.num_supported = 8;
    v6.current = 5;
    v6.frequency[5] = 1'800'000'000ULL;

    const auto preferred = ROCmGPUProbeMath::frequenciesLayoutFor(1U);
    ASSERT_EQ(preferred, ROCmGPUProbeMath::FrequenciesLayout::V5);
    EXPECT_EQ(ROCmGPUProbeMath::currentFrequencyHz(bufferFrom(v6), preferred), 1'800'000'000ULL);
}

TEST(ROCmGPUProbeMathTest, DecodesRocm5LayoutWhenVersionSuggestsRocm6)
{
    ROCmGPUProbeMath::RsmiFrequenciesV5 v5{};
    v5.num_supported = 4;
    v5.current = 3;
    v5.frequency[3] = 900'000'000ULL;

    EXPECT_EQ(ROCmGPUProbeMath::currentFrequencyHz(bufferFrom(v5), ROCmGPUProbeMath::FrequenciesLayout::V6), 900'000'000ULL);
}

TEST(ROCmGPUProbeMathTest, RejectsZeroFrequencyInEitherLayout)
{
    ROCmGPUProbeMath::RsmiFrequenciesV6 v6{};
    v6.num_supported = 2;
    v6.current = 1; // frequency[1] left at 0
    EXPECT_FALSE(ROCmGPUProbeMath::currentFrequencyHz(bufferFrom(v6), ROCmGPUProbeMath::FrequenciesLayout::V6).has_value());
}

TEST(ROCmGPUProbeMathTest, RejectsCurrentIndexOutsideSupportedOrArray)
{
    ROCmGPUProbeMath::RsmiFrequenciesV6 notSupported{};
    notSupported.num_supported = 2;
    notSupported.current = 2;
    EXPECT_FALSE(ROCmGPUProbeMath::currentFrequencyHz(bufferFrom(notSupported), ROCmGPUProbeMath::FrequenciesLayout::V6).has_value());

    // A library reporting more supported entries than the array holds must not index past it.
    ROCmGPUProbeMath::RsmiFrequenciesV5 pastArray{};
    pastArray.num_supported = 1000;
    pastArray.current = 32;
    EXPECT_FALSE(ROCmGPUProbeMath::currentFrequencyHz(bufferFrom(pastArray), ROCmGPUProbeMath::FrequenciesLayout::V5).has_value());
}

// #1162: device ids are resolved once at load. A lookup failing afterwards must not turn a GPU into
// a different id for a sample, and sampling makes no further id lookups.
TEST(LinuxROCmGPUProbeTest, DeviceIdsAreResolvedOnceAtLoad)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    void* library = dlopen("librocm_smi64.so.6", RTLD_NOW);
    ASSERT_NE(library, nullptr);
    // dlsym returns void* by POSIX definition; the casts restore the mock's signatures.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto configure = reinterpret_cast<void (*)(int)>(dlsym(library, "tasksmackRocmMockConfigure"));
    const auto idCalls = reinterpret_cast<unsigned int (*)()>(dlsym(library, "tasksmackRocmMockIdCalls"));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    ASSERT_NE(configure, nullptr);
    ASSERT_NE(idCalls, nullptr);

    configure(-1);
    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());
    const auto expectedIds = probe.enumerateGPUs();
    const unsigned int callsAtLoad = idCalls();
    configure(0); // every id lookup from now on fails

    const auto counters = probe.readGPUCounters();
    const auto gpus = probe.enumerateGPUs();
    const unsigned int callsWhileSampling = idCalls(); // configure(0) reset the count
    configure(-1);
    dlclose(library);

    EXPECT_EQ(callsWhileSampling, 0U); // sampling makes no further id lookups

    ASSERT_EQ(counters.size(), expectedIds.size());
    ASSERT_EQ(gpus.size(), expectedIds.size());
    for (std::size_t i = 0; i < expectedIds.size(); ++i)
    {
        EXPECT_EQ(counters[i].gpuId, expectedIds[i].id);
        EXPECT_EQ(gpus[i].id, expectedIds[i].id);
    }
    EXPECT_GT(callsAtLoad, 0U);
}

// #1111: a sensor read that fails (busy, GPU reset) is marked unread, not reported as a real 0, and
// the reads that still succeed stay available.
TEST(LinuxROCmGPUProbeTest, FailedSensorReadsAreMarkedUnavailable)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    void* library = dlopen("librocm_smi64.so.6", RTLD_NOW);
    ASSERT_NE(library, nullptr);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- dlsym returns void* by POSIX definition
    const auto failSensorReads = reinterpret_cast<void (*)(int)>(dlsym(library, "tasksmackRocmMockFailSensorReads"));
    ASSERT_NE(failSensorReads, nullptr);

    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());
    const auto good = probe.readGPUCounters();
    failSensorReads(1);
    const auto failed = probe.readGPUCounters();
    failSensorReads(0);
    dlclose(library);

    ASSERT_FALSE(good.empty());
    EXPECT_TRUE(good[0].utilizationAvailable);
    EXPECT_TRUE(good[0].memoryAvailable);
    EXPECT_TRUE(good[0].temperatureAvailable);
    EXPECT_TRUE(good[0].powerAvailable);
    EXPECT_TRUE(good[0].gpuClockAvailable);

    ASSERT_EQ(failed.size(), good.size());
    EXPECT_FALSE(failed[0].utilizationAvailable);
    EXPECT_FALSE(failed[0].memoryAvailable);
    EXPECT_FALSE(failed[0].temperatureAvailable);
    EXPECT_FALSE(failed[0].powerAvailable);
    EXPECT_FALSE(failed[0].gpuClockAvailable);
    EXPECT_GT(good[0].fanSpeedMaxRaw, 0U);
    EXPECT_EQ(failed[0].fanSpeedRaw, good[0].fanSpeedRaw); // reads that still succeed are unaffected
}

TEST(LinuxROCmGPUProbeTest, BasicOperationsDoNotThrow)
{
    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxROCmGPUProbeTest, UnavailableProbeReportsNoCapabilities)
{
    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    if (probe.isAvailable())
    {
        // Under CTest (ENVIRONMENT_MODIFICATION), LD_LIBRARY_PATH is prepended with the
        // mock library so the probe is always available. The unavailable path cannot be
        // exercised in this process.
        GTEST_SKIP() << "ROCm probe is available (mock library loaded); unavailable path not testable in this environment";
    }

    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasClockSpeeds);
    EXPECT_FALSE(caps.hasFanSpeed);
    EXPECT_FALSE(caps.hasPerProcessMetrics);
}

TEST(LinuxROCmGPUProbeTest, ProcessCountersAreEmptyWhenAvailableOrUnavailable)
{
    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    const auto processCounters = probe.readProcessGPUCounters();
    EXPECT_TRUE(processCounters.empty());
}

TEST(LinuxROCmGPUProbeTest, MockLibraryEnablesAvailableCapabilities)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);

    ASSERT_TRUE(probe.isAvailable());

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed);
    EXPECT_FALSE(caps.hasEngineUtilization);
    EXPECT_FALSE(caps.hasPerProcessMetrics);
    EXPECT_FALSE(caps.hasEncoderDecoder);
    EXPECT_TRUE(caps.supportsMultiGPU);
}

TEST(LinuxROCmGPUProbeTest, MockLibraryEnumeratesDevicesWithFallbackIdentifiers)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);

    ASSERT_TRUE(probe.isAvailable());

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 3U);

    EXPECT_EQ(gpus[0].name, "Mock AMD GPU 0");
    EXPECT_EQ(gpus[0].id, "4001");
    EXPECT_EQ(gpus[0].driverVersion, "ROCm");
    EXPECT_EQ(gpus[0].vendor, "AMD");

    EXPECT_EQ(gpus[1].name, "AMD GPU 1");
    EXPECT_EQ(gpus[1].id, "9001");

    EXPECT_EQ(gpus[2].name, "Mock AMD GPU 2");
    EXPECT_EQ(gpus[2].id, "amd_2");
}

TEST(LinuxROCmGPUProbeTest, MockLibraryReturnsExpectedCountersAndFallbacks)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);

    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 3U);

    // gpuId must match the ID returned by enumerateGPUs() so the domain layer can correlate
    // counters to their GPUInfo entry. Device 0: uniqueId=4001; device 1: pciId=9001;
    // device 2: no unique/PCI id, falls back to "amd_<index>".
    EXPECT_EQ(counters[0].gpuId, "4001");
    EXPECT_DOUBLE_EQ(counters[0].utilizationPercent, 80.0);
    EXPECT_EQ(counters[0].memoryUsedBytes, 3ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].memoryTotalBytes, 12ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].temperatureC, 65);
    EXPECT_DOUBLE_EQ(counters[0].powerDrawWatts, 150.0);
    EXPECT_DOUBLE_EQ(counters[0].powerLimitWatts, 220.0);
    EXPECT_EQ(counters[0].gpuClockMHz, 1500U);
    // ROCmMock reports a raw fan value of 170 and a mocked RSMI_MAX_FAN_SPEED of 255; ROCmGPUProbe
    // stores both unconverted (Domain normalizes them to a percentage -- see #734).
    EXPECT_EQ(counters[0].fanSpeedRaw, 170U);
    EXPECT_EQ(counters[0].fanSpeedMaxRaw, 255U);

    EXPECT_EQ(counters[1].gpuId, "9001");
    EXPECT_EQ(counters[1].gpuClockMHz, 0U);
    EXPECT_EQ(counters[1].fanSpeedRaw, 0U); // hasFanSpeed=false in the mock
    EXPECT_EQ(counters[1].fanSpeedMaxRaw, 0U);

    EXPECT_EQ(counters[2].gpuId, "amd_2");
    // Legitimate 0 reading (fan stopped / 0% duty) must still be stored as a real sample --
    // fanSpeedMaxRaw must be populated too, not left at 0 as if the metric were unavailable
    // (regression coverage for a "fanSpeed > 0" guard that used to misclassify this as missing).
    EXPECT_EQ(counters[2].fanSpeedRaw, 0U);
    EXPECT_EQ(counters[2].fanSpeedMaxRaw, 255U);
    EXPECT_DOUBLE_EQ(counters[2].encoderUtilPercent, 0.0);
    EXPECT_DOUBLE_EQ(counters[2].decoderUtilPercent, 0.0);
}

// ROCm SMI's BDF id packs (domain << 32) | (bus << 8) | (device << 3) | function (#1117).
TEST(ROCmGPUProbeMathTest, SysfsPciAddressUnpacksTheBdfId)
{
    EXPECT_EQ(ROCmGPUProbeMath::sysfsPciAddress(0x0300ULL), "0000:03:00.0");
    EXPECT_EQ(ROCmGPUProbeMath::sysfsPciAddress(9001ULL), "0000:23:05.1"); // 0x2329
    EXPECT_EQ(ROCmGPUProbeMath::sysfsPciAddress((1ULL << 32U) | 0xC100ULL), "0001:c1:00.0");
}

// =============================================================================
// APU classification (#1266), shared with DRMGPUProbe through AmdApu.h (#1344)
// =============================================================================

using AmdApu::GcIpVersion;

TEST(AmdApuTest, AnApuGraphicsCoreVersionIsIntegrated)
{
    // The GC IP versions amdgpu flags AMD_IS_APU: Renoir, Rembrandt, Phoenix, Strix Point, Strix Halo.
    for (const GcIpVersion gc : {GcIpVersion{.major = 9, .minor = 3, .revision = 0},
                                 GcIpVersion{.major = 10, .minor = 3, .revision = 3},
                                 GcIpVersion{.major = 11, .minor = 0, .revision = 1},
                                 GcIpVersion{.major = 11, .minor = 5, .revision = 0},
                                 GcIpVersion{.major = 11, .minor = 5, .revision = 1},
                                 GcIpVersion{.major = 11, .minor = 5, .revision = 4},
                                 GcIpVersion{.major = 11, .minor = 5, .revision = 6},
                                 GcIpVersion{.major = 11, .minor = 7, .revision = 0},
                                 GcIpVersion{.major = 11, .minor = 7, .revision = 1}})
    {
        EXPECT_TRUE(AmdApu::isAmdApu(gc, std::nullopt)) << gc.major << "." << gc.minor << "." << gc.revision;
    }
}

TEST(AmdApuTest, ADiscreteGraphicsCoreVersionIsDiscreteWhateverTheDeviceId)
{
    // Navi 21 (10.3.0), Navi 31 (11.0.0), Navi 48 (12.0.1), MI210 (9.4.2): discrete. The GC version
    // decides when it is known, even against a device id that looks like an APU's.
    for (const GcIpVersion gc : {GcIpVersion{.major = 10, .minor = 3, .revision = 0},
                                 GcIpVersion{.major = 11, .minor = 0, .revision = 0},
                                 GcIpVersion{.major = 12, .minor = 0, .revision = 1},
                                 GcIpVersion{.major = 9, .minor = 4, .revision = 2}})
    {
        EXPECT_FALSE(AmdApu::isAmdApu(gc, std::nullopt));
        EXPECT_FALSE(AmdApu::isAmdApu(gc, std::uint16_t{0x15BF}));
    }
}

TEST(AmdApuTest, WithoutAGraphicsCoreVersionTheDeviceIdDecides)
{
    EXPECT_TRUE(AmdApu::isAmdApu(std::nullopt, std::uint16_t{0x15DD}));  // Raven
    EXPECT_TRUE(AmdApu::isAmdApu(std::nullopt, std::uint16_t{0x1636}));  // Renoir
    EXPECT_TRUE(AmdApu::isAmdApu(std::nullopt, std::uint16_t{0x1304}));  // Kaveri, first of its range
    EXPECT_TRUE(AmdApu::isAmdApu(std::nullopt, std::uint16_t{0x131D}));  // Kaveri, last of its range
    EXPECT_TRUE(AmdApu::isAmdApu(std::nullopt, std::uint16_t{0x1586}));  // Strix Halo
    EXPECT_FALSE(AmdApu::isAmdApu(std::nullopt, std::uint16_t{0x131E})); // just past Kaveri
    // Gaps in Kaveri's ids that the kernel's pciidlist doesn't flag AMD_IS_APU (#1343 review).
    for (const std::uint16_t gap : {std::uint16_t{0x1308}, std::uint16_t{0x1314}, std::uint16_t{0x1319}, std::uint16_t{0x131A}})
    {
        EXPECT_FALSE(AmdApu::isAmdApu(std::nullopt, gap)) << std::hex << gap;
    }
    // IP-discovery APUs' ids, for a kernel without ip_discovery (#1343 review, ids from pci.ids).
    for (const std::uint16_t id : {std::uint16_t{0x1435},
                                   std::uint16_t{0x13C0},
                                   std::uint16_t{0x1900},
                                   std::uint16_t{0x1901},
                                   std::uint16_t{0x1114},
                                   std::uint16_t{0x1902}})
    {
        EXPECT_TRUE(AmdApu::isAmdApu(std::nullopt, id)) << std::hex << id;
    }
    // Every Cyan Skillfish id the kernel flags AMD_IS_APU (#1343 review).
    for (const std::uint16_t id : {std::uint16_t{0x13DB},
                                   std::uint16_t{0x13F9},
                                   std::uint16_t{0x13FA},
                                   std::uint16_t{0x13FB},
                                   std::uint16_t{0x13FC},
                                   std::uint16_t{0x13FE},
                                   std::uint16_t{0x143F}})
    {
        EXPECT_TRUE(AmdApu::isAmdApu(std::nullopt, id)) << std::hex << id;
    }
    EXPECT_FALSE(AmdApu::isAmdApu(std::nullopt, std::uint16_t{0x73BF})); // Navi 21
    EXPECT_FALSE(AmdApu::isAmdApu(std::nullopt, std::uint16_t{0x744C})); // Navi 31
    EXPECT_FALSE(AmdApu::isAmdApu(std::nullopt, std::nullopt)) << "no signal: discrete, as before #1266";
}

/// A fake amdgpu sysfs entry for mock device 1 (PCI id 9001 = 0000:23:05.1): its PCI device id and,
/// if given, the ip_discovery GC entry under `gcDirName` ("GC" or the hardware id "11").
void makeAmdgpuSysfs(const std::filesystem::path& root,
                     const std::string& deviceId,
                     std::optional<GcIpVersion> gc,
                     const std::string& gcDirName = "GC")
{
    const auto dir = root / "0000:23:05.1";
    std::filesystem::create_directories(dir / "power");
    std::ofstream(dir / "power" / "runtime_status") << "active\n";
    std::ofstream(dir / "device") << deviceId << "\n";
    if (gc.has_value())
    {
        const auto gcDir = dir / "ip_discovery" / "die" / "0" / gcDirName / "0";
        std::filesystem::create_directories(gcDir);
        std::ofstream(gcDir / "major") << gc->major << "\n";
        std::ofstream(gcDir / "minor") << gc->minor << "\n";
        std::ofstream(gcDir / "revision") << gc->revision << "\n";
    }
}

/// isIntegrated of each mock device, enumerated with `pciRoot` as the sysfs PCI root.
std::vector<bool> integratedFlags(const std::filesystem::path& pciRoot)
{
    ROCmGPUProbe probe(pciRoot.string());
    std::vector<bool> flags;
    for (const auto& gpu : probe.enumerateGPUs())
    {
        flags.push_back(gpu.isIntegrated);
    }
    return flags;
}

TEST(LinuxROCmGPUProbeTest, AnApuIsReportedAsIntegrated)
{
    // #1266: every ROCm GPU was hard-coded discrete, so an APU's shared memory counted as VRAM.
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }

    {
        // Phoenix (GC 11.0.1) by its ip_discovery entry.
        const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_apu_gc");
        makeAmdgpuSysfs(pciRoot.path, "0x15bf", GcIpVersion{.major = 11, .minor = 0, .revision = 1});
        EXPECT_EQ(integratedFlags(pciRoot.path), (std::vector<bool>{false, true, false}))
            << "only device 1 has a PCI address; the others have no signal and stay discrete";
    }
    {
        // The same entry listed under the GC hardware id (11) rather than its name.
        const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_apu_hwid");
        makeAmdgpuSysfs(pciRoot.path, "0x15bf", GcIpVersion{.major = 11, .minor = 0, .revision = 1}, "11");
        EXPECT_EQ(integratedFlags(pciRoot.path), (std::vector<bool>{false, true, false}));
    }
    {
        // A kernel without ip_discovery: Renoir's PCI device id decides.
        const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_apu_id");
        makeAmdgpuSysfs(pciRoot.path, "0x1636", std::nullopt);
        EXPECT_EQ(integratedFlags(pciRoot.path), (std::vector<bool>{false, true, false}));
    }
}

TEST(LinuxROCmGPUProbeTest, ADiscreteGpuIsNotReportedAsIntegrated)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }

    {
        // Navi 21 (GC 10.3.0).
        const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_dgpu_gc");
        makeAmdgpuSysfs(pciRoot.path, "0x73bf", GcIpVersion{.major = 10, .minor = 3, .revision = 0});
        EXPECT_EQ(integratedFlags(pciRoot.path), (std::vector<bool>(3, false)));
    }
    {
        // Navi 21 by device id alone.
        const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_dgpu_id");
        makeAmdgpuSysfs(pciRoot.path, "0x73bf", std::nullopt);
        EXPECT_EQ(integratedFlags(pciRoot.path), (std::vector<bool>(3, false)));
    }
    // No sysfs at all: discrete, as before.
    EXPECT_EQ(integratedFlags(TestSupport::ISOLATED_PCI_ROOT), (std::vector<bool>(3, false)));
}

// #1272 review: a transient failure (RSMI_STATUS_BUSY) while sensors are probed at enumeration doesn't
// hide them for the session; only not-supported / not-found / not-implemented does.
TEST(LinuxROCmGPUProbeTest, ATransientFailureAtEnumerationKeepsTheSensors)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    void* library = dlopen("librocm_smi64.so.6", RTLD_NOW);
    ASSERT_NE(library, nullptr);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- dlsym returns void* by POSIX definition
    const auto failSensorReads = reinterpret_cast<void (*)(int)>(dlsym(library, "tasksmackRocmMockFailSensorReads"));
    ASSERT_NE(failSensorReads, nullptr);

    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());
    failSensorReads(1);
    const auto gpus = probe.enumerateGPUs();
    failSensorReads(0);
    const auto counters = probe.readGPUCounters();
    dlclose(library);

    ASSERT_EQ(gpus.size(), 3U);
    const auto full = gpus[0].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(full.hasTemperature);
    EXPECT_TRUE(full.hasPowerMetrics);
    EXPECT_TRUE(full.hasClockSpeeds);
    const auto partial = gpus[1].sensorCapabilities.value_or(GPUCapabilities{});
    // The fan read isn't failed by the mock, so its definitive NOT_FOUND still means unsupported.
    EXPECT_FALSE(partial.hasFanSpeed);

    ASSERT_FALSE(counters.empty());
    EXPECT_TRUE(counters[0].temperatureAvailable);
}

// #1272 review: a clock sample that can't be decoded at enumeration keeps the clock capability (the
// chart and bar stay), and each undecodable reading is reported unavailable -- a gap -- not hidden.
TEST(LinuxROCmGPUProbeTest, AnUndecodableClockAtEnumerationKeepsTheClock)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_GE(gpus.size(), 2U);
    EXPECT_TRUE(gpus[1].sensorCapabilities.value_or(GPUCapabilities{}).hasClockSpeeds); // mock device 1: index out of range

    const auto counters = probe.readGPUCounters();
    ASSERT_GE(counters.size(), 2U);
    EXPECT_FALSE(counters[1].gpuClockAvailable);
    EXPECT_TRUE(counters[0].gpuClockAvailable);
}

// #1112: each device's sensors, from which reads succeed at enumeration. Mock device 1 has no
// junction sensor, no fan and an undecodable GPU clock sample; device 0 has all of them.
TEST(LinuxROCmGPUProbeTest, SensorCapabilitiesArePerDevice)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 3U);

    ASSERT_TRUE(gpus[0].sensorCapabilities.has_value());
    const auto full = gpus[0].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(full.hasTemperature);
    EXPECT_TRUE(full.hasPowerMetrics);
    EXPECT_TRUE(full.hasClockSpeeds);
    EXPECT_TRUE(full.hasFanSpeed);

    ASSERT_TRUE(gpus[1].sensorCapabilities.has_value());
    const auto partial = gpus[1].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(partial.hasTemperature);
    EXPECT_TRUE(partial.hasPowerMetrics);
    // Its clock query succeeds but the sample can't be decoded (current index out of range). That
    // isn't "unsupported": the clock is kept and its readings are unavailable until one decodes.
    EXPECT_TRUE(partial.hasClockSpeeds);
    EXPECT_FALSE(partial.hasFanSpeed);
}

// #1117: a runtime-suspended AMD GPU (mock device 1, PCI id 9001 = 0000:23:05.1) is left alone:
// its readings are unavailable and it is marked asleep, while the others are read as usual.
TEST(LinuxROCmGPUProbeTest, RuntimeSuspendedGpuIsNotRead)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_runtime_pm");
    const auto statusPath = pciRoot.path / "0000:23:05.1" / "power" / "runtime_status";
    std::filesystem::create_directories(statusPath.parent_path());
    std::ofstream(statusPath) << "active\n";

    ROCmGPUProbe probe(pciRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.readGPUCounters().size(), 3U); // awake: records the VRAM total

    std::ofstream(statusPath) << "suspended\n";
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 3U);
    EXPECT_EQ(counters[1].gpuId, "9001");
    EXPECT_TRUE(counters[1].suspended);
    EXPECT_FALSE(counters[1].utilizationAvailable);
    EXPECT_FALSE(counters[1].temperatureAvailable);
    EXPECT_FALSE(counters[1].powerAvailable);
    EXPECT_FALSE(counters[1].memoryAvailable);
    EXPECT_EQ(counters[1].temperatureC, 0);
    EXPECT_EQ(counters[1].memoryTotalBytes, 8ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_FALSE(counters[0].suspended);
    EXPECT_EQ(counters[0].temperatureC, 65);

    EXPECT_FALSE(probe.enumerateGPUs()[1].sensorCapabilities.has_value()); // not woken to probe
}

// =============================================================================
// Re-enumeration (#1116, #1289)
// =============================================================================

/// Drives the ROCm mock's #1116 controls for one test and resets them afterwards. dlopen() returns
/// the same instance the probe loads, and holding it keeps the mock's state across a probe re-init.
class RocmMockControls
{
  public:
    RocmMockControls() : m_Library(dlopen("librocm_smi64.so.6", RTLD_NOW))
    {
        if (m_Library != nullptr)
        {
            // dlsym returns void* by POSIX definition; the casts restore the mock's signatures.
            // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
            m_SetDeviceCount = reinterpret_cast<SetCountFn>(dlsym(m_Library, "tasksmackRocmMockSetDeviceCount"));
            m_InitErrorReads = reinterpret_cast<SetFlagFn>(dlsym(m_Library, "tasksmackRocmMockInitErrorReads"));
            m_InitCalls = reinterpret_cast<CountFn>(dlsym(m_Library, "tasksmackRocmMockInitCalls"));
            m_FailInits = reinterpret_cast<SetCountFn>(dlsym(m_Library, "tasksmackRocmMockFailInits"));
            // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        }
    }

    ~RocmMockControls()
    {
        if (available())
        {
            m_SetDeviceCount(MOCK_DEVICE_COUNT);
            m_InitErrorReads(0);
            m_FailInits(0);
        }
        if (m_Library != nullptr)
        {
            dlclose(m_Library);
        }
    }

    RocmMockControls(const RocmMockControls&) = delete;
    RocmMockControls& operator=(const RocmMockControls&) = delete;
    RocmMockControls(RocmMockControls&&) = delete;
    RocmMockControls& operator=(RocmMockControls&&) = delete;

    [[nodiscard]] bool available() const
    {
        return m_SetDeviceCount != nullptr && m_InitErrorReads != nullptr && m_InitCalls != nullptr && m_FailInits != nullptr;
    }

    void setDeviceCount(unsigned int count) const
    {
        m_SetDeviceCount(count);
    }

    void initErrorReads(bool fail) const
    {
        m_InitErrorReads(fail ? 1 : 0);
    }

    [[nodiscard]] unsigned int initCalls() const
    {
        return m_InitCalls();
    }

    /// The next `count` rsmi_init calls fail with RSMI_STATUS_INIT_ERROR.
    void failInits(unsigned int count) const
    {
        m_FailInits(count);
    }

    static constexpr unsigned int MOCK_DEVICE_COUNT = 3;

  private:
    using SetCountFn = void (*)(unsigned int);
    using SetFlagFn = void (*)(int);
    using CountFn = unsigned int (*)();

    void* m_Library;
    SetCountFn m_SetDeviceCount = nullptr;
    SetFlagFn m_InitErrorReads = nullptr;
    CountFn m_InitCalls = nullptr;
    SetCountFn m_FailInits = nullptr;
};

/// A fake /sys/bus/pci/devices entry for an AMD GPU bound to `driver`, with power/runtime_status.
void makeAmdPciDevice(const std::filesystem::path& root,
                      const std::string& address,
                      const std::string& runtimeStatus = "active",
                      const std::string& driver = "amdgpu")
{
    const auto dir = root / address;
    std::filesystem::create_directories(dir / "power");
    std::ofstream(dir / "vendor") << "0x1002\n";
    std::ofstream(dir / "class") << "0x030000\n";
    std::ofstream(dir / "power" / "runtime_status") << runtimeStatus << "\n";
    std::filesystem::create_symlink("/nonexistent/drivers/" + driver, dir / "driver");
}

// #1116: an AMD GPU hot-plugged after startup gets ROCm SMI re-initialised at the next full rescan;
// the GPUs already there keep their ids.
TEST(LinuxROCmGPUProbeTest, AHotPluggedGpuIsFoundOnTheNextFullRescan)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const RocmMockControls controls;
    ASSERT_TRUE(controls.available());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_hotplug");
    makeAmdPciDevice(pciRoot.path, "0000:23:05.1");
    controls.setDeviceCount(2);

    ROCmGPUProbe probe(pciRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto before = probe.enumerateGPUs();
    ASSERT_EQ(before.size(), 2U);
    const unsigned int initsBefore = controls.initCalls();
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));

    makeAmdPciDevice(pciRoot.path, "0000:c1:00.0");
    controls.setDeviceCount(3);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // a quick rescan doesn't scan sysfs
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.initCalls(), initsBefore + 1);

    const auto after = probe.enumerateGPUs();
    ASSERT_EQ(after.size(), 3U);
    EXPECT_EQ(after[0].id, before[0].id);
    EXPECT_EQ(after[1].id, before[1].id);
    EXPECT_EQ(after[2].name, "Mock AMD GPU 2");
    EXPECT_EQ(probe.readGPUCounters().size(), 3U);
}

// #1295 review: a restart that fails while an amdgpu GPU is still bound reports no change, so
// GPUModel keeps the known GPU list rather than publishing an empty one; the next full rescan retries,
// and the restart that works reports the change.
TEST(LinuxROCmGPUProbeTest, AFailedRestartWithTheGpusStillPresentReportsNoChange)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const RocmMockControls controls;
    ASSERT_TRUE(controls.available());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_failed_restart");
    makeAmdPciDevice(pciRoot.path, "0000:23:05.1");
    controls.setDeviceCount(2);

    ROCmGPUProbe probe(pciRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);

    makeAmdPciDevice(pciRoot.path, "0000:c1:00.0"); // a hot-plug triggers a restart...
    controls.setDeviceCount(3);
    controls.failInits(1);                           // ...which fails once
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)); // GPUs still bound: keep the known list
    EXPECT_FALSE(probe.isAvailable());

    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full)); // retried and started: publish
    EXPECT_TRUE(probe.isAvailable());
    EXPECT_EQ(probe.enumerateGPUs().size(), 3U);
}

// #1116: a read failing with RSMI_STATUS_INIT_ERROR gets ROCm SMI re-initialised at the next full
// rescan, not on every sample.
TEST(LinuxROCmGPUProbeTest, AnInitErrorReinitialisesOnTheNextFullRescan)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const RocmMockControls controls;
    ASSERT_TRUE(controls.available());

    ROCmGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 3U); // finds every sensor set, so quick rescans have nothing to ask for
    const unsigned int initsBefore = controls.initCalls();
    controls.initErrorReads(true);
    const auto failed = probe.readGPUCounters();
    controls.initErrorReads(false);
    ASSERT_FALSE(failed.empty());
    EXPECT_FALSE(failed[0].utilizationAvailable);

    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_EQ(controls.initCalls(), initsBefore);
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.initCalls(), initsBefore + 1);
    ASSERT_TRUE(probe.isAvailable());
    EXPECT_EQ(probe.enumerateGPUs().size(), 3U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
}

// #1289: an AMD GPU asleep at enumeration (mock device 1, at 0000:23:05.1) gets its own sensor set on
// its first awake sample: a quick rescan asks for a re-enumeration, which finds it has no junction
// sensor or fan.
TEST(LinuxROCmGPUProbeTest, AGpuAsleepAtEnumerationGetsItsOwnSensorsOnceAwake)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_wake");
    makeAmdPciDevice(pciRoot.path, "0000:23:05.1", "suspended");

    ROCmGPUProbe probe(pciRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto asleep = probe.enumerateGPUs();
    ASSERT_EQ(asleep.size(), 3U);
    EXPECT_FALSE(asleep[1].sensorCapabilities.has_value());
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));

    std::ofstream(pciRoot.path / "0000:23:05.1" / "power" / "runtime_status") << "active\n";
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    const auto awake = probe.enumerateGPUs();
    ASSERT_EQ(awake.size(), 3U);
    ASSERT_TRUE(awake[1].sensorCapabilities.has_value());
    const auto sensors = awake[1].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(sensors.hasTemperature);
    EXPECT_FALSE(sensors.hasFanSpeed);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // found: no more re-enumerations
}

// #1295 review: a GPU asleep through a ROCm SMI restart -- here one triggered by another AMD GPU
// being hot-plugged -- keeps the sensor set found while it was awake (mock device 1, at
// 0000:23:05.1: no junction sensor or fan), and its VRAM total. It isn't woken to find them again,
// so forgetting them would republish the probe-wide capabilities for it until it woke.
TEST(LinuxROCmGPUProbeTest, AGpuAsleepThroughARestartKeepsItsSensors)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const RocmMockControls controls;
    ASSERT_TRUE(controls.available());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_restart_asleep");
    makeAmdPciDevice(pciRoot.path, "0000:23:05.1");
    controls.setDeviceCount(2);

    ROCmGPUProbe probe(pciRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);   // both sensor sets found while awake
    ASSERT_EQ(probe.readGPUCounters().size(), 2U); // and both VRAM totals

    std::ofstream(pciRoot.path / "0000:23:05.1" / "power" / "runtime_status") << "suspended\n";
    const unsigned int initsBefore = controls.initCalls();
    makeAmdPciDevice(pciRoot.path, "0000:c1:00.0");
    controls.setDeviceCount(3);
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.initCalls(), initsBefore + 1);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 3U);
    EXPECT_EQ(gpus[1].id, "9001");
    ASSERT_TRUE(gpus[1].sensorCapabilities.has_value());
    const auto sensors = gpus[1].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(sensors.hasTemperature);
    EXPECT_FALSE(sensors.hasFanSpeed);
    EXPECT_TRUE(gpus[2].sensorCapabilities.has_value()); // the new GPU is awake: found now
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 3U);
    EXPECT_TRUE(counters[1].suspended);
    EXPECT_EQ(counters[1].memoryTotalBytes, 8ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // nothing left to find
}

// #1295 review: ROCm SMI failing to start while an amdgpu-bound GPU is present (TaskSmack started
// during a driver reload) is retried at each full rescan -- not every sample -- until it starts; the
// rescan that brings it up reports a change, so the GPUs are enumerated.
TEST(LinuxROCmGPUProbeTest, RocmNotStartingWithAnAmdGpuPresentIsRetriedAtFullRescans)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const RocmMockControls controls;
    ASSERT_TRUE(controls.available());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_start_retry");
    makeAmdPciDevice(pciRoot.path, "0000:23:05.1");
    controls.failInits(2);
    const unsigned int initsBefore = controls.initCalls();

    ROCmGPUProbe probe(pciRoot.path.string());
    EXPECT_FALSE(probe.isAvailable());
    EXPECT_TRUE(probe.enumerateGPUs().empty());
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // waits for the full rescan
    EXPECT_EQ(controls.initCalls(), initsBefore + 1);

    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)); // still not ready: no change reported, nothing to publish
    EXPECT_EQ(controls.initCalls(), initsBefore + 2);
    EXPECT_FALSE(probe.isAvailable());

    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.initCalls(), initsBefore + 3);
    ASSERT_TRUE(probe.isAvailable());
    EXPECT_EQ(probe.enumerateGPUs().size(), 3U);

    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)); // started: no further retries
    EXPECT_EQ(controls.initCalls(), initsBefore + 3);
}

// #1295 review: with no GPU bound to amdgpu (none at all, or one on radeon) ROCm SMI failing to start
// is not retried: it could never succeed, and a driver binding later changes the PCI list, which
// restarts ROCm SMI anyway.
TEST(LinuxROCmGPUProbeTest, RocmNotStartingWithoutAnAmdgpuBoundGpuIsNotRetried)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const RocmMockControls controls;
    ASSERT_TRUE(controls.available());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_rocm_start_no_retry");
    makeAmdPciDevice(pciRoot.path, "0000:23:05.1", "active", "radeon");

    for (const auto& root : {pciRoot.path.string(), std::string(TestSupport::ISOLATED_PCI_ROOT)})
    {
        controls.failInits(1);
        const unsigned int initsBefore = controls.initCalls();
        ROCmGPUProbe probe(root);
        EXPECT_FALSE(probe.isAvailable()) << root;
        EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << root;
        EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << root;
        EXPECT_EQ(controls.initCalls(), initsBefore + 1) << root;
        EXPECT_FALSE(probe.isAvailable()) << root;
    }
}

} // namespace
} // namespace Platform

#endif
