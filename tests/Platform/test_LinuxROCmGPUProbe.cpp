#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/ROCmGPUProbe.h"
#include "Platform/Linux/ROCmGPUProbeMath.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

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
    ROCmGPUProbe probe;
    ASSERT_TRUE(probe.isAvailable());
    const auto expectedIds = probe.enumerateGPUs();
    const unsigned int callsAtLoad = idCalls();
    configure(0); // every id lookup from now on fails

    const auto counters = probe.readGPUCounters();
    const auto gpus = probe.enumerateGPUs();
    configure(-1);
    dlclose(library);

    ASSERT_EQ(counters.size(), expectedIds.size());
    ASSERT_EQ(gpus.size(), expectedIds.size());
    for (std::size_t i = 0; i < expectedIds.size(); ++i)
    {
        EXPECT_EQ(counters[i].gpuId, expectedIds[i].id);
        EXPECT_EQ(gpus[i].id, expectedIds[i].id);
    }
    EXPECT_GT(callsAtLoad, 0U);
}

TEST(LinuxROCmGPUProbeTest, BasicOperationsDoNotThrow)
{
    ROCmGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxROCmGPUProbeTest, UnavailableProbeReportsNoCapabilities)
{
    ROCmGPUProbe probe;
    if (probe.isAvailable())
    {
        // Under CTest (ENVIRONMENT_MODIFICATION), LD_LIBRARY_PATH is prepended with the
        // mock library so the probe is always available. The unavailable path cannot be
        // exercised in this process.
        GTEST_SKIP() << "ROCm probe is available (mock library loaded); unavailable path not testable in this environment";
    }

    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasHotspotTemp);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasClockSpeeds);
    EXPECT_FALSE(caps.hasFanSpeed);
    EXPECT_FALSE(caps.hasPerProcessMetrics);
}

TEST(LinuxROCmGPUProbeTest, ProcessCountersAreEmptyWhenAvailableOrUnavailable)
{
    ROCmGPUProbe probe;
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
    ROCmGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasHotspotTemp);
    EXPECT_TRUE(caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed);
    EXPECT_FALSE(caps.hasPCIeMetrics);
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
    ROCmGPUProbe probe;

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
    ROCmGPUProbe probe;

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
    EXPECT_EQ(counters[0].hotspotTempC, 72);
    EXPECT_DOUBLE_EQ(counters[0].powerDrawWatts, 150.0);
    EXPECT_DOUBLE_EQ(counters[0].powerLimitWatts, 220.0);
    EXPECT_EQ(counters[0].gpuClockMHz, 1500U);
    EXPECT_EQ(counters[0].memoryClockMHz, 2000U);
    // ROCmMock reports a raw fan value of 170 and a mocked RSMI_MAX_FAN_SPEED of 255; ROCmGPUProbe
    // stores both unconverted (Domain normalizes them to a percentage -- see #734).
    EXPECT_EQ(counters[0].fanSpeedRaw, 170U);
    EXPECT_EQ(counters[0].fanSpeedMaxRaw, 255U);

    EXPECT_EQ(counters[1].gpuId, "9001");
    EXPECT_EQ(counters[1].hotspotTempC, -1);
    EXPECT_EQ(counters[1].gpuClockMHz, 0U);
    EXPECT_EQ(counters[1].memoryClockMHz, 0U);
    EXPECT_EQ(counters[1].fanSpeedRaw, 0U); // hasFanSpeed=false in the mock
    EXPECT_EQ(counters[1].fanSpeedMaxRaw, 0U);

    EXPECT_EQ(counters[2].gpuId, "amd_2");
    // Legitimate 0 reading (fan stopped / 0% duty) must still be stored as a real sample --
    // fanSpeedMaxRaw must be populated too, not left at 0 as if the metric were unavailable
    // (regression coverage for a "fanSpeed > 0" guard that used to misclassify this as missing).
    EXPECT_EQ(counters[2].fanSpeedRaw, 0U);
    EXPECT_EQ(counters[2].fanSpeedMaxRaw, 255U);
    EXPECT_EQ(counters[2].pcieTxBytes, 0U);
    EXPECT_EQ(counters[2].pcieRxBytes, 0U);
    EXPECT_DOUBLE_EQ(counters[2].computeUtilPercent, 0.0);
    EXPECT_DOUBLE_EQ(counters[2].encoderUtilPercent, 0.0);
    EXPECT_DOUBLE_EQ(counters[2].decoderUtilPercent, 0.0);
}

} // namespace
} // namespace Platform

#endif
