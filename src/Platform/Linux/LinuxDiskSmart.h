#pragma once

// Linux disk health through udisks2 (#1631): what udisks2 reports for one drive, and how that becomes
// PhysicalDisk's health. SystemdBus.cpp reads it over D-Bus; everything here is plain data so the
// fixture tests run without a bus.
//
// udisks2 caches each drive's SMART data and refreshes it itself, so reading it is unprivileged: an
// ATA drive's org.freedesktop.UDisks2.Drive.Ata properties, an NVMe drive's
// org.freedesktop.UDisks2.NVMe.Controller properties plus its SmartGetAttributes() health log.

#include "Platform/ISystemInfoProbe.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::LinuxDiskSmart
{

/// What udisks2 reported for one drive, in its own units.
struct DriveSmart
{
    enum class Kind : std::uint8_t
    {
        None, ///< No SMART interface: a USB stick, a card reader, a virtual disk
        Ata,
        Nvme,
    };
    Kind kind = Kind::None;
    std::uint64_t updated = 0; ///< SmartUpdated: when udisks2 last read the data; 0 is never

    // ATA (Drive.Ata)
    bool smartSupported = false;
    bool smartEnabled = false;
    bool failing = false;
    double temperatureKelvin = 0.0;   ///< 0 is unknown
    std::uint64_t powerOnSeconds = 0; ///< 0 is unknown
    std::int64_t badSectors = -1;     ///< -1 is unknown

    // NVMe (NVMe.Controller)
    std::vector<std::string> criticalWarnings; ///< SmartCriticalWarning: "spare", "temperature", ...
    std::uint16_t nvmeTemperatureKelvin = 0;   ///< 0 is unknown
    bool attributesRead = false;               ///< SmartGetAttributes() answered
    std::string attributesError;               ///< Why not, when it didn't
    std::uint8_t percentUsed = 0;
    std::uint8_t availableSpare = 0;
    std::uint64_t mediaErrors = 0;
};

/// One drive's read: the data, or why there is none.
struct DriveSmartRead
{
    std::optional<DriveSmart> smart;
    std::string error; ///< When !smart: udisks2 isn't reachable, has no such drive, or a call failed
};

/// Reads one /sys/block disk's SMART data by its kernel name ("nvme0n1", "sda").
using SmartReader = std::function<DriveSmartRead(const std::string& blockName)>;

/// udisks2's object path for a block device: /org/freedesktop/UDisks2/block_devices/<name>, with every
/// byte outside [A-Za-z0-9_] written as _xx in hex, as udisks2 builds it.
[[nodiscard]] inline std::string blockObjectPath(std::string_view blockName)
{
    std::string path = "/org/freedesktop/UDisks2/block_devices/";
    for (const char character : blockName)
    {
        const auto byte = static_cast<unsigned char>(character);
        const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '_';
        path += plain ? std::string(1, character) : std::format("_{:02x}", byte);
    }
    return path;
}

/// udisks2's critical-warning names as the NVMe health log's bits (NVMe base spec, SMART log byte 0).
[[nodiscard]] inline std::uint8_t criticalWarningBits(const std::vector<std::string>& warnings)
{
    struct Bit
    {
        std::string_view name;
        std::uint8_t mask;
    };
    constexpr std::array BITS{
        Bit{.name = "spare", .mask = 0x01},
        Bit{.name = "temperature", .mask = 0x02},
        Bit{.name = "degraded", .mask = 0x04},
        Bit{.name = "readonly", .mask = 0x08},
        Bit{.name = "volatile_mem", .mask = 0x10},
        Bit{.name = "pmr_readonly", .mask = 0x20},
    };
    std::uint8_t bits = 0;
    for (const std::string& warning : warnings)
    {
        for (const Bit& bit : BITS)
        {
            if (warning == bit.name)
            {
                bits = static_cast<std::uint8_t>(bits | bit.mask);
            }
        }
    }
    return bits;
}

/// Whole degrees Celsius from udisks2's Kelvin; nullopt for its 0 (unknown) or a nonsense reading.
[[nodiscard]] inline std::optional<int> celsiusFromKelvin(double kelvin)
{
    constexpr double ABSOLUTE_ZERO_CELSIUS = -273.15;
    constexpr double PLAUSIBLE_MIN_KELVIN = 200.0; // -73 C
    constexpr double PLAUSIBLE_MAX_KELVIN = 400.0; // 127 C
    if (!std::isfinite(kelvin) || kelvin < PLAUSIBLE_MIN_KELVIN || kelvin > PLAUSIBLE_MAX_KELVIN)
    {
        return std::nullopt;
    }
    return static_cast<int>(std::lround(kelvin + ABSOLUTE_ZERO_CELSIUS));
}

/// @p read into @p disk's health (or the reason there is none), and its temperature when hwmon gave none.
inline void applySmart(PhysicalDisk& disk, const DriveSmartRead& read)
{
    if (!read.smart.has_value())
    {
        disk.healthUnavailableReason = read.error;
        return;
    }
    const DriveSmart& smart = *read.smart;
    if (smart.kind == DriveSmart::Kind::None)
    {
        disk.healthUnavailableReason = "udisks2 has no SMART data for this drive";
        return;
    }
    if (smart.kind == DriveSmart::Kind::Ata && !smart.smartSupported)
    {
        disk.healthUnavailableReason = "This drive doesn't support SMART";
        return;
    }
    if (smart.kind == DriveSmart::Kind::Ata && !smart.smartEnabled)
    {
        disk.healthUnavailableReason = "SMART is turned off on this drive";
        return;
    }
    if (smart.updated == 0)
    {
        disk.healthUnavailableReason = "udisks2 hasn't read this drive's SMART data yet";
        return;
    }

    std::optional<int> temperature;
    if (smart.kind == DriveSmart::Kind::Ata)
    {
        constexpr std::uint64_t SECONDS_PER_HOUR = 3600;
        AtaHealth health;
        health.failing = smart.failing;
        if (smart.badSectors >= 0)
        {
            health.badSectors = static_cast<std::uint64_t>(smart.badSectors);
        }
        if (smart.powerOnSeconds != 0)
        {
            health.powerOnHours = smart.powerOnSeconds / SECONDS_PER_HOUR;
        }
        disk.ataHealth = health;
        temperature = celsiusFromKelvin(smart.temperatureKelvin);
    }
    else
    {
        if (!smart.attributesRead)
        {
            disk.healthUnavailableReason = smart.attributesError.empty() ? "udisks2 didn't return the drive's health log"
                                                                         : "udisks2 didn't return the health log: " + smart.attributesError;
        }
        else
        {
            disk.health = NvmeHealth{.criticalWarning = criticalWarningBits(smart.criticalWarnings),
                                     .availableSparePercent = smart.availableSpare,
                                     .percentageUsed = smart.percentUsed,
                                     .mediaErrors = smart.mediaErrors};
        }
        temperature = celsiusFromKelvin(static_cast<double>(smart.nvmeTemperatureKelvin));
    }
    if (!disk.temperatureCelsius.has_value())
    {
        disk.temperatureCelsius = temperature;
    }
    if (disk.health.has_value() || disk.ataHealth.has_value())
    {
        disk.healthUnavailableReason.clear();
    }
}

} // namespace Platform::LinuxDiskSmart
