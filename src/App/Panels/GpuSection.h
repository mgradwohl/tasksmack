#pragma once

#include "Domain/GPUModel.h"
#include "Domain/GPUSnapshot.h"
#include "Platform/GPUTypes.h"
#include "UI/FillPlotLayout.h"
#include "UI/Format.h"
#include "UI/RateAxis.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace App::GpuSection
{

/// Smoothed GPU values for a single GPU device.
/// Stored per GPU ID to handle multiple GPUs.
struct SmoothedGPU
{
    double utilizationPercent = 0.0;
    double memoryPercent = 0.0;
    double temperatureC = 0.0;
    double powerWatts = 0.0;
    double clockMHz = 0.0;
    double encoderPercent = 0.0;
    double decoderPercent = 0.0;
    double fanPercent = 0.0;
    // Whether each value holds a reading. Utilization, temperature, power, clock, fan, encoder and
    // decoder are left alone on a sample that could not read them (their bars show N/A) and restart
    // from the next reading, rather than easing toward 0 (#1111, #1477). Clock and fan were drawn raw
    // before, so they stepped while the bars beside them glided (#1012).
    bool utilizationInitialized = false;
    bool memoryInitialized = false;
    bool temperatureInitialized = false;
    bool powerInitialized = false;
    bool clockInitialized = false;
    bool fanInitialized = false;
    bool encoderInitialized = false;
    bool decoderInitialized = false;
    bool initialized = false;
};

/// Why the GPU tab has nothing to chart, if it does not.
enum class EmptyReason : std::uint8_t
{
    None, ///< There is data; render the tab.
    /// GPU data could not be read: either nothing has been published at all (the probe is missing
    /// or its read failed), or something was published but enumerating the devices failed, so its
    /// empty device list means "could not look" rather than "found none".
    Unavailable,
    NoDevices,  ///< Enumeration succeeded and found no GPU.
    NoReadings, ///< GPUs are known, but the latest read returned no counters for any of them.
};

/// Classifies the GPU tab's empty state from what the model published (#927).
///
/// The reasons need different words, and each of the obvious shortcuts says something false:
///
///   - A null publication is not a wait. GPUModel publishes on every successful refresh, including
///     one that finds zero GPUs, and the panel takes its first refresh before it ever renders -- so
///     it means the probe is missing or its read failed.
///   - An empty snapshot list does not mean there is no GPU. The device list is published
///     separately, so known devices with no counters is a failed or momentarily empty read.
///   - An empty device list does not mean there is no GPU either, unless enumeration succeeded.
///     GPUModel catches a failed enumeration and carries on with an empty list, and later reads
///     still publish; "no GPU detected" would then be a guess presented as a finding.
///
/// @param hasPublication  The model has published at least once.
/// @param devicesKnown    Device enumeration succeeded (GPUPublication::gpuInfoKnown).
/// @param deviceCount     Devices enumerated.
/// @param snapshotCount   Devices the latest read returned counters for.
[[nodiscard]] constexpr EmptyReason
classifyEmptyState(bool hasPublication, bool devicesKnown, std::size_t deviceCount, std::size_t snapshotCount) noexcept
{
    if (!hasPublication)
    {
        return EmptyReason::Unavailable;
    }
    if (snapshotCount > 0)
    {
        return EmptyReason::None;
    }
    if (deviceCount > 0)
    {
        return EmptyReason::NoReadings;
    }
    return devicesKnown ? EmptyReason::NoDevices : EmptyReason::Unavailable;
}

/// The capabilities that apply to one GPU. GPUCapabilities describes the probe as a whole -- on a
/// hybrid Windows laptop NVML's sensor capabilities are set because the NVIDIA GPU has them -- so
/// each sensor series is kept only if this adapter reports it (GPUInfo::sensorCapabilities), rather
/// than drawn as a line stuck at 0 (#1040). Without per-adapter sensors the probe's apply as is.
/// Utilization and per-process metrics are not per adapter and are left alone.
[[nodiscard]] constexpr Platform::GPUCapabilities
capabilitiesForGpu(Platform::GPUCapabilities caps, const std::optional<Platform::GPUCapabilities>& adapterSensors) noexcept
{
    if (adapterSensors.has_value())
    {
        caps.hasTemperature = caps.hasTemperature && adapterSensors->hasTemperature;
        caps.hasPowerMetrics = caps.hasPowerMetrics && adapterSensors->hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds && adapterSensors->hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed && adapterSensors->hasFanSpeed;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder && adapterSensors->hasEncoderDecoder;
    }
    return caps;
}

/// One GPU the tab draws: its enumeration entry and its latest snapshot, either of which may be
/// missing. gpuId keys the GPU's ImGui IDs and points into the publication it was built from.
struct GpuDrawEntry
{
    std::string_view gpuId;
    const Platform::GPUInfo* info = nullptr;       ///< Null: the read returned a GPU enumeration did not list.
    const Domain::GPUSnapshot* snapshot = nullptr; ///< Null: no reading for this GPU this sample.
};

/// Which of a publication's adapters a tab draws (#1799): the GPU tab's GPUs, or the NPU tab's NPUs and
/// other compute accelerators. The two tabs share this section's code.
enum class DeviceSet : std::uint8_t
{
    Gpus,         ///< GPUPublication::gpuInfo / snapshots
    Accelerators, ///< GPUPublication::acceleratorInfo / acceleratorSnapshots
};

/// The devices a tab draws, in order: every enumerated device in enumeration order, whether or not the
/// latest read returned it, then any snapshot for a device enumeration did not list (all of them, if
/// enumeration failed), in the published order. A device missing from one read keeps its slot and its
/// UI state instead of vanishing and shifting the devices after it, whose state used to be keyed by
/// position (#1163).
///
/// Written into @p entries, replacing what it held but keeping its capacity, so the tab can rebuild the
/// list every frame without allocating (#1171). The entries point into @p infos and @p snapshots.
inline void
deviceDrawList(std::span<const Platform::GPUInfo> infos, std::span<const Domain::GPUSnapshot> snapshots, std::vector<GpuDrawEntry>& entries)
{
    entries.clear();
    entries.reserve(infos.size() + snapshots.size());
    const auto listed = [&entries](std::string_view gpuId)
    {
        return std::ranges::any_of(entries, [gpuId](const GpuDrawEntry& entry) { return entry.gpuId == gpuId; });
    };

    std::size_t infoIndex = 0;
    while (infoIndex < infos.size())
    {
        const Platform::GPUInfo& info = infos[infoIndex];
        ++infoIndex;
        if (listed(info.id))
        {
            continue;
        }
        const auto snapshotIt = std::ranges::find(snapshots, info.id, &Domain::GPUSnapshot::gpuId);
        entries.push_back({
            .gpuId = info.id,
            .info = &info,
            .snapshot = (snapshotIt != snapshots.end()) ? &*snapshotIt : nullptr,
        });
    }
    std::size_t snapshotIndex = 0;
    while (snapshotIndex < snapshots.size())
    {
        const Domain::GPUSnapshot& snapshot = snapshots[snapshotIndex];
        ++snapshotIndex;
        if (!listed(snapshot.gpuId))
        {
            entries.push_back({.gpuId = snapshot.gpuId, .info = nullptr, .snapshot = &snapshot});
        }
    }
}

/// The GPU tab's draw list (deviceDrawList()): the publication's GPUs.
inline void gpuDrawList(const Domain::GPUPublication& publication, std::vector<GpuDrawEntry>& entries)
{
    deviceDrawList(publication.gpuInfo, publication.snapshots, entries);
}

/// gpuDrawList() into a new vector.
[[nodiscard]] inline std::vector<GpuDrawEntry> gpuDrawList(const Domain::GPUPublication& publication)
{
    std::vector<GpuDrawEntry> entries;
    gpuDrawList(publication, entries);
    return entries;
}

/// The NPU tab's draw list (deviceDrawList()): the publication's NPUs and compute accelerators (#1799).
inline void acceleratorDrawList(const Domain::GPUPublication& publication, std::vector<GpuDrawEntry>& entries)
{
    deviceDrawList(publication.acceleratorInfo, publication.acceleratorSnapshots, entries);
}

/// acceleratorDrawList() into a new vector.
[[nodiscard]] inline std::vector<GpuDrawEntry> acceleratorDrawList(const Domain::GPUPublication& publication)
{
    std::vector<GpuDrawEntry> entries;
    acceleratorDrawList(publication, entries);
    return entries;
}

/// Whether the machine view shows its NPU tab (#1799): only once @p publication lists an NPU or other
/// compute accelerator, enumerated or read. Never before the first publication.
[[nodiscard]] inline bool showAcceleratorTab(const Domain::GPUPublication* publication) noexcept
{
    return publication != nullptr && (!publication->acceleratorInfo.empty() || !publication->acceleratorSnapshots.empty());
}

/// What a device is called where its kind is named (#1799): "NPU", "Compute accelerator" or "GPU".
[[nodiscard]] constexpr std::string_view deviceKindLabel(Platform::DeviceKind kind) noexcept
{
    switch (kind)
    {
    case Platform::DeviceKind::Npu:
        return "NPU";
    case Platform::DeviceKind::ComputeAccelerator:
        return "Compute accelerator";
    case Platform::DeviceKind::Gpu:
        break;
    }
    return "GPU";
}

/// An NPU's or compute accelerator's collapsing-header label (#1799): its name and kind, and
/// "Sleeping" as gpuHeaderLabel() says it. The same "###gpuHeader" id, so its collapse state survives
/// a missed read and a label change.
[[nodiscard]] inline std::string
acceleratorHeaderLabel(std::string_view icon, std::string_view name, Platform::DeviceKind kind, bool suspended)
{
    std::string label = std::format("{} {} [{}]", icon, name, deviceKindLabel(kind));
    if (suspended)
    {
        label += " (Sleeping)";
    }
    label += "###gpuHeader";
    return label;
}

/// The line under an NPU's or compute accelerator's header (#1799): its kind, vendor, driver version
/// and which memory its figures count -- "Kind: NPU    Vendor: Intel    Driver: 32.0.100.4778    Memory:
/// Shared". A vendor the probe could not name is left out; a driver version it could not read says so.
/// With @p memoryInUseBytes (a device that reports memory in use but no total, #1827), the memory it
/// has in use follows: "Memory: Shared, 12.0 MiB in use".
[[nodiscard]] inline std::string acceleratorDetailsText(const Platform::GPUInfo& info,
                                                        std::optional<std::uint64_t> memoryInUseBytes = std::nullopt)
{
    constexpr std::string_view SEPARATOR = "    ";
    std::string text = std::format("Kind: {}", deviceKindLabel(info.kind));
    if (!info.vendor.empty() && info.vendor != "Unknown")
    {
        text += std::format("{}Vendor: {}", SEPARATOR, info.vendor);
    }
    const std::string_view driver = info.driverVersion.empty() ? std::string_view{"Unknown"} : std::string_view{info.driverVersion};
    text += std::format("{}Driver: {}", SEPARATOR, driver);
    text += std::format("{}Memory: {}", SEPARATOR, info.memoryIsShared ? "Shared" : "Dedicated");
    if (memoryInUseBytes.has_value())
    {
        text += std::format(", {} in use", UI::Format::formatBytes(static_cast<double>(*memoryInUseBytes)));
    }
    return text;
}

/// The Overview header's "VRAM" figure: memory totals summed over discrete GPUs only. An integrated
/// GPU's total is the share of system RAM it may borrow (on Windows DXGI's SharedSystemMemory, about
/// half of RAM), so adding it double-counted RAM as VRAM on almost every laptop (#1114).
[[nodiscard]] inline std::uint64_t totalDedicatedVramBytes(std::span<const Domain::GPUSnapshot> snapshots) noexcept
{
    std::uint64_t total = 0;
    for (const auto& snapshot : snapshots)
    {
        if (!snapshot.isIntegrated)
        {
            total += snapshot.memoryTotalBytes;
        }
    }
    return total;
}

/// The GPU's collapsing-header label: name, a discrete GPU's VRAM size, its kind, and "Sleeping"
/// while the probe is leaving a runtime-suspended GPU alone (#1117). The "###" suffix keeps the
/// header's ImGui id stable while the label changes, so it doesn't re-expand as the GPU wakes.
[[nodiscard]] inline std::string
gpuHeaderLabel(std::string_view icon, std::string_view name, bool isIntegrated, std::uint64_t memoryTotalBytes, bool suspended)
{
    std::string label = std::string(icon) + " " + std::string(name);
    if (!isIntegrated && memoryTotalBytes > 0)
    {
        label += ", " + UI::Format::formatBytes(static_cast<double>(memoryTotalBytes)) + " VRAM";
    }
    label += isIntegrated ? " [Shared Memory]" : " [Discrete]";
    if (suspended)
    {
        label += " (Sleeping)";
    }
    label += "###gpuHeader";
    return label;
}

/// Lowest reference the clock line and bar are scaled against, so an idle GPU's few hundred MHz
/// don't fill the chart.
inline constexpr float GPU_CLOCK_REFERENCE_FLOOR_MHZ = 2000.0F;

/// The clock the GPU clock line and bar are drawn as a percentage of: the highest clock in the
/// history (no probe reports the device's maximum), floored at GPU_CLOCK_REFERENCE_FLOOR_MHZ. The
/// scale moves only when a new peak arrives or the old one ages out, not with every sample, and no
/// sample in the history exceeds 100 % (#994).
[[nodiscard]] inline float gpuClockReferenceMHz(std::span<const float> clockHistory, std::uint32_t currentClockMHz) noexcept
{
    float reference = std::max(GPU_CLOCK_REFERENCE_FLOOR_MHZ, static_cast<float>(currentClockMHz));
    for (const float clock : clockHistory)
    {
        if (std::isfinite(clock))
        {
            reference = std::max(reference, clock);
        }
    }
    return reference;
}

/// gpuClockReferenceMHz() from the window's largest clock sample, @p visibleClockMaxMHz: the
/// maxOfSeriesSince() of the overload below, which the tab keeps between frames
/// (UI::Widgets::windowSeriesMax(), #843).
[[nodiscard]] inline float gpuClockReferenceMHz(double visibleClockMaxMHz, std::uint32_t currentClockMHz, double shownClockMHz) noexcept
{
    // The result is one of the float samples, the floor or current clock, or a whole MHz, so narrowing
    // it back to float is exact.
    return static_cast<float>(UI::Widgets::withCurrentValues(
        visibleClockMaxMHz, {static_cast<double>(gpuClockReferenceMHz({}, currentClockMHz)), std::ceil(shownClockMHz)}));
}

/// gpuClockReferenceMHz() over only the clock samples the chart's window shows: those at x >= @p xMin
/// on @p timeAxis, to whose tail @p clockHistory is aligned (UI::Widgets::maxOfSeriesSince()).
///
/// The history holds the trim anchor just left of the window (#1016), and may hold samples older than
/// the window. They are not drawn, so they may not set the 100 % mark: a boost spike that had
/// just scrolled out kept the idle clock line drawn low against it (#1324), as #1145 fixed for the rate
/// axes. The current clock still counts, and so does @p shownClockMHz, the smoothed clock the NowBar
/// shows (UI::Widgets::currentIfAvailable(): NaN when the bar shows N/A, which is ignored). Easing down
/// from a peak that has just left the window, the bar's value can stay above every visible sample and
/// the current clock; without it the tooltip read over 100 % and the bar clamped full, as
/// UI::Widgets::withCurrentValues() prevents on the other windowed axes (#1003). It is folded in rounded
/// up to a whole MHz, which a float holds exactly, so the narrowed reference is never below it.
[[nodiscard]] inline float gpuClockReferenceMHz(std::span<const double> timeAxis,
                                                double xMin,
                                                std::span<const float> clockHistory,
                                                std::uint32_t currentClockMHz,
                                                double shownClockMHz) noexcept
{
    return gpuClockReferenceMHz(UI::Widgets::maxOfSeriesSince(timeAxis, xMin, clockHistory), currentClockMHz, shownClockMHz);
}

/// Step the clock's 100 % mark is rounded up to (gpuClockScaleMHz()).
inline constexpr float GPU_CLOCK_SCALE_STEP_MHZ = 100.0F;

/// The clock the GPU clock line and bar are drawn as a percentage of, and that their label names
/// (#1205): gpuClockReferenceMHz() rounded up to a whole GPU_CLOCK_SCALE_STEP_MHZ, so the label reads
/// "% of 2100 MHz" rather than "% of 2087 MHz" and changes only when the peak crosses a step, not with
/// every MHz the smoothed clock eases by. Never below the reference, so no sample exceeds 100 %. A
/// non-finite or non-positive reference gets the floor.
[[nodiscard]] inline float gpuClockScaleMHz(float referenceMHz) noexcept
{
    if (!std::isfinite(referenceMHz) || referenceMHz <= 0.0F)
    {
        return GPU_CLOCK_REFERENCE_FLOOR_MHZ;
    }
    return std::ceil(referenceMHz / GPU_CLOCK_SCALE_STEP_MHZ) * GPU_CLOCK_SCALE_STEP_MHZ;
}

/// The temperature the GPU temperature line and bar are drawn as a percentage of.
inline constexpr double GPU_TEMPERATURE_SCALE_C = 100.0;

/// The power limit assumed when a GPU reports none, for drawing its power as a percentage.
inline constexpr double GPU_ASSUMED_POWER_LIMIT_WATTS = 300.0;

/// The power the GPU power line and bar are drawn as a percentage of: the GPU's reported limit, or
/// GPU_ASSUMED_POWER_LIMIT_WATTS -- and saying so (#1205) -- when it reports none.
struct GpuPowerScale
{
    double watts = GPU_ASSUMED_POWER_LIMIT_WATTS;
    bool assumed = true;
    bool operator==(const GpuPowerScale&) const = default;
};

[[nodiscard]] inline GpuPowerScale gpuPowerScale(double powerLimitWatts) noexcept
{
    if (std::isfinite(powerLimitWatts) && powerLimitWatts > 0.0)
    {
        return {.watts = powerLimitWatts, .assumed = false};
    }
    return {};
}

// The labels of the GPU series drawn on a percent axis in another unit (#1205). Each names what 100 %
// is, so the axis can be read; the value beside it is the reading in its own unit. One label per series
// is shared by its value-strip entry, tooltip row and chart line (#1008), so the basis is stated
// wherever the series is named, the strip's compact "label: value" form included.

/// "Clock (% of 2100 MHz)", for gpuClockScaleMHz().
[[nodiscard]] inline std::string gpuClockSeriesLabel(float scaleMHz)
{
    return std::format("Clock (% of {})", UI::Format::formatMegahertz(static_cast<double>(scaleMHz)));
}

/// "Temperature (% of 100°C)".
[[nodiscard]] inline std::string gpuTemperatureSeriesLabel()
{
    return std::format("Temperature (% of {})", UI::Format::formatCelsius(GPU_TEMPERATURE_SCALE_C));
}

/// "Power (% of 250.0 W limit)", or "Power (% of 300.0 W, assumed)" for a GPU that reports no limit.
[[nodiscard]] inline std::string gpuPowerSeriesLabel(GpuPowerScale scale)
{
    return std::format("Power (% of {}{})", UI::Format::formatWatts(scale.watts), scale.assumed ? ", assumed" : " limit");
}

/// One GPU's labels that name a scale that can change (gpuClockSeriesLabel(), gpuPowerSeriesLabel()),
/// kept with the scale they were built for and rebuilt only when it changes: they are shown every
/// frame (#1171).
struct GpuScaledLabels
{
    std::string clock;
    float clockScaleMHz = -1.0F;
    std::string power;
    std::optional<GpuPowerScale> powerScale;

    /// Rebuilds each label whose scale differs from the one it was built for.
    void update(float newClockScaleMHz, GpuPowerScale newPowerScale)
    {
        // Any change at all must rebuild the label naming the scale, so no tolerance.
        if (clock.empty() || std::abs(clockScaleMHz - newClockScaleMHz) > 0.0F)
        {
            clock = gpuClockSeriesLabel(newClockScaleMHz);
            clockScaleMHz = newClockScaleMHz;
        }
        if (!powerScale.has_value() || *powerScale != newPowerScale)
        {
            power = gpuPowerSeriesLabel(newPowerScale);
            powerScale = newPowerScale;
        }
    }
};

/// Scale each sample to a 0–100 percentage relative to @p maxVal, into @p out (resized in place, so a
/// reused buffer allocates only when the history grows).
inline void normalizeToPercent(std::span<const float> hist, float maxVal, std::vector<float>& out)
{
    out.resize(hist.size());
    std::ranges::transform(hist, out.begin(), [maxVal](float v) { return (v / maxVal) * 100.0F; });
}

/// One GPU history drawn as a percentage of its scale (normalizeToPercent(): the clock's, temperature's
/// or power's 100 % mark, #1205), kept from frame to frame (#843). Between samples every frame draws
/// the same publication, so it is scaled again only when the history -- its data generation, address
/// or length -- or the scale changes. A generation of 0 is never kept: percentOf() then scales on
/// every call.
class PercentSeriesCache
{
  public:
    /// @p history as a percentage of @p scale, valid until the next call.
    [[nodiscard]] std::span<const float> percentOf(std::uint64_t generation, std::span<const float> history, float scale)
    {
        const Key key{
            .generation = generation, .data = history.data(), .size = history.size(), .scaleBits = std::bit_cast<std::uint32_t>(scale)};
        if (!m_Valid || generation == 0 || key != m_Key)
        {
            normalizeToPercent(history, scale, m_Values);
            m_Key = key;
            m_Valid = generation != 0;
            ++m_RebuildCount;
        }
        return m_Values;
    }

    /// How many times percentOf() has scaled the history rather than returned the kept values.
    [[nodiscard]] std::uint64_t rebuildCount() const noexcept
    {
        return m_RebuildCount;
    }

  private:
    struct Key
    {
        std::uint64_t generation = 0;
        const float* data = nullptr;
        std::size_t size = 0;
        std::uint32_t scaleBits = 0; ///< The scale's bit pattern, compared exactly

        [[nodiscard]] bool operator==(const Key&) const = default;
    };

    std::vector<float> m_Values;
    Key m_Key;
    std::uint64_t m_RebuildCount = 0;
    bool m_Valid = false;
};

/// A GPU's lines drawn as percentages of their scales, kept per draw-list entry (FrameCache).
struct GpuPercentSeries
{
    PercentSeriesCache clock;
    PercentSeriesCache temperature;
    PercentSeriesCache power;
};

/// What the GPU tab keeps from frame to frame, so that once warmed up drawing it allocates nothing
/// (#1171): its scratch buffers and draw list, reused, and the strings built from a publication --
/// header labels and chart layout IDs, one per draw-list entry -- rebuilt only when a new publication
/// arrives. Owned by the panel; one per GPU tab. UI thread only.
struct FrameCache
{
    std::vector<GpuDrawEntry> drawList;
    /// Per draw-list entry: its clock, temperature and power lines as percentages, rebuilt only for a
    /// new publication or scale (#843).
    std::vector<GpuPercentSeries> percentSeries;

    /// Per draw-list entry, built from the publication named below.
    std::vector<std::string> headerLabels;
    /// The NPU tab's line under each header (acceleratorDetailsText(), #1799); empty for a GPU, or for an
    /// accelerator enumeration did not list.
    std::vector<std::string> detailTexts;
    std::vector<std::string> coreLayoutIds;
    std::vector<std::string> thermalLayoutIds;
    /// Per draw-list entry, rebuilt when the scale a label names changes (GpuScaledLabels::update()).
    std::vector<GpuScaledLabels> scaledLabels;
    const Domain::GPUPublication* labelsPublication = nullptr;
    std::uint64_t labelsVersion = 0;
};

/// Context struct containing all state needed to render the GPU section.
/// This allows the render function to be extracted from SystemMetricsPanel
/// without requiring access to private members.
struct RenderContext
{
    // Model (non-owning pointer)
    const Domain::GPUPublication* publication = nullptr;
    // Generation of the histories charted (UI::Widgets::nextChartDataGeneration()), so the charts keep
    // their reduced points until it changes (HistoryChartConfig::dataGeneration, #1139). 0: none.
    std::uint64_t chartDataGeneration = 0;

    // History configuration
    double maxHistorySeconds = 300.0;
    float lastDeltaSeconds = 0.0F;

    // Refresh interval for smoothing alpha calculation
    std::chrono::milliseconds refreshInterval{1000};

    // Smoothed values per GPU (map keyed by GPU ID)
    std::unordered_map<std::string, SmoothedGPU>* smoothedGPUs = nullptr;

    // Shares the tab's height among every expanded GPU's charts, as the other tabs' charts do
    // (#959). Null keeps the fixed default height.
    UI::Widgets::FillPlotLayout* fill = nullptr;

    // Kept across frames by the caller so drawing allocates nothing (#1171). Null: a fresh one for the
    // frame, which draws the same but rebuilds everything.
    FrameCache* cache = nullptr;

    // The GPU tab's GPUs, or the NPU tab's NPUs and compute accelerators (#1799). Each tab keeps its own
    // smoothed values, fill state and cache.
    DeviceSet devices = DeviceSet::Gpus;
};

/// Render the GPU section with utilization, memory, thermal, and power charts -- or, for
/// DeviceSet::Accelerators, the NPU tab: each NPU or compute accelerator's kind, vendor and driver, and
/// its utilization and memory chart (#1799). An accelerator has no clock, video engines or thermal
/// sensors, so those series and the notes on their absence are left out.
/// @param ctx Render context containing model and smoothed values
void renderGpuSection(RenderContext& ctx);

/// Update smoothed values for a single GPU.
/// @param gpuId The GPU identifier
/// @param snap Current GPU snapshot
/// @param ctx Render context (uses refreshInterval and lastDeltaSeconds)
void updateSmoothedGPU(const std::string& gpuId, const Domain::GPUSnapshot& snap, const RenderContext& ctx);

} // namespace App::GpuSection
