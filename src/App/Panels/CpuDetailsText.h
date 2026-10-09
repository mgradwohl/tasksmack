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
};

/// Everything the rows are built from. The live values (utilization, kernel time, speed, uptime)
/// come from the system snapshot; the process, thread and handle totals from the process model's
/// system histories, which the Overview's Resources chart already draws.
struct Inputs
{
    const Domain::SystemSnapshot* snapshot = nullptr;
    bool hasCpuFreq = false;            ///< Platform::SystemCapabilities::hasCpuFreq
    bool hasUptime = false;             ///< Platform::SystemCapabilities::hasUptime
    bool hasVirtualizationInfo = false; ///< Platform::SystemCapabilities::hasVirtualizationInfo
    std::optional<std::size_t> processCount;
    std::optional<double> threadCount;
    std::optional<double> handleCount; ///< Handles (Windows) or file descriptors (elsewhere)
    bool handlesAreFileDescriptors = false;
    std::uint64_t totalVramBytes = 0; ///< Dedicated VRAM across discrete GPUs; 0 = none
};

/// "3.70 GHz", as the CPU Cores header writes a clock.
[[nodiscard]] inline std::string formatGigahertz(std::uint64_t megahertz)
{
    return std::format("{:.2f} GHz", Domain::Numeric::toDouble(megahertz) / 1000.0);
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

    // Virtualization (Windows)
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
        flag("Virtualization-based security", cpu.vbsRunning, "Running", "Not running");
        flag("Memory integrity", cpu.hvciEnabled, "On", "Off");
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

/// The shallowest layout of the rows that fits `availableWidthPx`: the most column pairs (at most
/// MAX_COLUMN_PAIRS) whose columns -- each pair sized to its own rows' widest label and value, plus
/// `perPairExtraPx` of cell padding -- fit side by side; one pair when none does. The rows per pair
/// are then evened out, so no pair is left empty or nearly so, and the width left over is shared out
/// among the value columns, so the block spans the whole width. The fewer rows, the more height the
/// charts below keep. `labelWidthsPx` and `valueWidthsPx` hold each row's measured width (the same
/// length); pure arithmetic, so it costs nothing to run each frame.
[[nodiscard]] inline ColumnLayout columnLayout(std::span<const float> labelWidthsPx,
                                               std::span<const float> valueWidthsPx,
                                               float availableWidthPx,
                                               float perPairExtraPx) noexcept
{
    const std::size_t rowCount = std::min(labelWidthsPx.size(), valueWidthsPx.size());
    ColumnLayout layout;
    if (rowCount == 0)
    {
        return layout;
    }
    const float extra = (std::isfinite(perPairExtraPx) && perPairExtraPx > 0.0F) ? perPairExtraPx : 0.0F;
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
        const bool fits = std::isfinite(availableWidthPx) && candidate.totalWidth <= availableWidthPx;
        if (fits || tryPairs == 1)
        {
            layout = candidate;
            break;
        }
    }
    // Spread what is left of the width over the value columns, so the block uses all of it
    if (std::isfinite(availableWidthPx) && layout.totalWidth < availableWidthPx && layout.pairs > 0)
    {
        const float share = (availableWidthPx - layout.totalWidth) / static_cast<float>(layout.pairs);
        for (std::size_t pair = 0; pair < layout.pairs; ++pair)
        {
            layout.valueWidths[pair] += share;
        }
        layout.totalWidth = availableWidthPx;
    }
    return layout;
}

} // namespace App::CpuDetailsText
