/// @file test_WindowsEnergyMeterMath.cpp
/// @brief WindowsEnergyMeterMath.h (#2587): the EMI metadata and measurement blocks parsed out of
/// bytes, which channel names are a CPU package's, picowatt-hours to microjoules, and the busy CPU
/// ticks. No Windows header, so these build and run on every platform (#1133).

#include "Platform/Windows/WindowsEnergyMeterMath.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{
namespace
{

/// Appends @p value's bytes, little-endian as the driver writes them on every Windows machine.
template<typename T> void append(std::vector<std::byte>& out, T value)
{
    std::array<std::byte, sizeof(T)> raw{};
    std::memcpy(raw.data(), &value, sizeof(T));
    out.insert(out.end(), raw.begin(), raw.end());
}

/// A UTF-16 name with its terminating NUL, as a channel name is stored; returns its size in bytes.
std::uint16_t appendName(std::vector<std::byte>& out, std::string_view ascii)
{
    for (const char ch : ascii)
    {
        append<char16_t>(out, static_cast<char16_t>(ch));
    }
    append<char16_t>(out, 0);
    return static_cast<std::uint16_t>((ascii.size() + 1) * sizeof(char16_t));
}

/// The 16-WCHAR HardwareOEM / HardwareModel fields.
void appendFixedName(std::vector<std::byte>& out, std::string_view ascii)
{
    for (std::size_t i = 0; i < EMI_NAME_CHARS; ++i)
    {
        append<char16_t>(out, i < ascii.size() ? static_cast<char16_t>(ascii[i]) : char16_t{0});
    }
}

/// An EMI_METADATA_V2 block naming @p channels, every one in picowatt-hours unless @p unit says otherwise.
std::vector<std::byte> metadataV2(const std::vector<std::string>& channels, std::uint32_t unit = EMI_UNIT_PICOWATT_HOURS)
{
    std::vector<std::byte> out;
    appendFixedName(out, "Intel");
    appendFixedName(out, "RAPL");
    append<std::uint16_t>(out, 1); // HardwareRevision
    append<std::uint16_t>(out, static_cast<std::uint16_t>(channels.size()));
    for (const std::string& name : channels)
    {
        append<std::uint32_t>(out, unit);
        std::vector<std::byte> nameBytes;
        const std::uint16_t size = appendName(nameBytes, name);
        append<std::uint16_t>(out, size);
        out.insert(out.end(), nameBytes.begin(), nameBytes.end());
    }
    return out;
}

/// An EMI_METADATA_V1 block: one channel, MeteredHardwareName.
std::vector<std::byte> metadataV1(std::string_view name, std::uint32_t unit = EMI_UNIT_PICOWATT_HOURS)
{
    std::vector<std::byte> out;
    append<std::uint32_t>(out, unit);
    appendFixedName(out, "Contoso");
    appendFixedName(out, "Meter");
    append<std::uint16_t>(out, 2); // HardwareRevision
    std::vector<std::byte> nameBytes;
    const std::uint16_t size = appendName(nameBytes, name);
    append<std::uint16_t>(out, size);
    out.insert(out.end(), nameBytes.begin(), nameBytes.end());
    return out;
}

/// An EMI_MEASUREMENT_DATA_V2 block: each channel's AbsoluteEnergy, with a timestamp.
std::vector<std::byte> measurement(const std::vector<std::uint64_t>& energies)
{
    std::vector<std::byte> out;
    std::uint64_t time = 133'000'000'000'000'000;
    for (const std::uint64_t energy : energies)
    {
        append<std::uint64_t>(out, energy);
        append<std::uint64_t>(out, time++);
    }
    return out;
}

/// parseEmiMetadata()'s list, or an empty one for nullopt: a plain value keeps clang-tidy's optional
/// dataflow satisfied (bugprone-unchecked-optional-access doesn't model gtest's ASSERT_TRUE), and
/// the tests that tell "no channels" from "rejected" check has_value() themselves.
std::vector<EmiChannel> channelsOf(std::uint16_t version, std::span<const std::byte> metadata)
{
    return parseEmiMetadata(version, metadata).value_or(std::vector<EmiChannel>{});
}

// -----------------------------------------------------------------------------------------------
// Layout constants: the SDK's emi.h offsets, which WindowsEnergyMeter.cpp static_asserts against.
// -----------------------------------------------------------------------------------------------

TEST(WindowsEnergyMeterMath, LayoutConstantsMatchEmiHeader)
{
    EXPECT_EQ(EMI_METADATA_V1_HEADER_BYTES, 72U); // unit (4) + 2 names (64) + revision (2) + name size (2)
    EXPECT_EQ(EMI_METADATA_V2_HEADER_BYTES, 68U); // 2 names (64) + revision (2) + channel count (2)
    EXPECT_EQ(EMI_CHANNEL_V2_HEADER_BYTES, 6U);   // unit (4) + name size (2)
    EXPECT_EQ(EMI_MEASUREMENT_BYTES, 16U);        // energy (8) + time (8)
}

// -----------------------------------------------------------------------------------------------
// parseEmiMetadata
// -----------------------------------------------------------------------------------------------

TEST(WindowsEnergyMeterMath, ParsesV2ChannelsAcrossOddAlignment)
{
    // "RAPL_Package0_PKG" is 17 characters: 36 bytes with its NUL, so the next channel starts at
    // 68 + 6 + 36 = 110, a 2-byte boundary. The parser must read it anyway (memcpy, not a cast).
    const auto block = metadataV2({"RAPL_Package0_PKG", "RAPL_Package0_PP0", "RAPL_Package0_DRAM"});
    const std::vector<EmiChannel> channels = channelsOf(EMI_INTERFACE_V2, block);
    ASSERT_EQ(channels.size(), 3U);
    EXPECT_EQ(channels[0], (EmiChannel{.name = "RAPL_Package0_PKG", .unit = EMI_UNIT_PICOWATT_HOURS}));
    EXPECT_EQ(channels[1].name, "RAPL_Package0_PP0");
    EXPECT_EQ(channels[2].name, "RAPL_Package0_DRAM");
}

TEST(WindowsEnergyMeterMath, ParsesV1AsOneChannel)
{
    const std::vector<EmiChannel> channels = channelsOf(EMI_INTERFACE_V1, metadataV1("RAPL_Package0_PKG"));
    ASSERT_EQ(channels.size(), 1U);
    EXPECT_EQ(channels.front().name, "RAPL_Package0_PKG");
    EXPECT_EQ(channels.front().unit, EMI_UNIT_PICOWATT_HOURS);
}

TEST(WindowsEnergyMeterMath, KeepsTheUnitOfEveryChannel)
{
    constexpr std::uint32_t SOME_OTHER_UNIT = 7;
    const std::vector<EmiChannel> channels = channelsOf(EMI_INTERFACE_V2, metadataV2({"RAPL_Package0_PKG"}, SOME_OTHER_UNIT));
    ASSERT_EQ(channels.size(), 1U);
    EXPECT_EQ(channels.front().unit, SOME_OTHER_UNIT);
}

TEST(WindowsEnergyMeterMath, V2WithNoChannelsIsAnEmptyList)
{
    const std::optional<std::vector<EmiChannel>> channels = parseEmiMetadata(EMI_INTERFACE_V2, metadataV2({}));
    ASSERT_TRUE(channels.has_value());
    EXPECT_TRUE(channels.value_or(std::vector<EmiChannel>{EmiChannel{}}).empty());
}

TEST(WindowsEnergyMeterMath, TruncatedBlocksAreRejected)
{
    const auto full = metadataV2({"RAPL_Package0_PKG", "RAPL_Package1_PKG"});
    // Cut in the second channel's name: too short for what its own ChannelNameSize claims.
    const std::vector<std::byte> cut(full.begin(), full.end() - 5);
    EXPECT_FALSE(parseEmiMetadata(EMI_INTERFACE_V2, cut).has_value());
    // Shorter than the header itself.
    const std::vector<std::byte> header(full.begin(), full.begin() + 10);
    EXPECT_FALSE(parseEmiMetadata(EMI_INTERFACE_V2, header).has_value());
    EXPECT_FALSE(parseEmiMetadata(EMI_INTERFACE_V2, {}).has_value());
    // A V1 block cut inside its name.
    const auto v1 = metadataV1("RAPL_Package0_PKG");
    const std::vector<std::byte> v1Cut(v1.begin(), v1.end() - 2);
    EXPECT_FALSE(parseEmiMetadata(EMI_INTERFACE_V1, v1Cut).has_value());
}

TEST(WindowsEnergyMeterMath, UnknownVersionIsRejected)
{
    const auto block = metadataV2({"RAPL_Package0_PKG"});
    EXPECT_FALSE(parseEmiMetadata(3, block).has_value());
    EXPECT_FALSE(parseEmiMetadata(0, block).has_value());
}

TEST(WindowsEnergyMeterMath, AnAbsurdChannelCountIsRejected)
{
    auto block = metadataV2({"RAPL_Package0_PKG"});
    const auto count = static_cast<std::uint16_t>(EMI_CHANNEL_CAP + 1);
    std::memcpy(block.data() + EMI_METADATA_V2_HEADER_BYTES - 2, &count, sizeof(count));
    EXPECT_FALSE(parseEmiMetadata(EMI_INTERFACE_V2, block).has_value());
}

TEST(WindowsEnergyMeterMath, NamesStopAtTheirNulAndNarrowNonAscii)
{
    // A name whose declared size runs past its NUL (padding) is cut at the NUL; a code unit past
    // 0x7F becomes '?' rather than a stray byte.
    std::vector<std::byte> out;
    appendFixedName(out, "OEM");
    appendFixedName(out, "Model");
    append<std::uint16_t>(out, 1);
    append<std::uint16_t>(out, 1);
    append<std::uint32_t>(out, EMI_UNIT_PICOWATT_HOURS);
    append<std::uint16_t>(out, static_cast<std::uint16_t>(5 * sizeof(char16_t)));
    append<char16_t>(out, u'P');
    append<char16_t>(out, u'é'); // é
    append<char16_t>(out, 0);
    append<char16_t>(out, u'X'); // past the NUL: padding
    append<char16_t>(out, u'Y');
    const std::vector<EmiChannel> channels = channelsOf(EMI_INTERFACE_V2, out);
    ASSERT_EQ(channels.size(), 1U);
    EXPECT_EQ(channels.front().name, "P?");
}

// -----------------------------------------------------------------------------------------------
// isPackageChannelName
// -----------------------------------------------------------------------------------------------

TEST(WindowsEnergyMeterMath, PackageChannelsAreTheRaplPackageDomain)
{
    EXPECT_TRUE(isPackageChannelName("RAPL_Package0_PKG"));
    EXPECT_TRUE(isPackageChannelName("RAPL_Package1_PKG"));
    EXPECT_TRUE(isPackageChannelName("rapl_package0_pkg")); // ASCII case ignored
    EXPECT_TRUE(isPackageChannelName("RAPL_Package12_PKG"));
    // A package's parts, the platform and anything else are not.
    EXPECT_FALSE(isPackageChannelName("RAPL_Package0_PP0"));
    EXPECT_FALSE(isPackageChannelName("RAPL_Package0_PP1"));
    EXPECT_FALSE(isPackageChannelName("RAPL_Package0_DRAM"));
    EXPECT_FALSE(isPackageChannelName("RAPL_Package0_PSYS"));
    EXPECT_FALSE(isPackageChannelName("Battery"));
    EXPECT_FALSE(isPackageChannelName("PKG"));
    EXPECT_FALSE(isPackageChannelName("_PKG"));
    EXPECT_FALSE(isPackageChannelName(""));
    EXPECT_FALSE(isPackageChannelName("RAPL_Package"));
}

// -----------------------------------------------------------------------------------------------
// picowattHoursToMicrojoules
// -----------------------------------------------------------------------------------------------

TEST(WindowsEnergyMeterMath, PicowattHoursBecomeMicrojoules)
{
    EXPECT_EQ(picowattHoursToMicrojoules(0), 0U);
    EXPECT_EQ(picowattHoursToMicrojoules(2'500), 9U);                         // 2500 pWh = 9 uJ exactly
    EXPECT_EQ(picowattHoursToMicrojoules(1'000'000'000'000), 3'600'000'000U); // 1 Wh = 3600 J
    EXPECT_EQ(picowattHoursToMicrojoules(1), 0U);                             // 3.6 nJ rounds down
    EXPECT_EQ(picowattHoursToMicrojoules(278), 1U);                           // 1.0008 uJ
    // Deltas of the converted counter are the energy used: 10 J at 0.5 s intervals is 20 W.
    const std::uint64_t before = picowattHoursToMicrojoules(5'000'000'000'000);
    const std::uint64_t after = picowattHoursToMicrojoules(5'000'000'000'000 + 2'777'777'778);
    EXPECT_NEAR(static_cast<double>(after - before), 10'000'000.0, 1.0);
}

TEST(WindowsEnergyMeterMath, PicowattHoursConversionDoesNotOverflow)
{
    // A 64-bit counter's top: 18.4 million kWh. 9x it would overflow; the split product doesn't.
    constexpr std::uint64_t MAX = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t uj = picowattHoursToMicrojoules(MAX);
    // 2^64 pWh * 0.0036 uJ/pWh = 6.64e16 uJ
    EXPECT_GT(uj, 66'000'000'000'000'000ULL);
    EXPECT_LT(uj, 67'000'000'000'000'000ULL);
}

// -----------------------------------------------------------------------------------------------
// emiChannelEnergy
// -----------------------------------------------------------------------------------------------

TEST(WindowsEnergyMeterMath, ReadsEachChannelsEnergyFromTheMeasurementBlock)
{
    const auto block = measurement({100, 200, 300});
    EXPECT_EQ(emiChannelEnergy(block, 0), std::optional<std::uint64_t>{100});
    EXPECT_EQ(emiChannelEnergy(block, 1), std::optional<std::uint64_t>{200});
    EXPECT_EQ(emiChannelEnergy(block, 2), std::optional<std::uint64_t>{300});
    EXPECT_FALSE(emiChannelEnergy(block, 3).has_value()); // Fewer channels answered than asked
    EXPECT_FALSE(emiChannelEnergy({}, 0).has_value());
    EXPECT_FALSE(emiChannelEnergy(block, EMI_CHANNEL_CAP).has_value());
    // A block cut inside a channel's energy doesn't yield that channel.
    const std::vector<std::byte> cut(block.begin(), block.begin() + 20);
    EXPECT_TRUE(emiChannelEnergy(cut, 0).has_value());
    EXPECT_FALSE(emiChannelEnergy(cut, 1).has_value());
}

// -----------------------------------------------------------------------------------------------
// busyCpuTicks
// -----------------------------------------------------------------------------------------------

TEST(WindowsEnergyMeterMath, BusyTicksAreKernelLessIdlePlusUser)
{
    EXPECT_EQ(busyCpuTicks(700, 1'000, 500), 800U);
    EXPECT_EQ(busyCpuTicks(0, 0, 0), 0U);
    // Idle a hair above kernel (the per-processor sums aren't read atomically, #1303): no wrap.
    EXPECT_EQ(busyCpuTicks(1'001, 1'000, 500), 500U);
}

} // namespace
} // namespace Platform::Windows
