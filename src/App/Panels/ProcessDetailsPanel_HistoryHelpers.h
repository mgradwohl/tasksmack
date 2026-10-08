#pragma once

// How ProcessDetailsPanel takes the selected process's samples into its history, extracted so it is
// unit-testable without a live ImGui context (ProcessDetailsPanel.cpp is not linked into the tests),
// following CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include "Domain/ProcessSnapshot.h"
#include "ProcessDetailsLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace App::Detail
{

/// A per-process history value, or NaN when the probe could not read it (#1110): the chart draws a
/// gap and the tooltip N/A, where a 0 read as a measurement.
[[nodiscard]] constexpr double readingOrGap(bool available, double value) noexcept
{
    return available ? value : std::numeric_limits<double>::quiet_NaN();
}

/// Whether a sample's rate is a reading: the probe can supply it on this system at all
/// (ProcessCapabilities::hasIoCounters / hasNetworkCounters) and read it for this process this sample
/// (ProcessSnapshot::ioAvailable / networkAvailable). A snapshot from a probe without the counters
/// still says "available" with a rate of 0, which would chart as a measured zero (#1210).
[[nodiscard]] constexpr bool rateIsReading(bool probeSupports, bool readThisSample) noexcept
{
    return probeSupports && readThisSample;
}

/// Which of a sample's rates are readings (rateIsReading()).
struct SampleRateReadings
{
    bool io = false;
    bool network = false;
    bool gpuPerProcess = false;  ///< The GPU probe supplied per-process GPU data for this sample
    bool gpuUtilization = false; ///< ...and per-process utilization among it
    /// The GPU probe supports per-process data at all, whether or not this sample's read succeeded:
    /// a failed read is a gap in one sample, not a lack of support (#1210).
    bool gpuSupported = false;
};

/// A sample's I/O and network rates, and its GPU figures, as readings or not, each judged by the
/// support published with that sample's own generation (ProcessSample::ioCountersSupported,
/// networkCountersSupported, gpuPerProcessSupported, gpuUtilizationSupported) -- not the latest:
/// when a batch spans a generation in which a probe withdrew or gained one, the samples before it
/// keep what they were (#1210).
[[nodiscard]] inline SampleRateReadings rateReadings(const Domain::ProcessSample& sample) noexcept
{
    if (sample.snapshot == nullptr)
    {
        return {};
    }
    // A generation whose GPU read failed has no GPU readings, but keeps the probe's support.
    // Nor does a process started since the GPU sampler's last read (#1417).
    const bool gpuRead = sample.gpuPerProcessSupported && !sample.gpuReadFailed && sample.snapshot->gpuFieldsRead;
    return {.io = rateIsReading(sample.ioCountersSupported, sample.snapshot->ioAvailable),
            .network = rateIsReading(sample.networkCountersSupported, sample.snapshot->networkAvailable),
            .gpuPerProcess = gpuRead,
            .gpuUtilization = gpuRead && sample.gpuUtilizationSupported,
            .gpuSupported = sample.gpuPerProcessSupported};
}

/// Whether a history holds any actual reading rather than only gaps (NaN).
[[nodiscard]] inline bool hasAnyReading(std::span<const double> history) noexcept
{
    return std::ranges::any_of(history, [](double value) { return !std::isnan(value); });
}

/// Whether Process Details' Network and I/O tab has anything to chart, or should show its empty
/// state (#1210). Only readings count: every sample adds a point to each history, a gap where the
/// rate was not read, so a non-empty history alone does not mean there is data.
[[nodiscard]] inline bool hasNetworkOrIoReadings(std::span<const double> ioRead,
                                                 std::span<const double> ioWrite,
                                                 std::span<const double> netSent,
                                                 std::span<const double> netReceived) noexcept
{
    return hasAnyReading(ioRead) || hasAnyReading(ioWrite) || hasAnyReading(netSent) || hasAnyReading(netReceived);
}

/// Where the pane is in the selected process's samples (ProcessModel::watchedSamplesSince(), #1098).
struct SampleIntake
{
    std::uint64_t lastVersion = 0; ///< Newest generation taken in; 0 = none since the selection
    bool present = false;          ///< Whether that generation listed the selected process
};

/// Takes @p samples (oldest first) into @p intake for the process selected as @p selectedPid /
/// @p selectedStartTicks: its exact identity, so neither a reused PID nor a colliding uniqueKey hash is
/// taken for it (#927, #1503).
///
/// Calls `record(sample, gapBefore)` once for each sample of the selected process -- one history point
/// per published generation, stamped with the generation's own sample time -- where the pane used to
/// record only the generation it happened to see on a frame, stamped with that frame's time, and
/// lose any published between two frames (#1098). @p gapBefore is true when generations between this
/// one and the previous one taken in are no longer available (more arrived unread than the model
/// keeps), so the chart can show a gap rather than a line drawn across them.
///
/// A sample whose version is not newer than the last one taken in is skipped; one that does not list
/// the selected process (it exited, or its PID now belongs to another process) only advances the version
/// and clears `present`.
template<typename Record>
inline void takeSamples(std::span<const Domain::ProcessSample> samples,
                        std::int32_t selectedPid,
                        std::uint64_t selectedStartTicks,
                        SampleIntake& intake,
                        Record&& record) // NOLINT(cppcoreguidelines-missing-std-forward) - called once per sample, as an lvalue
{
    for (const Domain::ProcessSample& sample : samples)
    {
        if (sample.version <= intake.lastVersion)
        {
            continue;
        }
        const bool gapBefore = (intake.lastVersion != 0) && (sample.version > intake.lastVersion + 1);
        intake.lastVersion = sample.version;

        const Domain::ProcessSnapshot* snapshot = sample.snapshot.get();
        intake.present = (snapshot != nullptr) && ProcessDetailsLayout::snapshotIsSelectedProcess(
                                                      selectedPid, selectedStartTicks, snapshot->pid, snapshot->startTimeTicks);
        if (!intake.present)
        {
            continue;
        }
        record(sample, gapBefore);
    }
}

/// Whether the selected process has exited after a batch of samples whose newest generation doesn't
/// contain it: only if it was seen before -- a snapshot already held, *or* a sample of it accepted
/// earlier in this same batch. Otherwise it simply hasn't been seen yet (just selected).
[[nodiscard]] constexpr bool exitedAfterBatch(bool presentInNewest, bool hadSnapshot, bool recordedThisBatch) noexcept
{
    return !presentInNewest && (hadSnapshot || recordedThisBatch);
}

} // namespace App::Detail
