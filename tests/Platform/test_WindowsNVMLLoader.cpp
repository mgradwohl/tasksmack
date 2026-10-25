/// @file test_WindowsNVMLLoader.cpp
/// @brief Loading nvml.dll through NVMLGPUProbe's injectable loader (#1720): the DLL missing, each
/// required export missing, the optional ones missing, the PCI-info _v3 -> _v2 fallback, nvmlInit
/// failing, and the probe's shutdown. The fake loader resolves each export to WindowsNVMLFake.h's
/// fakes, so no real nvml.dll is ever loaded.

#ifdef _WIN32

#include "Mocks/WindowsNVMLFake.h"
#include "Platform/GPUTypes.h"
#include "Platform/NVMLTypes.h"
#include "Platform/Windows/NVMLGPUProbe.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

using namespace Platform::NVML;     // NOLINT(google-build-using-namespace) - test fakes mirror the C API
using namespace Platform::NVMLFake; // NOLINT(google-build-using-namespace)

class NVMLLoaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fakeState() = FakeNvmlState{};
        fakeLibrary() = FakeLibraryState{};
    }

    /// A probe built on the fake loader, with every fake GPU awake: the real PnP query would look at
    /// whatever adapter this machine has at the fake's PCI location (#1265).
    static std::unique_ptr<NVMLGPUProbe> makeProbe()
    {
        auto probe = std::make_unique<NVMLGPUProbe>(fakeLibraryFunctions());
        NVMLGPUProbeTestAccessor::setAsleep(*probe, [](const PciLocation&) { return false; });
        return probe;
    }

    /// One fake GPU, with WindowsNVMLFake.h's default readings.
    static void oneDevice()
    {
        fakeState().deviceCount = 1;
        static_cast<void>(deviceData(0));
    }

    /// How many times @p name was looked up.
    static std::size_t lookups(const std::string& name)
    {
        return static_cast<std::size_t>(std::ranges::count(fakeLibrary().lookups, name));
    }
};

// ==========================================================================
// nvml.dll not installed
// ==========================================================================

TEST_F(NVMLLoaderTest, WithoutTheDllTheProbeIsUnavailableAndAsksNothing)
{
    fakeLibrary().present = false;

    {
        const auto probe = makeProbe();
        EXPECT_FALSE(probe->isLoaded());
        EXPECT_FALSE(probe->isAvailable());
        EXPECT_TRUE(probe->enumerateGPUs().empty());
        EXPECT_TRUE(probe->readGPUCounters().empty());
        const auto caps = probe->capabilities();
        EXPECT_FALSE(caps.hasTemperature);
        EXPECT_FALSE(caps.hasPowerMetrics);
        EXPECT_FALSE(caps.hasEncoderDecoder);
        EXPECT_FALSE(caps.supportsMultiGPU);
    }

    EXPECT_EQ(fakeLibrary().loadCount, 1);
    EXPECT_TRUE(fakeLibrary().lookups.empty()) << "No export is looked up without a module";
    EXPECT_EQ(fakeLibrary().freeCount, 0) << "Nothing was loaded, so nothing is freed";
    EXPECT_EQ(fakeState().initCallCount, 0);
    EXPECT_EQ(fakeState().shutdownCallCount, 0);
}

// The DLL can be installed after TaskSmack starts (a driver install): restart() loads it then (#1294).
TEST_F(NVMLLoaderTest, RestartLoadsADllThatWasMissingAtConstruction)
{
    fakeLibrary().present = false;
    const auto probe = makeProbe();
    ASSERT_FALSE(probe->isLoaded());

    // Still missing: a failed restart, logged once at info and then quietly.
    EXPECT_FALSE(probe->restart());
    EXPECT_FALSE(probe->restart());
    EXPECT_FALSE(probe->isAvailable());
    EXPECT_EQ(fakeLibrary().loadCount, 3);

    fakeLibrary().present = true;
    oneDevice();
    EXPECT_TRUE(probe->restart());
    EXPECT_TRUE(probe->isLoaded());
    EXPECT_TRUE(probe->isAvailable());
    EXPECT_EQ(fakeLibrary().loadCount, 4);
    EXPECT_EQ(fakeState().initCallCount, 1);
    EXPECT_EQ(probe->enumerateGPUs().size(), 1U);
}

// ==========================================================================
// A full load
// ==========================================================================

TEST_F(NVMLLoaderTest, AFullLoadReadsEveryMetricOfTheDevice)
{
    oneDevice();
    auto& device = deviceData(0);
    device.uuid = "GPU-loader";
    device.pciBus = 0x41;

    const auto probe = makeProbe();
    ASSERT_TRUE(probe->isLoaded());
    ASSERT_TRUE(probe->isAvailable());
    EXPECT_EQ(fakeLibrary().loadCount, 1);
    EXPECT_EQ(fakeState().initCallCount, 1);
    EXPECT_EQ(lookups("nvmlDeviceGetPciInfo_v3"), 1U);
    EXPECT_EQ(lookups("nvmlDeviceGetPciInfo_v2"), 0U) << "_v2 is only the fallback";

    const auto caps = probe->capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed);
    EXPECT_TRUE(caps.hasEncoderDecoder);
    EXPECT_TRUE(caps.supportsMultiGPU);

    const auto gpus = probe->enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].id, "GPU-loader");
    EXPECT_EQ(gpus[0].name, device.name);
    EXPECT_EQ(gpus[0].driverVersion, device.vbios);
    ASSERT_TRUE(gpus[0].pciLocation.has_value());
    EXPECT_EQ(gpus[0].pciLocation.value_or(PciLocation{}).bus, 0x41U);
    ASSERT_TRUE(gpus[0].sensorCapabilities.has_value());

    const auto counters = probe->readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    const auto& counter = counters[0];
    EXPECT_EQ(counter.gpuId, "GPU-loader");
    EXPECT_DOUBLE_EQ(counter.utilizationPercent, 37.0);
    EXPECT_EQ(counter.memoryUsedBytes, device.memUsed);
    EXPECT_EQ(counter.memoryTotalBytes, device.memTotal);
    EXPECT_EQ(counter.temperatureC, 63);
    EXPECT_DOUBLE_EQ(counter.powerDrawWatts, 180.0);
    EXPECT_DOUBLE_EQ(counter.powerLimitWatts, 450.0);
    EXPECT_EQ(counter.gpuClockMHz, 2100U);
    EXPECT_EQ(counter.fanSpeedRaw, 48U);
    EXPECT_DOUBLE_EQ(counter.encoderUtilPercent, 30.0);
    EXPECT_DOUBLE_EQ(counter.decoderUtilPercent, 12.0);
    EXPECT_TRUE(counter.utilizationAvailable);
    EXPECT_TRUE(counter.memoryAvailable);
    EXPECT_TRUE(counter.temperatureAvailable);
    EXPECT_TRUE(counter.powerAvailable);
    EXPECT_TRUE(counter.gpuClockAvailable);
    EXPECT_TRUE(counter.encoderAvailable);
    EXPECT_TRUE(counter.decoderAvailable);
}

// Each metric's own failure, read through a loaded nvml.dll: a sampled reading becomes unavailable
// (not a real 0, #1111), and the power limit and fan, which have no flag, stay unset.
TEST_F(NVMLLoaderTest, EachFailedMetricIsUnavailableOnItsOwn)
{
    oneDevice();
    auto& device = deviceData(0);
    device.memoryOk = false;
    device.temperatureOk = false;
    device.powerOk = false;
    device.powerLimitOk = false;
    device.gpuClockOk = false;
    device.utilizationOk = false;
    device.fanOk = false;
    device.encoderResult = NVML_ERROR_TIMEOUT;

    const auto probe = makeProbe();
    const auto gpus = probe->enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    const auto sensors = gpus[0].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_FALSE(sensors.hasTemperature);
    EXPECT_FALSE(sensors.hasPowerMetrics);
    EXPECT_FALSE(sensors.hasClockSpeeds);
    EXPECT_FALSE(sensors.hasFanSpeed);
    EXPECT_TRUE(sensors.hasEncoderDecoder) << "A timeout isn't 'no encoder', and the decoder answered";

    const auto counters = probe->readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    const auto& counter = counters[0];
    EXPECT_FALSE(counter.memoryAvailable);
    EXPECT_EQ(counter.memoryTotalBytes, 0U);
    EXPECT_FALSE(counter.temperatureAvailable);
    EXPECT_FALSE(counter.powerAvailable);
    EXPECT_DOUBLE_EQ(counter.powerLimitWatts, 0.0);
    EXPECT_FALSE(counter.gpuClockAvailable);
    EXPECT_FALSE(counter.utilizationAvailable);
    EXPECT_EQ(counter.fanSpeedRaw, 0U);
    EXPECT_EQ(counter.fanSpeedMaxRaw, 0U);
    EXPECT_FALSE(counter.encoderAvailable);
    EXPECT_TRUE(counter.decoderAvailable);
}

TEST_F(NVMLLoaderTest, NoDevicesIsAvailableWithNothingToList)
{
    fakeState().deviceCount = 0;

    const auto probe = makeProbe();
    EXPECT_TRUE(probe->isAvailable());
    EXPECT_TRUE(probe->enumerateGPUs().empty());
    EXPECT_TRUE(probe->readGPUCounters().empty());
}

TEST_F(NVMLLoaderTest, AFailedDeviceCountListsNothing)
{
    fakeState().deviceCountResult = NVML_ERROR_UNKNOWN;
    oneDevice();

    const auto probe = makeProbe();
    EXPECT_TRUE(probe->isAvailable());
    EXPECT_TRUE(probe->enumerateGPUs().empty());
}

// ==========================================================================
// Missing exports
// ==========================================================================

/// Every export loadNVML() requires, in the order it looks them up.
const std::vector<std::string>& requiredExports()
{
    static const std::vector<std::string> names = {
        "nvmlInit",
        "nvmlShutdown",
        "nvmlDeviceGetCount",
        "nvmlDeviceGetHandleByIndex",
        "nvmlDeviceGetName",
        "nvmlDeviceGetUUID",
        "nvmlDeviceGetMemoryInfo",
        "nvmlDeviceGetTemperature",
        "nvmlDeviceGetPowerUsage",
        "nvmlDeviceGetPowerManagementLimit",
        "nvmlDeviceGetClockInfo",
        "nvmlDeviceGetMaxClockInfo",
        "nvmlDeviceGetUtilizationRates",
        "nvmlSystemGetDriverVersion",
        "nvmlDeviceGetVbiosVersion",
        "nvmlDeviceGetFanSpeed",
    };
    return names;
}

class NVMLLoaderMissingExportTest : public NVMLLoaderTest, public ::testing::WithParamInterface<std::string>
{};

// A driver missing a required export can't be used: the module is freed (not leaked, #781), every
// pointer resolved before it is cleared (isLoaded() false), and nvmlInit is never called.
TEST_P(NVMLLoaderMissingExportTest, AMissingRequiredExportFreesTheDllAndLeavesNVMLUnloaded)
{
    const std::string& missing = GetParam();
    fakeLibrary().missingExports.insert(missing);

    {
        const auto probe = makeProbe();
        EXPECT_FALSE(probe->isLoaded());
        EXPECT_FALSE(probe->isAvailable());
        EXPECT_FALSE(probe->capabilities().hasTemperature);
        EXPECT_EQ(fakeLibrary().freeCount, 1);
        EXPECT_EQ(fakeLibrary().lastFreed, fakeModule());
    }

    // The lookups stop at the missing export.
    const auto& exports = requiredExports();
    const auto position = std::ranges::find(exports, missing);
    ASSERT_NE(position, exports.end());
    const std::vector<std::string> expected(exports.begin(), std::next(position));
    EXPECT_EQ(fakeLibrary().lookups, expected);
    EXPECT_EQ(fakeState().initCallCount, 0);
    EXPECT_EQ(fakeState().shutdownCallCount, 0);
    EXPECT_EQ(fakeLibrary().freeCount, 1) << "The destructor doesn't free the module a second time";
}

INSTANTIATE_TEST_SUITE_P(EachRequiredExport,
                         NVMLLoaderMissingExportTest,
                         ::testing::ValuesIn(requiredExports()),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

// Older drivers lack the optional exports: NVML still loads, without the video engines.
TEST_F(NVMLLoaderTest, MissingOptionalExportsStillLoad)
{
    oneDevice();
    fakeLibrary().missingExports = {"nvmlDeviceGetPcieThroughput", "nvmlDeviceGetEncoderUtilization", "nvmlDeviceGetDecoderUtilization"};

    const auto probe = makeProbe();
    ASSERT_TRUE(probe->isLoaded());
    ASSERT_TRUE(probe->isAvailable());
    EXPECT_EQ(fakeLibrary().freeCount, 0);
    EXPECT_FALSE(probe->capabilities().hasEncoderDecoder);
    EXPECT_TRUE(probe->capabilities().hasTemperature);

    const auto gpus = probe->enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasEncoderDecoder);

    const auto counters = probe->readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_FALSE(counters[0].encoderAvailable);
    EXPECT_FALSE(counters[0].decoderAvailable);
    EXPECT_TRUE(counters[0].temperatureAvailable);
}

// Either video-engine export alone keeps the capability (#1485).
TEST_F(NVMLLoaderTest, OneVideoEngineExportKeepsTheCapability)
{
    fakeLibrary().missingExports = {"nvmlDeviceGetEncoderUtilization"};

    const auto probe = makeProbe();
    ASSERT_TRUE(probe->isAvailable());
    EXPECT_TRUE(probe->capabilities().hasEncoderDecoder);
}

// nvml.h maps nvmlDeviceGetPciInfo to the _v3 export; older drivers have only _v2.
TEST_F(NVMLLoaderTest, PciInfoFallsBackToTheV2Export)
{
    oneDevice();
    deviceData(0).pciBus = 0x41;
    fakeLibrary().missingExports = {"nvmlDeviceGetPciInfo_v3"};

    const auto probe = makeProbe();
    ASSERT_TRUE(probe->isAvailable());
    EXPECT_EQ(lookups("nvmlDeviceGetPciInfo_v3"), 1U);
    EXPECT_EQ(lookups("nvmlDeviceGetPciInfo_v2"), 1U);

    const auto gpus = probe->enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    ASSERT_TRUE(gpus[0].pciLocation.has_value());
    EXPECT_EQ(gpus[0].pciLocation.value_or(PciLocation{}).bus, 0x41U);
}

// Neither PCI-info export: NVML loads, and its devices have no PCI identity (#1091).
TEST_F(NVMLLoaderTest, WithoutAnyPciInfoExportDevicesHaveNoPciIdentity)
{
    oneDevice();
    fakeLibrary().missingExports = {"nvmlDeviceGetPciInfo_v3", "nvmlDeviceGetPciInfo_v2"};

    const auto probe = makeProbe();
    ASSERT_TRUE(probe->isAvailable());

    const auto gpus = probe->enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].pciLocation.has_value());
    EXPECT_EQ(gpus[0].pciDeviceId, 0U);
}

// ==========================================================================
// nvmlInit and the driver version
// ==========================================================================

// The DLL is loaded but NVML won't start (a driver mismatch, say): loaded, not available, and the
// destructor frees the module without an nvmlShutdown to match no nvmlInit.
TEST_F(NVMLLoaderTest, AFailedInitLeavesTheDllLoadedButNVMLUnavailable)
{
    fakeState().initResult = NVML_ERROR_DRIVER_NOT_LOADED;
    oneDevice();

    {
        const auto probe = makeProbe();
        EXPECT_TRUE(probe->isLoaded());
        EXPECT_FALSE(probe->isAvailable());
        EXPECT_TRUE(probe->enumerateGPUs().empty());
        EXPECT_FALSE(probe->capabilities().hasTemperature);
        EXPECT_EQ(fakeState().initCallCount, 1);

        // A restart retries nvmlInit on the loaded DLL rather than loading it again; a second failure
        // is logged quietly.
        EXPECT_FALSE(probe->restart());
        EXPECT_FALSE(probe->restart());
        EXPECT_EQ(fakeState().initCallCount, 3);
        EXPECT_EQ(fakeLibrary().loadCount, 1);

        fakeState().initResult = NVML_SUCCESS;
        EXPECT_TRUE(probe->restart());
        EXPECT_TRUE(probe->isAvailable());
    }

    EXPECT_EQ(fakeState().shutdownCallCount, 1) << "Only the successful nvmlInit is shut down";
    EXPECT_EQ(fakeLibrary().freeCount, 1);
}

TEST_F(NVMLLoaderTest, AnUnavailableDriverVersionStillInitialises)
{
    fakeState().driverVersionResult = NVML_ERROR_INSUFFICIENT_SIZE;
    oneDevice();

    const auto probe = makeProbe();
    EXPECT_TRUE(probe->isAvailable());
    EXPECT_EQ(probe->enumerateGPUs().size(), 1U);
}

// ==========================================================================
// Shutdown
// ==========================================================================

TEST_F(NVMLLoaderTest, DestructionShutsNVMLDownAndFreesTheDllOnce)
{
    oneDevice();
    {
        const auto probe = makeProbe();
        ASSERT_TRUE(probe->isAvailable());
        [[maybe_unused]] const auto gpus = probe->enumerateGPUs();
        EXPECT_EQ(fakeState().shutdownCallCount, 0);
        EXPECT_EQ(fakeLibrary().freeCount, 0);
    }
    EXPECT_EQ(fakeState().shutdownCallCount, 1);
    EXPECT_EQ(fakeLibrary().freeCount, 1);
    EXPECT_EQ(fakeLibrary().lastFreed, fakeModule());
    EXPECT_EQ(fakeLibrary().loadCount, 1);
}

// A restart of a loaded NVML shuts it down and starts it again on the same module.
TEST_F(NVMLLoaderTest, RestartReusesTheLoadedDll)
{
    oneDevice();
    {
        const auto probe = makeProbe();
        EXPECT_TRUE(probe->restart());
        EXPECT_EQ(fakeLibrary().loadCount, 1);
        EXPECT_EQ(fakeState().initCallCount, 2);
        EXPECT_EQ(fakeState().shutdownCallCount, 1);
    }
    EXPECT_EQ(fakeState().shutdownCallCount, 2);
    EXPECT_EQ(fakeLibrary().freeCount, 1);
}

// ==========================================================================
// The system loader
// ==========================================================================

// systemLibrary()'s GetProcAddress and FreeLibrary wrappers, on a module every process has (its
// loadLibrary() is nvml.dll's, which CI hasn't).
TEST(NVMLSystemLibraryTest, ResolvesAndFreesARealModule)
{
    const NVMLLibraryFunctions library = NVMLGPUProbe::systemLibrary();
    ASSERT_NE(library.loadLibrary, nullptr);
    ASSERT_NE(library.getProcAddress, nullptr);
    ASSERT_NE(library.freeLibrary, nullptr);

    // A reference of the test's own, which freeLibrary() releases.
    HMODULE kernel32 = LoadLibraryExW(L"kernel32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(kernel32, nullptr);
    EXPECT_NE(library.getProcAddress(kernel32, "GetTickCount64"), nullptr);
    EXPECT_EQ(library.getProcAddress(kernel32, "nvmlInit"), nullptr);
    library.freeLibrary(kernel32);
}

} // namespace
} // namespace Platform

#endif // _WIN32
