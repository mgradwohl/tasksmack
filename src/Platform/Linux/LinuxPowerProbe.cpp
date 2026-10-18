// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<unistd.h>)

#include "LinuxPowerProbe.h"

#include "Platform/BatteryCapacity.h"
#include "Platform/PowerTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform
{

namespace
{

/// |value| as unsigned. std::abs(INT64_MIN) is undefined behaviour; this is not (#1231 review).
[[nodiscard]] constexpr std::uint64_t magnitude(std::int64_t value) noexcept
{
    return value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
}

/// The first line of a small sysfs attribute, or empty if it can't be read.
[[nodiscard]] std::string readFirstLine(const std::string& path)
{
    std::ifstream file(path);
    std::string line;
    if (file.is_open())
    {
        std::getline(file, line);
    }
    return line;
}

/// An AC adapter type: "Mains", and USB charging sources in all their kernel spellings ("USB",
/// "USB_C", "USB_PD", "USB_PD_DRP", "USB_DCP", "USB_CDP", ...), which USB-C laptops use (#1231 review).
[[nodiscard]] bool isAdapterType(const std::string& type)
{
    return type == "Mains" || type.starts_with("USB");
}

/// A power supply that belongs to a peripheral -- a wireless mouse, keyboard, pen or headset --
/// rather than to the system (sysfs `scope` = "Device"). Its battery is not the system's, and its
/// charging state says nothing about whether the machine is on AC (#1109).
[[nodiscard]] bool isPeripheralSupply(const std::string& devicePath)
{
    return readFirstLine(devicePath + "/scope") == "Device";
}

[[nodiscard]] BatteryState parseBatteryState(const std::string& status)
{
    if (status == "Charging")
    {
        return BatteryState::Charging;
    }
    if (status == "Discharging")
    {
        return BatteryState::Discharging;
    }
    if (status == "Full")
    {
        return BatteryState::Full;
    }
    if (status == "Not charging")
    {
        // Plugged in but not charging: often a charge threshold holding it at, say, 80%. Not Full,
        // which showed "100%" (#1158); Windows reports the same situation the same way.
        return BatteryState::NotCharging;
    }
    return BatteryState::Unknown;
}

} // namespace

LinuxPowerProbe::LinuxPowerProbe(std::string powerSupplyRoot) : m_PowerSupplyRoot(std::move(powerSupplyRoot))
{
    discoverBatteries();
}

void LinuxPowerProbe::discoverBatteries()
{
    namespace Fs = std::filesystem;

    // Every system-scoped battery: the primary ones (BAT*, CMB* on ThinkPads) and the others (rarer:
    // a UPS's, a tablet dock's). A peripheral's (scope "Device": a mouse, a pen) is never the system's.
    std::vector<std::string> primaryBatteries;
    std::vector<std::string> otherBatteries;
    std::error_code ec;
    for (Fs::directory_iterator it(m_PowerSupplyRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        const auto devicePath = it->path().string();
        std::error_code typeEc;
        if (!it->is_directory(typeEc) || isPeripheralSupply(devicePath) || readFirstLine(devicePath + "/type") != "Battery")
        {
            continue;
        }
        const auto deviceName = it->path().filename().string();
        (deviceName.starts_with("BAT") || deviceName.starts_with("CMB") ? primaryBatteries : otherBatteries).push_back(devicePath);
    }

    // Every primary battery is one of the system's packs, combined on each read (#2319), in name order
    // so which pack is "first" doesn't depend on directory order. Without one, the first other
    // system-scoped battery stands in for the system's, as before.
    std::ranges::sort(primaryBatteries);
    std::ranges::sort(otherBatteries);
    std::vector<std::string> batteryPaths = std::move(primaryBatteries);
    if (batteryPaths.empty() && !otherBatteries.empty())
    {
        batteryPaths.push_back(otherBatteries.front());
    }

    // Listed again on every read, as the ACPI driver adds a pack's device only while it is in (#2380);
    // the capabilities follow when the set changes.
    if (m_Discovered && batteryPaths == m_BatteryPaths)
    {
        return;
    }
    m_Discovered = true;
    m_BatteryPaths = std::move(batteryPaths);
    m_Capabilities = PowerCapabilities{};
    // Only a pack that is in: a bay the driver lists with `present` 0 is no battery (#2408)
    m_Capabilities.hasBattery = std::ranges::any_of(m_BatteryPaths, isPackPresent);
    for (const auto& batteryPath : m_BatteryPaths)
    {
        addPackCapabilities(batteryPath);
    }
    m_Capabilities.hasTimeEstimates = false; // Linux doesn't provide time estimates directly
    spdlog::debug("LinuxPowerProbe: {} system batteries under {}", m_BatteryPaths.size(), m_PowerSupplyRoot);
}

/// What any of the packs offers is what the combined battery offers (#2319)
void LinuxPowerProbe::addPackCapabilities(const std::string& batteryPath)
{
    namespace Fs = std::filesystem;
    std::error_code ec;
    const auto has = [&](const char* name)
    {
        return Fs::exists(batteryPath + name, ec);
    };

    const bool hasChargeCapacity = has("/energy_now") || has("/charge_now");
    m_Capabilities.hasChargePercent |= has("/capacity");
    m_Capabilities.hasChargeCapacity |= hasChargeCapacity;
    m_Capabilities.hasPowerRate |= has("/power_now") || has("/current_now");
    m_Capabilities.hasVoltage |= has("/voltage_now");
    m_Capabilities.hasTechnology |= has("/technology");
    m_Capabilities.hasCycleCount |= has("/cycle_count");
    // Health is full/design capacity, from the energy_* or the charge_* pair; neither needs voltage (#2341)
    m_Capabilities.hasHealthPercent |=
        (has("/energy_full") && has("/energy_full_design")) || (has("/charge_full") && has("/charge_full_design"));
    // Design and full-charge capacity are read with the charge capacity, from energy_* or charge_* (#1523)
    m_Capabilities.hasDesignCapacity |= hasChargeCapacity && (has("/energy_full_design") || has("/charge_full_design"));
}

PowerCounters LinuxPowerProbe::read()
{
    discoverBatteries(); // a pack inserted or removed since the last read (#2380)
    std::vector<PackReading> packs;
    packs.reserve(m_BatteryPaths.size());
    for (const auto& batteryPath : m_BatteryPaths)
    {
        if (auto pack = readPack(batteryPath))
        {
            packs.push_back(std::move(*pack));
        }
    }

    // A pack can be inserted into or taken from a bay the driver keeps listing, which leaves the
    // set of paths, and so discovery, unchanged (#2408)
    m_Capabilities.hasBattery = !packs.empty();
    if (packs.empty())
    {
        PowerCounters counters;
        counters.state = BatteryState::NotPresent;
        counters.isOnAc = readMainsOnline().value_or(true); // Assume on AC if no battery and no adapter says otherwise
        return counters;
    }

    PowerCounters counters = combinePacks(packs);
    // The AC adapter's own `online` is authoritative; the battery's status is only a fallback for
    // machines that expose no adapter (#1109).
    counters.isOnAc = readMainsOnline().value_or(counters.state == BatteryState::Charging || counters.state == BatteryState::Full ||
                                                 counters.state == BatteryState::NotCharging);
    return counters;
}

PowerCapabilities LinuxPowerProbe::capabilities() const
{
    return m_Capabilities;
}

bool LinuxPowerProbe::isPackPresent(const std::string& batteryPath)
{
    return readSysfsFile(batteryPath + "/type") == "Battery" && readSysfsInt(batteryPath + "/present", 1) != 0;
}

std::optional<LinuxPowerProbe::PackReading> LinuxPowerProbe::readPack(const std::string& batteryPath)
{
    namespace Fs = std::filesystem;

    // Removed since discovery (hot-swapped out, or the driver unbound), or present but empty (#2319)
    if (!isPackPresent(batteryPath))
    {
        return std::nullopt;
    }

    PackReading pack;
    PowerCounters& counters = pack.counters;
    counters.state = parseBatteryState(readSysfsFile(batteryPath + "/status"));
    counters.chargePercent = readSysfsInt(batteryPath + "/capacity", -1);

    // Capacities: energy (µWh) when the pack gives it, else charge (µAh), in Wh with a voltage
    const auto voltageUv = readSysfsUInt64(batteryPath + "/voltage_now", 0);
    if (Fs::exists(batteryPath + "/energy_now"))
    {
        pack.unit = CapacityUnit::WattHours;
        pack.now = static_cast<double>(readSysfsUInt64(batteryPath + "/energy_now")) / 1000000.0; // µWh to Wh
        pack.full = static_cast<double>(readSysfsUInt64(batteryPath + "/energy_full")) / 1000000.0;
        pack.design = static_cast<double>(readSysfsUInt64(batteryPath + "/energy_full_design")) / 1000000.0;
    }
    else if (Fs::exists(batteryPath + "/charge_now"))
    {
        pack.now = static_cast<double>(readSysfsUInt64(batteryPath + "/charge_now"));
        pack.full = static_cast<double>(readSysfsUInt64(batteryPath + "/charge_full"));
        pack.design = static_cast<double>(readSysfsUInt64(batteryPath + "/charge_full_design"));
        pack.unit = CapacityUnit::MicroAmpHours;
        if (voltageUv > 0)
        {
            // µAh * µV = pWh, then to Wh
            const double volts = static_cast<double>(voltageUv) / 1000000.0;
            pack.unit = CapacityUnit::WattHours;
            pack.now = pack.now * volts / 1000000.0;
            pack.full = pack.full * volts / 1000000.0;
            pack.design = pack.design * volts / 1000000.0;
        }
    }
    if (pack.unit == CapacityUnit::WattHours)
    {
        counters.chargeNowWh = pack.now;
        counters.chargeFullWh = pack.full;
        counters.chargeDesignWh = pack.design;
    }

    // Health is the raw full/design pair, so a charge-based pack needs no voltage (#2341)
    const bool energyPair = Fs::exists(batteryPath + "/energy_full") && Fs::exists(batteryPath + "/energy_full_design");
    if (energyPair || (Fs::exists(batteryPath + "/charge_full") && Fs::exists(batteryPath + "/charge_full_design")))
    {
        pack.healthFull = static_cast<double>(readSysfsUInt64(batteryPath + (energyPair ? "/energy_full" : "/charge_full")));
        pack.healthDesign =
            static_cast<double>(readSysfsUInt64(batteryPath + (energyPair ? "/energy_full_design" : "/charge_full_design")));
    }

    // Power: power_now (µW), else current_now (µA) times voltage. Signed: the sysfs ABI lets some
    // drivers report discharge as negative (#1158), so the sign comes from the battery state instead.
    if (Fs::exists(batteryPath + "/power_now"))
    {
        counters.powerNowW = static_cast<double>(magnitude(readSysfsInt64(batteryPath + "/power_now"))) / 1000000.0; // µW to W
    }
    else if (Fs::exists(batteryPath + "/current_now") && voltageUv > 0)
    {
        // µA * µV = pW, then to W; doubles so the product can't overflow
        counters.powerNowW =
            (static_cast<double>(magnitude(readSysfsInt64(batteryPath + "/current_now"))) * static_cast<double>(voltageUv)) /
            1000000000000.0;
    }
    if (counters.state == BatteryState::Charging)
    {
        counters.powerNowW = -counters.powerNowW; // Power going in
    }

    counters.voltageNowMv = voltageUv / 1000; // µV to mV
    counters.technology = readSysfsFile(batteryPath + "/technology");
    counters.model = readSysfsFile(batteryPath + "/model_name");
    counters.manufacturer = readSysfsFile(batteryPath + "/manufacturer");
    counters.cycleCount = readSysfsUInt64(batteryPath + "/cycle_count");
    return pack;
}

/// The packs as the system's one battery (#2319):
/// - State: Charging or Discharging as the packs are; when some charge and others discharge, by the
///   sign of the summed power (#2383). Otherwise Full if every pack whose state is known is, else Not
///   charging if any is held, else Unknown.
/// - Charge: the energy-weighted percentage, sum(now) / sum(full), when every pack gives its
///   capacities in the same unit and every pack reports a full capacity (#2409); one pack's own
///   `capacity`; otherwise unavailable rather than an unweighted average or one inflated by a pack
///   whose charge counts but whose capacity doesn't. The charge-now Wh and the time estimates follow
///   the same rule.
/// - Design, full charge and health: combinePackCapacities() (Platform/BatteryCapacity.h), the rule
///   Windows uses too (#2409), under the same unit rule: the sums over the packs that report both,
///   and health from those sums, rounded and capped at 100. The Wh figures need Wh; health also
///   comes from charge without a voltage (#2341), and a lone pack whose capacities read in neither
///   unit falls back to its raw full/design pair.
/// - Power: the sum of the packs' signed rates.
/// - Cycle count: the most-cycled pack's. Voltage, technology, model and manufacturer: the first
///   pack's (by name), as a combined value would mean nothing.
PowerCounters LinuxPowerProbe::combinePacks(const std::vector<PackReading>& packs)
{
    PowerCounters counters;
    const PackReading& first = packs.front();
    counters.voltageNowMv = first.counters.voltageNowMv;
    counters.technology = first.counters.technology;
    counters.model = first.counters.model;
    counters.manufacturer = first.counters.manufacturer;

    double netPower = 0.0;
    for (const PackReading& pack : packs)
    {
        netPower += pack.counters.powerNowW;
    }
    const auto any = [&](BatteryState state)
    {
        return std::ranges::any_of(packs, [state](const PackReading& p) { return p.counters.state == state; });
    };
    const bool charging = any(BatteryState::Charging);
    const bool discharging = any(BatteryState::Discharging);
    if (charging && discharging)
    {
        // One pack tops another up (#2383): the system's state is where the energy goes on net
        counters.state = netPower > 0.0 ? BatteryState::Discharging : BatteryState::Charging;
    }
    else if (charging)
    {
        counters.state = BatteryState::Charging;
    }
    else if (discharging)
    {
        counters.state = BatteryState::Discharging;
    }
    else
    {
        // An idle pack some drivers report as Unknown doesn't hide the others' Full or Not charging
        // (#2383); only when every pack is Unknown is the system's state unknown.
        const auto known = [](const PackReading& p)
        {
            return p.counters.state != BatteryState::Unknown;
        };
        if (std::ranges::any_of(packs, known) &&
            std::ranges::all_of(packs, [&known](const PackReading& p) { return !known(p) || p.counters.state == BatteryState::Full; }))
        {
            counters.state = BatteryState::Full;
        }
        else if (any(BatteryState::NotCharging))
        {
            counters.state = BatteryState::NotCharging;
        }
    }

    const CapacityUnit unit = first.unit;
    const bool sharedUnit =
        unit != CapacityUnit::None && std::ranges::all_of(packs, [unit](const PackReading& p) { return p.unit == unit; });
    // The charge figures count a pack's charge only with its capacity (#2409)
    const bool chargeCounted =
        sharedUnit && (packs.size() == 1 || std::ranges::all_of(packs, [](const PackReading& p) { return p.full > 0.0; }));
    double now = 0.0;
    double full = 0.0;
    std::vector<PackCapacity> capacities;
    capacities.reserve(packs.size());
    for (const PackReading& pack : packs)
    {
        now += pack.now;
        full += pack.full;
        capacities.push_back(PackCapacity{.fullCharge = pack.full, .design = pack.design});
        counters.powerNowW += pack.counters.powerNowW;
        counters.cycleCount = std::max(counters.cycleCount, pack.counters.cycleCount);
    }

    if (packs.size() == 1)
    {
        counters.chargePercent = first.counters.chargePercent;
    }
    else if (chargeCounted && full > 0.0)
    {
        counters.chargePercent = static_cast<int>(std::lround(std::clamp(now / full, 0.0, 1.0) * 100.0));
    }

    if (sharedUnit)
    {
        const CombinedCapacity combined = combinePackCapacities(capacities);
        counters.healthPercent = combined.healthPercent;
        if (unit == CapacityUnit::WattHours)
        {
            counters.chargeFullWh = combined.fullCharge;
            counters.chargeDesignWh = combined.design;
        }
    }
    else if (packs.size() == 1)
    {
        counters.healthPercent = batteryHealthPercent(first.healthFull, first.healthDesign);
    }

    const bool wattHours = chargeCounted && unit == CapacityUnit::WattHours;
    if (wattHours)
    {
        counters.chargeNowWh = now;
    }

    // Time remaining from the combined figures (rough)
    if (wattHours && counters.powerNowW > 0.1 && now > 0.0)
    {
        counters.timeToEmptySec = static_cast<std::uint64_t>((now / counters.powerNowW) * 3600.0);
    }
    else if (wattHours && counters.powerNowW < -0.1 && full > now)
    {
        // Every pack's full capacity, as the charge counts every pack: not the capacity rows' figure
        const auto remaining = full - now;
        counters.timeToFullSec = static_cast<std::uint64_t>((remaining / -counters.powerNowW) * 3600.0);
    }
    return counters;
}

std::string LinuxPowerProbe::readSysfsFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return "";
    }

    std::string value;
    std::getline(file, value);

    // Trim whitespace efficiently
    if (!value.empty())
    {
        // Trim trailing whitespace
        const auto lastNonWs = value.find_last_not_of(" \n\r\t");
        if (lastNonWs == std::string::npos)
        {
            return ""; // String contains only whitespace
        }
        value.resize(lastNonWs + 1);

        // Trim leading spaces
        const auto firstNonSpace = value.find_first_not_of(' ');
        if (firstNonSpace != std::string::npos && firstNonSpace > 0)
        {
            value.erase(0, firstNonSpace);
        }
    }

    return value;
}

std::uint64_t LinuxPowerProbe::readSysfsUInt64(const std::string& path, std::uint64_t fallback)
{
    const auto str = readSysfsFile(path);
    if (str.empty())
    {
        return fallback;
    }

    std::uint64_t value = 0;
    const auto result = std::from_chars(str.data(), str.data() + str.size(), value);
    if (result.ec != std::errc{})
    {
        return fallback;
    }

    return value;
}

std::int64_t LinuxPowerProbe::readSysfsInt64(const std::string& path, std::int64_t fallback)
{
    const auto str = readSysfsFile(path);
    std::int64_t value = 0;
    const auto result = std::from_chars(str.data(), str.data() + str.size(), value);
    if (str.empty() || result.ec != std::errc{})
    {
        return fallback;
    }
    return value;
}

std::optional<bool> LinuxPowerProbe::readMainsOnline() const
{
    // Listed afresh on every read, not cached at construction: a USB-C/PD charger's power-supply
    // device appears only when it is plugged in, possibly long after the probe was created (#1231
    // review). /sys/class/power_supply holds a handful of entries, so this is cheap.
    namespace Fs = std::filesystem;
    std::optional<bool> online;
    std::error_code ec;
    for (Fs::directory_iterator it(m_PowerSupplyRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string devicePath = it->path().string();
        if (isPeripheralSupply(devicePath) || !isAdapterType(readFirstLine(devicePath + "/type")))
        {
            continue;
        }
        const int value = readSysfsInt(devicePath + "/online", -1);
        if (value == 1)
        {
            return true;
        }
        if (value == 0)
        {
            online = false;
        }
    }
    return online;
}

int LinuxPowerProbe::readSysfsInt(const std::string& path, int fallback)
{
    const auto str = readSysfsFile(path);
    if (str.empty())
    {
        return fallback;
    }

    int value = 0;
    const auto result = std::from_chars(str.data(), str.data() + str.size(), value);
    if (result.ec != std::errc{})
    {
        return fallback;
    }

    return value;
}

} // namespace Platform

#endif // defined(__linux__) && __has_include(<unistd.h>)
