#pragma once

// The Linux Sensors section's facts (#1522), read under an injected root ("/" in the app, a fixture
// tree in tests): every hwmon chip's temperature, fan, voltage, current and power inputs
// (/sys/class/hwmon/hwmon*, Documentation/hwmon/sysfs-interface.rst) and the thermal zones
// (/sys/class/thermal/thermal_zone*) that no hwmon chip already reports. Only standard-library file
// access, so the parsing and the fixture tests build and run everywhere. Every file read here is
// world-readable. The facts can also name each reading's input file (SensorSource), which
// LinuxSensorProbe keeps open and re-reads per tick for the live Sensors section.

#include "LinuxOsInfo.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::LinuxSensors
{

/// One hwmon input family: the file prefix, the kind, and the divisor from the file's unit to the
/// kind's (millidegrees, RPM, millivolts, milliamps, microwatts).
struct InputFamily
{
    std::string_view prefix;
    SensorKind kind;
    double divisor;
    std::string_view inputSuffix; ///< "_input", or "_average" for power on chips that only average
};

inline constexpr std::array<InputFamily, 6> INPUT_FAMILIES{{
    {.prefix = "temp", .kind = SensorKind::Temperature, .divisor = 1000.0, .inputSuffix = "_input"},
    {.prefix = "fan", .kind = SensorKind::Fan, .divisor = 1.0, .inputSuffix = "_input"},
    {.prefix = "in", .kind = SensorKind::Voltage, .divisor = 1000.0, .inputSuffix = "_input"},
    {.prefix = "curr", .kind = SensorKind::Current, .divisor = 1000.0, .inputSuffix = "_input"},
    {.prefix = "power", .kind = SensorKind::Power, .divisor = 1'000'000.0, .inputSuffix = "_input"},
    {.prefix = "power", .kind = SensorKind::Power, .divisor = 1'000'000.0, .inputSuffix = "_average"},
}};

/// Where one reading comes from, for the per-tick read: its input file, the divisor from the file's
/// unit to the kind's, and, when the chip's device has one, its runtime power-management status file.
struct SensorSource
{
    std::filesystem::path input;
    double divisor = 1.0;
    std::filesystem::path runtimeStatus; ///< "<chip>/device/power/runtime_status"; empty when it has none
};

/// Whether a device whose runtime PM status reads @p status may have its sensors read without waking
/// it: "active" (awake) or "unsupported" (no runtime PM). A suspended discrete GPU is left asleep, as
/// the GPU probes leave it; its readings are gaps until it wakes.
[[nodiscard]] constexpr bool runtimeStatusAllowsRead(std::string_view status) noexcept
{
    return status.starts_with("active") || status.starts_with("unsupported");
}

/// A whole hwmon value: a signed decimal integer, optionally followed by a newline; nullopt otherwise.
[[nodiscard]] inline std::optional<std::int64_t> parseSysfsInteger(std::string_view text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    if (text.empty())
    {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, 10);
    if (ec != std::errc{} || ptr != last)
    {
        return std::nullopt;
    }
    return value;
}

/// The channel number of an input file name for @p family ("temp3_input" -> 3); nullopt when the name
/// isn't one of that family's inputs.
[[nodiscard]] inline std::optional<unsigned> inputChannel(std::string_view fileName, const InputFamily& family)
{
    if (!fileName.starts_with(family.prefix) || !fileName.ends_with(family.inputSuffix))
    {
        return std::nullopt;
    }
    const std::string_view digits =
        fileName.substr(family.prefix.size(), fileName.size() - family.prefix.size() - family.inputSuffix.size());
    unsigned channel = 0;
    const char* const first = digits.data();
    const char* const last = first + digits.size();
    const auto [ptr, ec] = std::from_chars(first, last, channel, 10);
    if (digits.empty() || ec != std::errc{} || ptr != last)
    {
        return std::nullopt;
    }
    return channel;
}

/// The label a channel without a driver label gets: "temp1", "fan2", "in0", as `sensors` prints it.
[[nodiscard]] inline std::string defaultLabel(const InputFamily& family, unsigned channel)
{
    return std::string(family.prefix) + std::to_string(channel);
}

/// The value of "<dir>/<prefix><channel>_<suffix>" in the kind's unit, or nullopt when it is missing
/// or not a number.
[[nodiscard]] inline std::optional<double>
readScaled(const std::filesystem::path& dir, const InputFamily& family, unsigned channel, std::string_view suffix)
{
    const std::string name = std::string(family.prefix) + std::to_string(channel) + std::string(suffix);
    const std::optional<std::int64_t> raw = parseSysfsInteger(LinuxOsInfo::readFile(dir / name));
    if (!raw.has_value())
    {
        return std::nullopt;
    }
    return static_cast<double>(*raw) / family.divisor;
}

/// One hwmon chip's readings, ordered by kind then channel. A channel whose input can't be read (a
/// disconnected fan header reads as an error on some chips) is left out. With @p sources, each
/// reading's input is appended to it, in the same order.
[[nodiscard]] inline std::vector<SensorReading> readHwmonReadings(const std::filesystem::path& dir,
                                                                  std::vector<SensorSource>* sources = nullptr)
{
    // (kind, channel) -> reading and its source, so the order is stable and a chip that offers both
    // power1_input and power1_average reports the channel once.
    std::map<std::pair<SensorKind, unsigned>, std::pair<SensorReading, SensorSource>> byChannel;
    std::vector<std::string> fileNames;
    std::error_code ec;
    std::filesystem::directory_iterator entries(dir, ec);
    for (; !ec && entries != std::filesystem::directory_iterator{}; entries.increment(ec))
    {
        fileNames.push_back(entries->path().filename().string());
    }
    // Family by family, in INPUT_FAMILIES' order, whatever order the directory lists the files in: so
    // power*_input wins over power*_average for the same channel.
    for (const InputFamily& family : INPUT_FAMILIES)
    {
        for (const std::string& fileName : fileNames)
        {
            const std::optional<unsigned> channel = inputChannel(fileName, family);
            if (!channel.has_value() || byChannel.contains({family.kind, *channel}))
            {
                continue;
            }
            const std::optional<double> value = readScaled(dir, family, *channel, family.inputSuffix);
            if (!value.has_value())
            {
                continue;
            }
            const std::string labelFile = std::string(family.prefix) + std::to_string(*channel) + "_label";
            std::string label = LinuxOsInfo::readLine(dir / labelFile);
            if (label.empty())
            {
                label = defaultLabel(family, *channel);
            }
            const bool temperature = family.kind == SensorKind::Temperature;
            byChannel.emplace(std::pair{family.kind, *channel},
                              std::pair{SensorReading{.kind = family.kind,
                                                      .label = std::move(label),
                                                      .value = *value,
                                                      .high = temperature ? readScaled(dir, family, *channel, "_max") : std::nullopt,
                                                      .critical = temperature ? readScaled(dir, family, *channel, "_crit") : std::nullopt},
                                        SensorSource{.input = dir / fileName, .divisor = family.divisor, .runtimeStatus = {}}});
        }
    }
    std::vector<SensorReading> readings;
    readings.reserve(byChannel.size());
    for (auto& [key, entry] : byChannel)
    {
        readings.push_back(std::move(entry.first));
        if (sources != nullptr)
        {
            sources->push_back(std::move(entry.second));
        }
    }
    return readings;
}

/// The thermal zones' temperatures, labelled by zone type ("x86_pkg_temp", "acpitz"), leaving out any
/// type in @p hwmonNames: those zones also register a hwmon chip of the same name, already listed.
/// With @p sources, each zone's temp file is appended to it, in the same order.
[[nodiscard]] inline std::vector<SensorReading>
readThermalZones(const std::filesystem::path& root, const std::set<std::string>& hwmonNames, std::vector<SensorSource>* sources = nullptr)
{
    // index -> reading and its temp file, sorted by index below.
    std::vector<std::pair<unsigned, std::pair<SensorReading, std::filesystem::path>>> zones;
    std::error_code ec;
    std::filesystem::directory_iterator entries(root / "sys/class/thermal", ec);
    for (; !ec && entries != std::filesystem::directory_iterator{}; entries.increment(ec))
    {
        const std::string name = entries->path().filename().string();
        constexpr std::string_view PREFIX = "thermal_zone";
        unsigned index = 0;
        const char* const first = name.data() + std::min(PREFIX.size(), name.size());
        const char* const last = name.data() + name.size();
        if (!name.starts_with(PREFIX) || std::from_chars(first, last, index, 10).ptr != last || first == last)
        {
            continue;
        }
        const std::string type = LinuxOsInfo::readLine(entries->path() / "type");
        const std::optional<std::int64_t> milli = parseSysfsInteger(LinuxOsInfo::readFile(entries->path() / "temp"));
        if (type.empty() || !milli.has_value() || hwmonNames.contains(type))
        {
            continue;
        }
        zones.emplace_back(index,
                           std::pair{SensorReading{.kind = SensorKind::Temperature,
                                                   .label = type,
                                                   .value = static_cast<double>(*milli) / 1000.0,
                                                   .high = std::nullopt,
                                                   .critical = std::nullopt},
                                     entries->path() / "temp"});
    }
    std::ranges::sort(zones, {}, [](const auto& zone) { return zone.first; });
    std::vector<SensorReading> readings;
    readings.reserve(zones.size());
    for (auto& [index, entry] : zones)
    {
        readings.push_back(std::move(entry.first));
        if (sources != nullptr)
        {
            sources->push_back({.input = std::move(entry.second), .divisor = 1000.0, .runtimeStatus = {}});
        }
    }
    return readings;
}

/// The facts under @p root into @p info. With @p sources, each reading's source is appended to it, in
/// the order of info's readings, device by device.
inline void readSensorFacts(const std::filesystem::path& root, SensorsInfo& info, std::vector<SensorSource>* sources = nullptr)
{
    info.available = true;
    std::error_code ec;
    std::filesystem::directory_iterator chips(root / "sys/class/hwmon", ec);
    info.listed = !ec;

    // Listed first and sorted by hwmon index, so devices of the same name are numbered in a stable order.
    std::vector<std::pair<unsigned, std::filesystem::path>> chipDirs;
    for (; !ec && chips != std::filesystem::directory_iterator{}; chips.increment(ec))
    {
        const std::string name = chips->path().filename().string();
        unsigned index = 0;
        const char* const first = name.data() + std::min<std::size_t>(5, name.size());
        const char* const last = name.data() + name.size();
        if (name.starts_with("hwmon") && first != last && std::from_chars(first, last, index, 10).ptr == last)
        {
            chipDirs.emplace_back(index, chips->path());
        }
    }
    std::ranges::sort(chipDirs, {}, &std::pair<unsigned, std::filesystem::path>::first);

    std::set<std::string> hwmonNames;
    std::map<std::string, unsigned> seen;
    for (const auto& [index, dir] : chipDirs)
    {
        std::string name = LinuxOsInfo::readLine(dir / "name");
        if (name.empty())
        {
            name = "hwmon" + std::to_string(index);
        }
        hwmonNames.insert(name);
        const std::size_t firstSource = (sources != nullptr) ? sources->size() : 0;
        std::vector<SensorReading> readings = readHwmonReadings(dir, sources);
        if (readings.empty())
        {
            continue; // a chip with nothing to read (an AC adapter's online flag only)
        }
        if (sources != nullptr)
        {
            // Read per tick, so a chip whose device sleeps (a discrete GPU) is checked first (LinuxSensorProbe).
            std::error_code statusError;
            if (const std::filesystem::path status = dir / "device/power/runtime_status"; std::filesystem::exists(status, statusError))
            {
                for (SensorSource& source : std::span(*sources).subspan(firstSource))
                {
                    source.runtimeStatus = status;
                }
            }
        }
        const unsigned count = ++seen[name];
        info.devices.push_back({.name = (count == 1) ? name : name + " #" + std::to_string(count), .readings = std::move(readings)});
    }

    std::vector<SensorReading> zones = readThermalZones(root, hwmonNames, sources);
    if (!zones.empty())
    {
        info.listed = true;
        info.devices.push_back({.name = "Thermal zones", .readings = std::move(zones)});
    }
}

} // namespace Platform::LinuxSensors
