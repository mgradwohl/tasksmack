#pragma once

// The Overview's CPU Details block (#809): its label/value rows and how many label/value column
// pairs they are laid out in. Pure (no ImGui calls), so both are unit-tested; CpuDetailsBlock draws.

#include "Domain/Numeric.h"
#include "Domain/SystemSnapshot.h"
#include "Platform/CpuDetails.h"
#include "UI/Format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App::CpuDetailsText
{

/// The text of a value that is not known: an em dash, muted, with a tooltip saying why -- the
/// convention #1210 set for the process table -- never a "-" or "0" that reads like a reading.
inline constexpr std::string_view UNAVAILABLE_TEXT = "\xE2\x80\x94";

/// One label/value row.
struct Row
{
    std::string_view label;
    std::string value;     ///< UNAVAILABLE_TEXT when !available.
    bool available = true; ///< False: drawn muted, with `tooltip` saying why.
    std::string tooltip;   ///< Hover text; empty for none.

    bool operator==(const Row&) const = default;
};

/// Everything the rows are built from. The live values (utilization, kernel time, speed, uptime)
/// come from the system snapshot; the process, thread and handle totals from the process model's
/// system histories, which the Overview's Resources chart already draws.
struct Inputs
{
    const Domain::SystemSnapshot* snapshot = nullptr;
    bool hasCpuFreq = false;                    ///< Platform::SystemCapabilities::hasCpuFreq
    bool hasUptime = false;                     ///< Platform::SystemCapabilities::hasUptime
    bool hasLoadAvg = false;                    ///< Platform::SystemCapabilities::hasLoadAvg (Linux)
    bool hasPressure = false;                   ///< Platform::SystemCapabilities::hasPressure (Linux PSI)
    bool hasVirtualizationInfo = false;         ///< Platform::SystemCapabilities::hasVirtualizationInfo
    bool hasVirtualizationSecurityInfo = false; ///< Platform::SystemCapabilities::hasVirtualizationSecurityInfo
    std::optional<std::size_t> processCount;
    std::optional<double> threadCount;
    std::optional<double> handleCount; ///< Handles (Windows) or file descriptors (elsewhere)
    bool handlesAreFileDescriptors = false;
    std::uint64_t totalVramBytes = 0; ///< Dedicated VRAM across discrete GPUs; 0 = none
};

/// "3.70 GHz", as the CPU Cores header writes a clock.
[[nodiscard]] inline std::string formatGigahertz(std::uint64_t megahertz)
{
    return UI::Format::formatFixedLocalized(Domain::Numeric::toDouble(megahertz) / 1000.0, 2, " GHz");
}

/// "24", or "24 (8 P + 16 E)" on a hybrid CPU.
[[nodiscard]] inline std::string formatCores(const Platform::CpuDetails& details)
{
    const std::size_t cores = details.physicalCores.value_or(0);
    if (details.performanceCores.has_value() && details.efficiencyCores.has_value())
    {
        return std::format("{} ({} P + {} E)",
                           UI::Format::formatIntLocalized(cores),
                           UI::Format::formatIntLocalized(*details.performanceCores),
                           UI::Format::formatIntLocalized(*details.efficiencyCores));
    }
    return UI::Format::formatIntLocalized(cores);
}

/// What the Virtualization row says: a running hypervisor means it is in use, whatever the firmware
/// flag reads (Windows does not report the firmware setting reliably from inside a hypervisor's root
/// partition); otherwise the firmware's setting. nullopt when neither is known.
[[nodiscard]] inline std::optional<std::string_view> virtualizationText(const Platform::CpuDetails& details) noexcept
{
    if (details.hypervisorPresent.value_or(false))
    {
        return "Enabled";
    }
    if (details.virtualizationFirmwareEnabled.has_value())
    {
        return *details.virtualizationFirmwareEnabled ? std::string_view{"Enabled"} : std::string_view{"Disabled"};
    }
    return std::nullopt;
}

/// "1.2%": a pressure average as the Pressure row writes it.
[[nodiscard]] inline std::string formatPressurePercent(double percent)
{
    return UI::Format::formatFixedLocalized(percent, 1, "%");
}

/// The Pressure row's hover text (#3062): what the figures are, then each resource's some and full
/// averages over 10 s, 60 s and 5 min.
[[nodiscard]] inline std::string pressureTooltip(const Domain::SystemSnapshot& snap)
{
    std::string text = "Share of time tasks were stalled waiting for each resource (Linux pressure stall information).\n"
                       "some: at least one task waited. full: every running task waited at once.\n"
                       "Averages over 10 s / 60 s / 5 min:";
    const auto line = [&text](std::string_view name, std::string_view kind, const std::optional<Platform::PressureStall>& stall)
    {
        if (stall.has_value())
        {
            text += std::format("\n{} {}: {} / {} / {}",
                                name,
                                kind,
                                formatPressurePercent(stall->avg10),
                                formatPressurePercent(stall->avg60),
                                formatPressurePercent(stall->avg300));
        }
    };
    for (const auto& [name, pressure] : {std::pair<std::string_view, const Platform::ResourcePressure*>{"CPU", &snap.cpuPressure},
                                         std::pair<std::string_view, const Platform::ResourcePressure*>{"Memory", &snap.memoryPressure},
                                         std::pair<std::string_view, const Platform::ResourcePressure*>{"I/O", &snap.ioPressure}})
    {
        line(name, "some", pressure->some);
        line(name, "full", pressure->full);
    }
    return text;
}

/// The block's rows, in reading order: the live figures, then the CPU's static facts, then (where
/// the platform reports them) its virtualization status, then installed memory. A row the platform
/// cannot report at all is left out; a value it could not read this time is UNAVAILABLE_TEXT.
[[nodiscard]] inline std::vector<Row> buildRows(const Inputs& in)
{
    std::vector<Row> rows;
    if (in.snapshot == nullptr)
    {
        return rows;
    }
    const Domain::SystemSnapshot& snap = *in.snapshot;
    const Platform::CpuDetails& cpu = snap.cpuDetails;
    rows.reserve(24);

    const auto add = [&rows](std::string_view label, std::string value)
    {
        rows.push_back({.label = label, .value = std::move(value), .available = true, .tooltip = {}});
    };
    const auto unavailable = [&rows](std::string_view label, std::string tooltip)
    {
        rows.push_back({.label = label, .value = std::string(UNAVAILABLE_TEXT), .available = false, .tooltip = std::move(tooltip)});
    };
    const auto count = [&](std::string_view label, std::optional<std::size_t> value, const char* reason)
    {
        if (value.has_value() && *value > 0)
        {
            add(label, UI::Format::formatIntLocalized(*value));
        }
        else
        {
            unavailable(label, reason);
        }
    };
    const auto cache = [&](std::string_view label, std::optional<std::uint64_t> bytes)
    {
        if (bytes.has_value())
        {
            add(label, UI::Format::formatBytes(Domain::Numeric::toDouble(*bytes)));
        }
        else
        {
            unavailable(label, "Not reported by this system (the CPU may not have this cache level)");
        }
    };
    const auto total = [&](std::string_view label, std::optional<double> value, const char* reason)
    {
        if (value.has_value() && std::isfinite(*value) && *value >= 0.0)
        {
            add(label, UI::Format::formatIntLocalized(std::llround(*value)));
        }
        else
        {
            unavailable(label, reason);
        }
    };
    const auto flag = [&](std::string_view label, std::optional<bool> value, std::string_view yes, std::string_view no)
    {
        if (value.has_value())
        {
            add(label, std::string(*value ? yes : no));
        }
        else
        {
            unavailable(label, "This system did not report it");
        }
    };

    // Live figures
    add("Utilization", UI::Format::formatPercent(snap.cpuTotal.totalPercent));
    add("Kernel time", UI::Format::formatPercent(snap.cpuTotal.systemPercent));
    if (in.hasCpuFreq)
    {
        if (snap.cpuFreqMHz > 0)
        {
            add("Speed", formatGigahertz(snap.cpuFreqMHz));
        }
        else
        {
            unavailable("Speed", "The current clock could not be read");
        }
    }
    constexpr const char* NO_PROCESS_DATA = "No process data yet";
    count("Processes", in.processCount, NO_PROCESS_DATA);
    total("Threads", in.threadCount, NO_PROCESS_DATA);
    total(in.handlesAreFileDescriptors ? std::string_view{"FDs"} : std::string_view{"Handles"},
          in.handleCount,
          in.handlesAreFileDescriptors ? "Open file descriptors are not readable here" : "Handle counts are not readable here");
    if (in.hasUptime)
    {
        // The duration alone: the label already says what it is ("Up time  1d 02h", not "Up: 1d 02h")
        if (snap.uptimeSeconds > 0)
        {
            add("Up time", UI::Format::formatDuration(Domain::Numeric::toDouble(snap.uptimeSeconds)));
        }
        else
        {
            unavailable("Up time", "Not read yet");
        }
    }

    // The 1, 5 and 15-minute load averages, where the platform has them (Linux, #3107). The row had
    // gone missing in January although the probe kept reading them and the docs kept describing it.
    if (in.hasLoadAvg)
    {
        rows.push_back({.label = "Load average",
                        .value = std::format("{} / {} / {}",
                                             UI::Format::formatFixedLocalized(snap.loadAvg1, 2, ""),
                                             UI::Format::formatFixedLocalized(snap.loadAvg5, 2, ""),
                                             UI::Format::formatFixedLocalized(snap.loadAvg15, 2, "")),
                        .available = true,
                        .tooltip = "Runnable and uninterruptible tasks, averaged over 1, 5 and 15 minutes"});
    }

    // Pressure stall information (Linux, #3062): how much of the last 10 s tasks spent waiting for CPU,
    // memory and I/O, which utilization alone doesn't show. The other averages and "full" on hover.
    if (in.hasPressure)
    {
        const auto some10 = [](const Platform::ResourcePressure& pressure)
        {
            return pressure.some.has_value() ? formatPressurePercent(pressure.some->avg10) : std::string("-");
        };
        if (snap.cpuPressure.read() || snap.memoryPressure.read() || snap.ioPressure.read())
        {
            rows.push_back({.label = "Pressure",
                            .value = std::format("CPU {} \xC2\xB7 memory {} \xC2\xB7 I/O {}",
                                                 some10(snap.cpuPressure),
                                                 some10(snap.memoryPressure),
                                                 some10(snap.ioPressure)),
                            .available = true,
                            .tooltip = pressureTooltip(snap)});
        }
        else
        {
            unavailable("Pressure", "The pressure files could not be read this time");
        }
    }

    // The CPU's static facts
    if (cpu.baseSpeedMHz.has_value() && *cpu.baseSpeedMHz > 0)
    {
        add("Base speed", formatGigahertz(*cpu.baseSpeedMHz));
    }
    else
    {
        unavailable("Base speed", "The rated clock is not reported by this system");
    }
    count("Sockets", cpu.sockets, "The physical package count is not reported by this system");
    if (cpu.physicalCores.has_value() && *cpu.physicalCores > 0)
    {
        add("Cores", formatCores(cpu));
    }
    else
    {
        unavailable("Cores", "The physical core count is not reported by this system");
    }
    // The probe's own count of the processors it samples, where the topology read gave none.
    std::optional<std::size_t> logical = cpu.logicalProcessors;
    if (!logical.has_value() && snap.coreCount > 0)
    {
        logical = static_cast<std::size_t>(snap.coreCount);
    }
    count("Logical processors", logical, "The logical processor count is not reported by this system");
    cache("L1 cache", cpu.l1CacheBytes);
    cache("L2 cache", cpu.l2CacheBytes);
    cache("L3 cache", cpu.l3CacheBytes);

    // Virtualization (Windows, and Linux since #2592)
    if (in.hasVirtualizationInfo)
    {
        if (const auto text = virtualizationText(cpu); text.has_value())
        {
            std::string tooltip;
            if (cpu.slatSupported.has_value())
            {
                tooltip = *cpu.slatSupported ? "Second-level address translation (SLAT): supported"
                                             : "Second-level address translation (SLAT): not supported";
            }
            rows.push_back({.label = "Virtualization", .value = std::string(*text), .available = true, .tooltip = std::move(tooltip)});
        }
        else
        {
            unavailable("Virtualization", "This system did not report it");
        }
        flag("Hypervisor", cpu.hypervisorPresent, "Detected", "Not detected");
        // Windows only: Linux has neither (#809), so the rows are left out there, not shown as unknown
        if (in.hasVirtualizationSecurityInfo)
        {
            flag("Virtualization-based security", cpu.vbsRunning, "Running", "Not running");
            flag("Memory integrity", cpu.hvciEnabled, "On", "Off");
        }
        // Linux only (#2592): only a platform that reports it sets it
        if (cpu.kvmAvailable.has_value())
        {
            add("KVM", *cpu.kvmAvailable ? "Available" : "Not available");
        }
    }

    // Installed memory: what the Overview's header line used to show beside the CPU model
    add("Memory", UI::Format::formatBytes(Domain::Numeric::toDouble(snap.memoryTotalBytes)));
    if (in.totalVramBytes > 0)
    {
        add("GPU memory", UI::Format::formatBytes(Domain::Numeric::toDouble(in.totalVramBytes)));
    }
    return rows;
}

/// The one-line summary the block's heading shows while it is collapsed (#809): the CPU model, then
/// the cores, base clock, current clock and utilization that are known, separated by middle dots,
/// e.g. "Intel(R) Core(TM) Ultra 7 255H · 16 cores (6 P + 10 E) · 2.00 GHz base · 3.71 GHz · 4.2%".
[[nodiscard]] inline std::string collapsedSummary(const Inputs& in)
{
    if (in.snapshot == nullptr)
    {
        return {};
    }
    const Domain::SystemSnapshot& snap = *in.snapshot;
    const Platform::CpuDetails& cpu = snap.cpuDetails;
    constexpr std::string_view SEPARATOR = " \xC2\xB7 ";
    std::string text = snap.cpuModel;
    const auto append = [&text, SEPARATOR](std::string_view part)
    {
        if (!text.empty())
        {
            text += SEPARATOR;
        }
        text += part;
    };
    if (cpu.physicalCores.has_value() && *cpu.physicalCores > 0)
    {
        const std::string cores = formatCores(cpu); // "16" or "16 (6 P + 10 E)"
        const auto split = cores.find(' ');
        append((split == std::string::npos) ? std::format("{} cores", cores)
                                            : std::format("{} cores{}", cores.substr(0, split), cores.substr(split)));
    }
    else if (snap.coreCount > 0)
    {
        append(std::format("{} logical processors", snap.coreCount));
    }
    if (cpu.baseSpeedMHz.has_value() && *cpu.baseSpeedMHz > 0)
    {
        append(formatGigahertz(*cpu.baseSpeedMHz) + " base");
    }
    if (in.hasCpuFreq && snap.cpuFreqMHz > 0)
    {
        append(formatGigahertz(snap.cpuFreqMHz));
    }
    append(UI::Format::formatPercent(snap.cpuTotal.totalPercent));
    return text;
}

/// The most label/value column pairs the block uses, however wide the window: enough for every row
/// in three lines on a wide window, past which a line of facts is too long to read across.
inline constexpr std::size_t MAX_COLUMN_PAIRS = 8;

/// How the rows are laid out: `pairs` label/value column pairs side by side, filled top to bottom
/// and then left to right, `rowsPerPair` rows deep, each pair's columns as wide as its own widest
/// label and value.
struct ColumnLayout
{
    std::size_t pairs = 1;
    std::size_t rowsPerPair = 0;
    std::array<float, MAX_COLUMN_PAIRS> labelWidths{}; ///< Each pair's label column, in pixels
    std::array<float, MAX_COLUMN_PAIRS> valueWidths{}; ///< Each pair's value column, in pixels
    float totalWidth = 0.0F;                           ///< Every pair's columns plus `perPairExtraPx`: the available width when it fits
};

/// What fitColumns() fits the rows to.
struct ColumnFit
{
    float availableWidthPx = 0.0F; ///< Width the pairs must fit in, side by side
    float perPairExtraPx = 0.0F;   ///< Cell padding each pair adds to its two columns
    /// Pairs the rows are laid out in now (last frame); 0 for none yet. More pairs than this are
    /// taken only with `growMarginPx` to spare, so text that changes width by a few pixels each
    /// sample (an up time, a process count) cannot flip the layout back and forth at a fixed window
    /// width (#1919). Fewer pairs are taken as soon as the current ones stop fitting.
    std::size_t currentPairs = 0;
    float growMarginPx = 0.0F;
};

/// The shallowest layout of the rows that fits `fit.availableWidthPx`: the most column pairs (at
/// most MAX_COLUMN_PAIRS) whose columns -- each pair sized to its own rows' widest label and value,
/// plus `fit.perPairExtraPx` of cell padding -- fit side by side; one pair when none does. The rows
/// per pair are then evened out, so no pair is left empty or nearly so. Every row is in the layout:
/// `pairs * rowsPerPair` is never less than the row count, so a block as tall as `rowsPerPair` rows
/// shows them all. The columns are at their measured widths; spreadColumns() widens them to a width.
/// `labelWidthsPx` and `valueWidthsPx` hold each row's measured width (the same length); pure
/// arithmetic, so it costs nothing to run each frame.
[[nodiscard]] inline ColumnLayout
fitColumns(std::span<const float> labelWidthsPx, std::span<const float> valueWidthsPx, const ColumnFit& fit) noexcept
{
    const std::size_t rowCount = std::min(labelWidthsPx.size(), valueWidthsPx.size());
    ColumnLayout layout;
    if (rowCount == 0)
    {
        return layout;
    }
    const float extra = (std::isfinite(fit.perPairExtraPx) && fit.perPairExtraPx > 0.0F) ? fit.perPairExtraPx : 0.0F;
    const float growMargin = (std::isfinite(fit.growMarginPx) && fit.growMarginPx > 0.0F) ? fit.growMarginPx : 0.0F;
    const auto widthOf = [](float px)
    {
        return (std::isfinite(px) && px > 0.0F) ? px : 0.0F;
    };
    for (std::size_t tryPairs = std::min(MAX_COLUMN_PAIRS, rowCount); tryPairs >= 1; --tryPairs)
    {
        ColumnLayout candidate;
        candidate.rowsPerPair = (rowCount + tryPairs - 1) / tryPairs;
        candidate.pairs = (rowCount + candidate.rowsPerPair - 1) / candidate.rowsPerPair;
        for (std::size_t row = 0; row < rowCount; ++row)
        {
            const std::size_t pair = row / candidate.rowsPerPair; // < pairs <= MAX_COLUMN_PAIRS
            candidate.labelWidths[pair] = std::max(candidate.labelWidths[pair], widthOf(labelWidthsPx[row]));
            candidate.valueWidths[pair] = std::max(candidate.valueWidths[pair], widthOf(valueWidthsPx[row]));
        }
        for (std::size_t pair = 0; pair < candidate.pairs; ++pair)
        {
            candidate.totalWidth += candidate.labelWidths[pair] + candidate.valueWidths[pair] + extra;
        }
        const bool grows = fit.currentPairs > 0 && candidate.pairs > fit.currentPairs;
        const float limit = fit.availableWidthPx - (grows ? growMargin : 0.0F);
        const bool fits = std::isfinite(limit) && candidate.totalWidth <= limit;
        if (fits || tryPairs == 1)
        {
            layout = candidate;
            break;
        }
    }
    return layout;
}

/// Widens `layout`'s value columns to share out what is left of `widthPx`, so the block spans the
/// whole width. A layout already as wide or wider is left as it is.
inline void spreadColumns(ColumnLayout& layout, float widthPx) noexcept
{
    if (std::isfinite(widthPx) && layout.totalWidth < widthPx && layout.pairs > 0)
    {
        const float share = (widthPx - layout.totalWidth) / static_cast<float>(layout.pairs);
        for (std::size_t pair = 0; pair < layout.pairs; ++pair)
        {
            layout.valueWidths[pair] += share;
        }
        layout.totalWidth = widthPx;
    }
}

} // namespace App::CpuDetailsText
