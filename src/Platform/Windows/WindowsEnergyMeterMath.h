#pragma once

// The pure part of the Windows Energy Meter Interface (EMI) reader (#2587): the layouts of what the
// energy meter driver answers IOCTL_EMI_GET_METADATA and IOCTL_EMI_GET_MEASUREMENT with, parsed out
// of byte buffers, which channels are a CPU package's, and the units. Includes no Windows header, so
// its tests build on every platform (tests/Platform/WindowsMath); WindowsEnergyMeter.cpp checks these
// offsets against the SDK's emi.h.
//
// Why bytes, not the SDK's structs: EMI_METADATA_V2 lists its channels back to back, each
// EMI_CHANNEL_V2_LENGTH(nameSize) = 6 + nameSize bytes long with no padding (emi.h's
// EMI_CHANNEL_V2_NEXT_CHANNEL), so a channel after an odd-length name sits on a 2-byte boundary and
// its 4-byte MeasurementUnit can't be read through a struct pointer without undefined behaviour.

#include "Platform/AsciiCase.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{

/// EMI_VERSION::EmiVersion: one channel named by EMI_METADATA_V1::MeteredHardwareName.
inline constexpr std::uint16_t EMI_INTERFACE_V1 = 1;
/// EMI_VERSION::EmiVersion: EMI_METADATA_V2's channel list and EMI_MEASUREMENT_DATA_V2's array.
inline constexpr std::uint16_t EMI_INTERFACE_V2 = 2;
/// EMI_MEASUREMENT_UNIT::EmiMeasurementUnitPicowattHours, the only unit emi.h defines.
inline constexpr std::uint32_t EMI_UNIT_PICOWATT_HOURS = 0;

/// EMI_NAME_MAX: HardwareOEM and HardwareModel are WCHAR[16] each.
inline constexpr std::size_t EMI_NAME_CHARS = 16;
/// EMI_METADATA_V1 up to MeteredHardwareName: MeasurementUnit (4), the two names (2 x 32),
/// HardwareRevision (2), MeteredHardwareNameSize (2).
inline constexpr std::size_t EMI_METADATA_V1_HEADER_BYTES = 4 + (2 * EMI_NAME_CHARS * sizeof(char16_t)) + 2 + 2;
/// EMI_METADATA_V2 up to Channels: the two names (2 x 32), HardwareRevision (2), ChannelCount (2).
inline constexpr std::size_t EMI_METADATA_V2_HEADER_BYTES = (2 * EMI_NAME_CHARS * sizeof(char16_t)) + 2 + 2;
/// EMI_CHANNEL_V2 up to ChannelName: MeasurementUnit (4), ChannelNameSize (2).
inline constexpr std::size_t EMI_CHANNEL_V2_HEADER_BYTES = 4 + 2;
/// EMI_CHANNEL_MEASUREMENT_DATA: AbsoluteEnergy (8) then AbsoluteTime (8).
inline constexpr std::size_t EMI_MEASUREMENT_BYTES = 8 + 8;
/// The most metadata read from one device: a real block is a few hundred bytes (a dozen channels with
/// short names); a driver that claims more than this is not read.
inline constexpr std::uint32_t EMI_METADATA_SIZE_CAP_BYTES = 64 * 1024;
/// The most channels taken from one device, so a corrupt ChannelCount can't make the measurement
/// buffer huge; the Intel driver exposes a handful per package.
inline constexpr std::size_t EMI_CHANNEL_CAP = 256;

/// One channel the metadata names.
struct EmiChannel
{
    std::string name;       ///< UTF-16 in the driver's block, narrowed: the names are ASCII ("RAPL_Package0_PKG")
    std::uint32_t unit = 0; ///< EMI_MEASUREMENT_UNIT

    bool operator==(const EmiChannel&) const = default;
};

namespace Detail
{

template<typename T> [[nodiscard]] inline std::optional<T> readAt(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
    {
        return std::nullopt;
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

/// A UTF-16 name of @p sizeBytes at @p offset, narrowed to ASCII (a code unit past 0x7F becomes '?',
/// so a name never holds a partial UTF-8 sequence) and cut at its first NUL.
[[nodiscard]] inline std::optional<std::string> readName(std::span<const std::byte> bytes, std::size_t offset, std::size_t sizeBytes)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeBytes)
    {
        return std::nullopt;
    }
    std::string name;
    name.reserve(sizeBytes / sizeof(char16_t));
    for (std::size_t i = 0; i + sizeof(char16_t) <= sizeBytes; i += sizeof(char16_t))
    {
        char16_t unit = 0;
        std::memcpy(&unit, bytes.data() + offset + i, sizeof(unit));
        if (unit == 0)
        {
            break;
        }
        name.push_back(unit < 0x80 ? static_cast<char>(unit) : '?'); // ASCII code unit; see above
    }
    return name;
}

} // namespace Detail

/// The channels an EMI device's metadata block names, for the interface @p version its
/// IOCTL_EMI_GET_VERSION answered (EMI_INTERFACE_V1: one channel, named by MeteredHardwareName;
/// EMI_INTERFACE_V2: ChannelCount channels). nullopt for another version or a block too short for
/// what its own counts claim; a V2 block with no channels is an empty list.
[[nodiscard]] inline std::optional<std::vector<EmiChannel>> parseEmiMetadata(std::uint16_t version, std::span<const std::byte> metadata)
{
    std::vector<EmiChannel> channels;
    if (version == EMI_INTERFACE_V1)
    {
        const auto unit = Detail::readAt<std::uint32_t>(metadata, 0);
        const auto nameSize = Detail::readAt<std::uint16_t>(metadata, EMI_METADATA_V1_HEADER_BYTES - 2);
        if (!unit.has_value() || !nameSize.has_value())
        {
            return std::nullopt;
        }
        auto name = Detail::readName(metadata, EMI_METADATA_V1_HEADER_BYTES, *nameSize);
        if (!name.has_value())
        {
            return std::nullopt;
        }
        channels.push_back(EmiChannel{.name = std::move(*name), .unit = *unit});
        return channels;
    }
    if (version != EMI_INTERFACE_V2)
    {
        return std::nullopt;
    }
    const auto count = Detail::readAt<std::uint16_t>(metadata, EMI_METADATA_V2_HEADER_BYTES - 2);
    if (!count.has_value() || *count > EMI_CHANNEL_CAP)
    {
        return std::nullopt;
    }
    channels.reserve(*count);
    std::size_t offset = EMI_METADATA_V2_HEADER_BYTES;
    for (std::uint16_t i = 0; i < *count; ++i)
    {
        const auto unit = Detail::readAt<std::uint32_t>(metadata, offset);
        const auto nameSize = Detail::readAt<std::uint16_t>(metadata, offset + 4);
        if (!unit.has_value() || !nameSize.has_value())
        {
            return std::nullopt;
        }
        auto name = Detail::readName(metadata, offset + EMI_CHANNEL_V2_HEADER_BYTES, *nameSize);
        if (!name.has_value())
        {
            return std::nullopt;
        }
        channels.push_back(EmiChannel{.name = std::move(*name), .unit = *unit});
        offset += EMI_CHANNEL_V2_HEADER_BYTES + *nameSize; // EMI_CHANNEL_V2_NEXT_CHANNEL: no padding
    }
    return channels;
}

/// Whether @p name is a CPU package's energy channel: the RAPL package domain, "RAPL_Package<N>_PKG"
/// as the Intel driver names it, ASCII case ignored. A package's PP0 (cores), PP1 (graphics), DRAM
/// and PSYS (platform) channels are not: PP0/PP1/DRAM are parts already in the package's figure, and
/// PSYS is the whole platform, not the CPU the processes' time is spent on. Linux reads the same
/// domain (powercap's intel-rapl:N, the package zone), so the two platforms' figures agree.
[[nodiscard]] constexpr bool isPackageChannelName(std::string_view name) noexcept
{
    constexpr std::string_view PREFIX = "rapl_package";
    constexpr std::string_view SUFFIX = "_pkg";
    if (name.size() < PREFIX.size() + SUFFIX.size())
    {
        return false;
    }
    return equalsIgnoreCase(name.substr(0, PREFIX.size()), PREFIX) && equalsIgnoreCase(name.substr(name.size() - SUFFIX.size()), SUFFIX);
}

/// Picowatt-hours, the EMI unit, as microjoules, the unit PackageEnergyReading carries: 1 pWh is
/// 3.6 nJ, so 2500 pWh are 9 uJ exactly. Split so the product can't overflow for any counter a
/// machine reaches (a 64-bit pWh counter holds 18 million kWh).
[[nodiscard]] constexpr std::uint64_t picowattHoursToMicrojoules(std::uint64_t picowattHours) noexcept
{
    constexpr std::uint64_t MICROJOULES = 9;
    constexpr std::uint64_t PER_PICOWATT_HOURS = 2500;
    return ((picowattHours / PER_PICOWATT_HOURS) * MICROJOULES) +
           (((picowattHours % PER_PICOWATT_HOURS) * MICROJOULES) / PER_PICOWATT_HOURS);
}

/// Channel @p index's AbsoluteEnergy out of a measurement block (EMI_MEASUREMENT_DATA_V2's
/// ChannelData, or V1's one EMI_CHANNEL_MEASUREMENT_DATA at index 0); nullopt if the block is too
/// short for it (a device answering fewer channels than its metadata named).
[[nodiscard]] inline std::optional<std::uint64_t> emiChannelEnergy(std::span<const std::byte> measurement, std::size_t index) noexcept
{
    if (index >= EMI_CHANNEL_CAP)
    {
        return std::nullopt;
    }
    return Detail::readAt<std::uint64_t>(measurement, index * EMI_MEASUREMENT_BYTES);
}

/// The system-wide CPU time spent running processes, from GetSystemTimes' three totals (kernel
/// includes idle): the share denominator for package energy attribution (PackageEnergyReading::
/// busyCpuTicks), in the same 100 ns ticks as ProcessCounters' times. GetSystemTimes sums
/// per-processor counters that aren't read atomically, so idle can exceed kernel by a hair (#1303);
/// then the kernel part is taken as 0 rather than wrapping.
[[nodiscard]] constexpr std::uint64_t busyCpuTicks(std::uint64_t idle, std::uint64_t kernel, std::uint64_t user) noexcept
{
    return (kernel > idle ? kernel - idle : 0) + user;
}

} // namespace Platform::Windows
