/// @file test_WindowsEnergyMeter.cpp
/// @brief Platform::Windows::EnergyMeter (#2587) against fake EMI device calls: a CPU package channel
/// found, none found (no meter, or a meter for something else), two packages on one device and across
/// two, a meter that goes away mid-run, a short answer, and a counter that steps back; the handles
/// closed exactly once. Then WindowsProcessProbe over the same fakes: hasPowerUsage follows what the
/// opener thread finds, and readPackageEnergy() carries the raw counters, the other packages and the
/// busy CPU ticks for Domain to share out, as the Linux probe's does.

#include "Domain/ProcessEnergyAttribution.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"
#include "Platform/Windows/WindowsEnergyMeter.h"
#include "Platform/Windows/WindowsEnergyMeterMath.h"
#include "Platform/Windows/WindowsProcessProbe.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <emi.h>
// clang-format on

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// NOLINTBEGIN(performance-no-int-to-ptr) - fake device handles are small integers, never dereferenced
namespace Platform::Windows
{
namespace
{

template<typename T> void append(std::vector<std::byte>& out, T value)
{
    std::array<std::byte, sizeof(T)> raw{};
    std::memcpy(raw.data(), &value, sizeof(T));
    out.insert(out.end(), raw.begin(), raw.end());
}

void appendFixedName(std::vector<std::byte>& out, std::string_view ascii)
{
    for (std::size_t i = 0; i < EMI_NAME_CHARS; ++i)
    {
        append<char16_t>(out, i < ascii.size() ? static_cast<char16_t>(ascii[i]) : char16_t{0});
    }
}

/// An EMI_METADATA_V2 block naming @p channels, all in picowatt-hours.
std::vector<std::byte> metadataV2(const std::vector<std::string>& channels)
{
    std::vector<std::byte> out;
    appendFixedName(out, "Intel");
    appendFixedName(out, "RAPL");
    append<std::uint16_t>(out, 1);
    append<std::uint16_t>(out, static_cast<std::uint16_t>(channels.size()));
    for (const std::string& name : channels)
    {
        append<std::uint32_t>(out, EMI_UNIT_PICOWATT_HOURS);
        append<std::uint16_t>(out, static_cast<std::uint16_t>((name.size() + 1) * sizeof(char16_t)));
        for (const char ch : name)
        {
            append<char16_t>(out, static_cast<char16_t>(ch));
        }
        append<char16_t>(out, 0);
    }
    return out;
}

/// One fake EMI device: its interface path, what its metadata names and what each channel reads.
struct FakeDevice
{
    std::wstring path;
    std::uint16_t version = EMI_INTERFACE_V2;
    std::vector<std::byte> metadata;
    std::vector<std::uint64_t> energiesPwh; ///< One per channel, in picowatt-hours
    bool failOpen = false;
    bool failMetadata = false;
    bool failMeasurement = false;
    DWORD measurementBytesCap = 0; ///< > 0: answer at most this many bytes (a short answer)
};

/// The fakes' world, set per test. Counters are atomics: the probe's opener thread bumps them.
struct FakeEmi
{
    std::vector<FakeDevice> devices;
    std::atomic<int> enumerations{0};
    std::atomic<int> opens{0};
    std::atomic<int> closes{0};
    std::atomic<int> measurements{0};

    void reset()
    {
        devices.clear();
        enumerations = 0;
        opens = 0;
        closes = 0;
        measurements = 0;
    }
};

FakeEmi& fake()
{
    static FakeEmi instance;
    return instance;
}

HANDLE handleFor(std::size_t deviceIndex)
{
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(deviceIndex + 1));
}

std::optional<std::size_t> deviceOf(HANDLE handle)
{
    const auto value = reinterpret_cast<std::uintptr_t>(handle);
    if (value == 0 || value > fake().devices.size())
    {
        return std::nullopt;
    }
    return value - 1;
}

std::vector<std::wstring> fakeEnumerate()
{
    ++fake().enumerations;
    std::vector<std::wstring> paths;
    for (const FakeDevice& device : fake().devices)
    {
        paths.push_back(device.path);
    }
    return paths;
}

HANDLE WINAPI fakeCreateFile(LPCWSTR path,
                             DWORD access,
                             DWORD /*share*/,
                             LPSECURITY_ATTRIBUTES /*security*/,
                             DWORD disposition,
                             DWORD /*flags*/,
                             HANDLE /*template*/)
{
    EXPECT_EQ(access, static_cast<DWORD>(GENERIC_READ)); // The IOCTLs need FILE_READ_ACCESS, nothing more
    EXPECT_EQ(disposition, static_cast<DWORD>(OPEN_EXISTING));
    for (std::size_t i = 0; i < fake().devices.size(); ++i)
    {
        if (fake().devices[i].path == path)
        {
            if (fake().devices[i].failOpen)
            {
                SetLastError(ERROR_ACCESS_DENIED);
                return INVALID_HANDLE_VALUE;
            }
            ++fake().opens;
            return handleFor(i);
        }
    }
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI fakeCloseHandle(HANDLE handle)
{
    EXPECT_TRUE(deviceOf(handle).has_value());
    ++fake().closes;
    return TRUE;
}

BOOL WINAPI fakeDeviceIoControl(
    HANDLE handle, DWORD code, LPVOID /*in*/, DWORD /*inSize*/, LPVOID out, DWORD outSize, LPDWORD returned, LPOVERLAPPED /*overlapped*/)
{
    const auto index = deviceOf(handle);
    if (!index.has_value())
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const FakeDevice& device = fake().devices[*index];
    std::vector<std::byte> answer;
    if (code == IOCTL_EMI_GET_VERSION)
    {
        append<std::uint16_t>(answer, device.version);
    }
    else if (code == IOCTL_EMI_GET_METADATA_SIZE)
    {
        append<std::uint32_t>(answer, static_cast<std::uint32_t>(device.metadata.size()));
    }
    else if (code == IOCTL_EMI_GET_METADATA)
    {
        if (device.failMetadata)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        answer = device.metadata;
    }
    else if (code == IOCTL_EMI_GET_MEASUREMENT)
    {
        ++fake().measurements;
        if (device.failMeasurement)
        {
            SetLastError(ERROR_DEVICE_NOT_CONNECTED);
            return FALSE;
        }
        std::uint64_t time = 133'000'000'000'000'000;
        for (const std::uint64_t energy : device.energiesPwh)
        {
            append<std::uint64_t>(answer, energy);
            append<std::uint64_t>(answer, time++);
        }
        if (device.measurementBytesCap > 0 && answer.size() > device.measurementBytesCap)
        {
            answer.resize(device.measurementBytesCap);
        }
    }
    else
    {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    if (answer.size() > outSize)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(out, answer.data(), answer.size());
    *returned = static_cast<DWORD>(answer.size());
    return TRUE;
}

EnergyMeterFunctions fakeApi()
{
    return {.enumerateMeters = &fakeEnumerate,
            .createFile = &fakeCreateFile,
            .deviceIoControl = &fakeDeviceIoControl,
            .closeHandle = &fakeCloseHandle};
}

/// An Intel-style meter: the package, its cores, its graphics and its DRAM.
FakeDevice intelMeter(std::wstring path, std::uint64_t packagePwh)
{
    return FakeDevice{.path = std::move(path),
                      .metadata = metadataV2({"RAPL_Package0_PKG", "RAPL_Package0_PP0", "RAPL_Package0_PP1", "RAPL_Package0_DRAM"}),
                      .energiesPwh = {packagePwh, packagePwh / 2, packagePwh / 10, packagePwh / 5}};
}

/// Polls @p condition for up to @p timeout.
bool waitFor(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

class WindowsEnergyMeterTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake().reset();
    }
};

// -----------------------------------------------------------------------------------------------
// EnergyMeter
// -----------------------------------------------------------------------------------------------

TEST_F(WindowsEnergyMeterTest, PackageChannelFoundAndReadInMicrojoules)
{
    constexpr std::uint64_t PACKAGE_PWH = 1'000'000'000'000; // 1 Wh = 3600 J
    fake().devices.push_back(intelMeter(LR"(\\?\acpi#int3xyz#{45bd8344-7ed6-49cf-a440-c276c933b053})", PACKAGE_PWH));
    {
        EnergyMeter meter = EnergyMeter::open(fakeApi());
        ASSERT_TRUE(meter.hasPackageChannels());
        ASSERT_EQ(meter.packageChannels().size(), 1U);
        EXPECT_EQ(meter.packageChannels()[0].name, "RAPL_Package0_PKG");
        EXPECT_EQ(meter.packageChannels()[0].device, 0U);
        EXPECT_EQ(meter.packageChannels()[0].channel, 0U);
        EXPECT_EQ(fake().opens.load(), 1);
        EXPECT_EQ(fake().closes.load(), 0); // Kept open for the samples

        std::vector<std::uint64_t> energyUj;
        ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));
        ASSERT_EQ(energyUj.size(), 1U);
        EXPECT_EQ(energyUj[0], 3'600'000'000ULL);
        EXPECT_EQ(fake().measurements.load(), 1);

        // The counter moves on: the next read is the new value, raw; Domain takes the delta.
        fake().devices[0].energiesPwh[0] += 2'777'777'778; // +10 J
        ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));
        EXPECT_NEAR(static_cast<double>(energyUj[0] - 3'600'000'000ULL), 10'000'000.0, 1.0);
    }
    EXPECT_EQ(fake().closes.load(), 1); // Closed exactly once, on destruction
}

TEST_F(WindowsEnergyMeterTest, NoEnergyMeterDeviceMeansNoPower)
{
    EnergyMeter meter = EnergyMeter::open(fakeApi());
    EXPECT_FALSE(meter.hasPackageChannels());
    EXPECT_TRUE(meter.packageChannels().empty());
    std::vector<std::uint64_t> energyUj{42};
    EXPECT_FALSE(meter.readPackageEnergyUj(energyUj));
    EXPECT_TRUE(energyUj.empty());
    EXPECT_EQ(fake().enumerations.load(), 1);
    EXPECT_EQ(fake().opens.load(), 0);
}

TEST_F(WindowsEnergyMeterTest, AMeterWithoutAPackageChannelIsClosedAtOpen)
{
    // A meter for something else (a display backlight, say): its channels are read and it is let go.
    fake().devices.push_back(
        FakeDevice{.path = L"\\\\?\\display-meter", .metadata = metadataV2({"Display", "Backlight"}), .energiesPwh = {1, 2}});
    // And one whose metadata can't be read at all.
    fake().devices.push_back(FakeDevice{
        .path = L"\\\\?\\broken-meter", .metadata = metadataV2({"RAPL_Package0_PKG"}), .energiesPwh = {1}, .failMetadata = true});
    // And one that won't open.
    fake().devices.push_back(
        FakeDevice{.path = L"\\\\?\\locked-meter", .metadata = metadataV2({"RAPL_Package0_PKG"}), .energiesPwh = {1}, .failOpen = true});
    const EnergyMeter meter = EnergyMeter::open(fakeApi());
    EXPECT_FALSE(meter.hasPackageChannels());
    EXPECT_EQ(fake().opens.load(), 2);
    EXPECT_EQ(fake().closes.load(), 2); // Both let go at open, not kept for samples
    EXPECT_EQ(fake().measurements.load(), 0);
}

TEST_F(WindowsEnergyMeterTest, TwoPackagesOnOneDeviceAreOrderedByName)
{
    // Listed out of order by the driver: Package1 before Package0, with the parts between.
    fake().devices.push_back(
        FakeDevice{.path = L"\\\\?\\two-socket",
                   .metadata = metadataV2({"RAPL_Package1_PKG", "RAPL_Package1_PP0", "RAPL_Package0_PKG", "RAPL_Package0_PP0"}),
                   .energiesPwh = {2'500'000, 1, 5'000'000, 2}});
    EnergyMeter meter = EnergyMeter::open(fakeApi());
    ASSERT_EQ(meter.packageChannels().size(), 2U);
    EXPECT_EQ(meter.packageChannels()[0].name, "RAPL_Package0_PKG");
    EXPECT_EQ(meter.packageChannels()[0].channel, 2U);
    EXPECT_EQ(meter.packageChannels()[1].name, "RAPL_Package1_PKG");
    EXPECT_EQ(meter.packageChannels()[1].channel, 0U);
    std::vector<std::uint64_t> energyUj;
    ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));
    ASSERT_EQ(energyUj.size(), 2U);
    EXPECT_EQ(energyUj[0], picowattHoursToMicrojoules(5'000'000)); // Package0 first
    EXPECT_EQ(energyUj[1], picowattHoursToMicrojoules(2'500'000));
    EXPECT_EQ(fake().measurements.load(), 1); // One device, one IOCTL per sample
}

TEST_F(WindowsEnergyMeterTest, TwoDevicesEachWithAPackageAreBothRead)
{
    fake().devices.push_back(
        FakeDevice{.path = L"\\\\?\\socket-b", .metadata = metadataV2({"RAPL_Package1_PKG"}), .energiesPwh = {250'000}});
    fake().devices.push_back(
        FakeDevice{.path = L"\\\\?\\socket-a", .metadata = metadataV2({"RAPL_Package0_PKG"}), .energiesPwh = {500'000}});
    {
        EnergyMeter meter = EnergyMeter::open(fakeApi());
        ASSERT_EQ(meter.packageChannels().size(), 2U);
        EXPECT_EQ(meter.packageChannels()[0].name, "RAPL_Package0_PKG");
        EXPECT_EQ(meter.packageChannels()[0].device, 1U);
        EXPECT_EQ(meter.packageChannels()[1].device, 0U);
        std::vector<std::uint64_t> energyUj;
        ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));
        ASSERT_EQ(energyUj.size(), 2U);
        EXPECT_EQ(energyUj[0], picowattHoursToMicrojoules(500'000));
        EXPECT_EQ(energyUj[1], picowattHoursToMicrojoules(250'000));
        EXPECT_EQ(fake().measurements.load(), 2);
    }
    EXPECT_EQ(fake().closes.load(), 2);
}

TEST_F(WindowsEnergyMeterTest, V1MeterIsReadAsOneChannel)
{
    std::vector<std::byte> v1;
    append<std::uint32_t>(v1, EMI_UNIT_PICOWATT_HOURS);
    appendFixedName(v1, "Contoso");
    appendFixedName(v1, "Meter");
    append<std::uint16_t>(v1, 1);
    const std::string name = "RAPL_Package0_PKG";
    append<std::uint16_t>(v1, static_cast<std::uint16_t>((name.size() + 1) * sizeof(char16_t)));
    for (const char ch : name)
    {
        append<char16_t>(v1, static_cast<char16_t>(ch));
    }
    append<char16_t>(v1, 0);
    fake().devices.push_back(FakeDevice{.path = L"\\\\?\\v1-meter", .version = EMI_INTERFACE_V1, .metadata = v1, .energiesPwh = {2'500}});
    EnergyMeter meter = EnergyMeter::open(fakeApi());
    ASSERT_EQ(meter.packageChannels().size(), 1U);
    std::vector<std::uint64_t> energyUj;
    ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));
    ASSERT_EQ(energyUj.size(), 1U);
    EXPECT_EQ(energyUj[0], 9U);
}

TEST_F(WindowsEnergyMeterTest, AMeterThatGoesAwayMakesTheSampleUnread)
{
    fake().devices.push_back(intelMeter(L"\\\\?\\socket-a", 5'000'000));
    fake().devices.push_back(
        FakeDevice{.path = L"\\\\?\\socket-b", .metadata = metadataV2({"RAPL_Package1_PKG"}), .energiesPwh = {250'000}});
    EnergyMeter meter = EnergyMeter::open(fakeApi());
    ASSERT_EQ(meter.packageChannels().size(), 2U);
    std::vector<std::uint64_t> energyUj;
    ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));

    // The second socket's meter stops answering: the whole sample is unread, not one package's
    // energy shared out over both packages' CPU time.
    fake().devices[1].failMeasurement = true;
    EXPECT_FALSE(meter.readPackageEnergyUj(energyUj));
    EXPECT_TRUE(energyUj.empty());
    EXPECT_EQ(meter.lastReadError(), static_cast<DWORD>(ERROR_DEVICE_NOT_CONNECTED));
    EXPECT_TRUE(meter.hasPackageChannels()); // Still claimed: the next read may succeed

    fake().devices[1].failMeasurement = false;
    ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));
    EXPECT_EQ(energyUj.size(), 2U);
}

TEST_F(WindowsEnergyMeterTest, AShortMeasurementAnswerIsUnread)
{
    FakeDevice device = intelMeter(L"\\\\?\\socket-a", 5'000'000);
    device.measurementBytesCap = static_cast<DWORD>(2 * EMI_MEASUREMENT_BYTES); // 2 of the 4 channels named
    fake().devices.push_back(device);
    EnergyMeter meter = EnergyMeter::open(fakeApi());
    std::vector<std::uint64_t> energyUj;
    EXPECT_FALSE(meter.readPackageEnergyUj(energyUj));
    EXPECT_TRUE(energyUj.empty());
    EXPECT_EQ(meter.lastReadError(), static_cast<DWORD>(ERROR_INSUFFICIENT_BUFFER));
}

TEST_F(WindowsEnergyMeterTest, ACounterThatStepsBackIsPassedThroughRawAndCreditsNothing)
{
    // The driver's 64-bit counter can only step back if the driver restarted. The meter passes the
    // raw value on; with no wrap point (maxRangeUj 0) Domain's delta credits that interval nothing
    // and the next interval starts from the new value, as Linux's does for an unknown range.
    fake().devices.push_back(intelMeter(L"\\\\?\\socket-a", 1'000'000'000'000));
    EnergyMeter meter = EnergyMeter::open(fakeApi());
    std::vector<std::uint64_t> energyUj;
    ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));
    const std::uint64_t before = energyUj[0];
    fake().devices[0].energiesPwh[0] = 400'000'000'000;
    ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));
    const std::uint64_t after = energyUj[0];
    EXPECT_LT(after, before);
    EXPECT_EQ(Domain::ProcessEnergy::energyDeltaUj(before, after, 0), 0U);
    // And the interval after that is measured from the new value.
    fake().devices[0].energiesPwh[0] += 2'500'000; // + 9 mJ
    ASSERT_TRUE(meter.readPackageEnergyUj(energyUj));
    EXPECT_EQ(Domain::ProcessEnergy::energyDeltaUj(after, energyUj[0], 0), 9'000U);
}

TEST_F(WindowsEnergyMeterTest, ADefaultMeterReadsNothing)
{
    EnergyMeter meter;
    EXPECT_FALSE(meter.hasPackageChannels());
    std::vector<std::uint64_t> energyUj;
    EXPECT_FALSE(meter.readPackageEnergyUj(energyUj));
}

// -----------------------------------------------------------------------------------------------
// WindowsProcessProbe over the fakes: the capability and the reading Domain gets
// -----------------------------------------------------------------------------------------------

WindowsProcessProbeFunctions probeFunctions(std::optional<EnergyMeterFunctions> energyMeter)
{
    WindowsProcessProbeFunctions functions = WindowsProcessProbe::systemFunctions();
    functions.energyMeter = energyMeter;
    return functions;
}

TEST_F(WindowsEnergyMeterTest, ProbeClaimsPowerOnceTheOpenerFindsAPackageChannel)
{
    fake().devices.push_back(intelMeter(L"\\\\?\\socket-a", 5'000'000'000'000));
    fake().devices.push_back(
        FakeDevice{.path = L"\\\\?\\socket-b", .metadata = metadataV2({"RAPL_Package1_PKG"}), .energiesPwh = {250'000}});
    {
        const WindowsProcessProbe probe(probeFunctions(fakeApi()));
        // The opener runs on its own thread (never the one building the probe); the capability follows it.
        ASSERT_TRUE(waitFor([&] { return probe.capabilities().hasPowerUsage; }));
        EXPECT_EQ(fake().enumerations.load(), 1);

        // Plain values, as gtest's ASSERT_TRUE isn't modelled by clang-tidy's optional dataflow: a
        // missing reading or energy reads as the zeroes below, which the expectations then reject.
        ASSERT_TRUE(probe.readPackageEnergy().has_value());
        const PackageEnergyReading reading = probe.readPackageEnergy().value_or(PackageEnergyReading{});
        EXPECT_EQ(reading.energyUj.value_or(0), picowattHoursToMicrojoules(5'000'000'000'000));
        EXPECT_EQ(reading.maxRangeUj, 0U); // No wrap point: the driver's counter is 64-bit
        ASSERT_EQ(reading.otherPackages.size(), 1U);
        EXPECT_EQ(reading.otherPackages[0].energyUj, picowattHoursToMicrojoules(250'000));
        EXPECT_EQ(reading.otherPackages[0].maxRangeUj, 0U);
        EXPECT_GT(reading.busyCpuTicks.value_or(0), 0U); // GetSystemTimes, read with the energy

        // A meter that goes away: a reading with no energy (the interval reads as 0 W), not no reading.
        fake().devices[0].failMeasurement = true;
        ASSERT_TRUE(probe.readPackageEnergy().has_value());
        const PackageEnergyReading failed = probe.readPackageEnergy().value_or(
            PackageEnergyReading{.energyUj = 1, .maxRangeUj = 0, .busyCpuTicks = std::nullopt, .otherPackages = {}});
        EXPECT_FALSE(failed.energyUj.has_value());
        EXPECT_TRUE(failed.otherPackages.empty());
        EXPECT_TRUE(probe.capabilities().hasPowerUsage);
        fake().devices[0].failMeasurement = false;
        EXPECT_TRUE(probe.readPackageEnergy().value_or(PackageEnergyReading{}).energyUj.has_value());
    }
    EXPECT_EQ(fake().closes.load(), 2); // The probe's destructor closes both devices
}

TEST_F(WindowsEnergyMeterTest, ProbeWithoutAPackageChannelClaimsNoPower)
{
    fake().devices.push_back(FakeDevice{.path = L"\\\\?\\display-meter", .metadata = metadataV2({"Display"}), .energiesPwh = {1}});
    const WindowsProcessProbe probe(probeFunctions(fakeApi()));
    ASSERT_TRUE(waitFor([&] { return fake().closes == 1; })); // The opener has been and gone
    EXPECT_FALSE(probe.capabilities().hasPowerUsage);
    EXPECT_FALSE(probe.readPackageEnergy().has_value()); // No attribution: enumerate()'s zeros stand
}

TEST_F(WindowsEnergyMeterTest, ProbeWithNoEnergyMeterFunctionsLooksForNone)
{
    const WindowsProcessProbe probe(probeFunctions(std::nullopt));
    EXPECT_FALSE(probe.capabilities().hasPowerUsage);
    EXPECT_FALSE(probe.readPackageEnergy().has_value());
    EXPECT_EQ(fake().enumerations.load(), 0);
}

} // namespace
} // namespace Platform::Windows
// NOLINTEND(performance-no-int-to-ptr)
