#pragma once

// The selected process's smoothed "now bar" values in Process Details: each eases toward the latest
// sample at the shared chart smoothing rate (UI::Widgets::computeAlpha), so the bars animate rather
// than jump once per refresh. Extracted from ProcessDetailsPanel (#1179) so it is unit-testable
// without a live ImGui context (ProcessDetailsPanel.cpp is not linked into the tests), following
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.
//
// The panel owns one instance, calls update() for each sample it shows and reset() when the
// selection changes (a new PID, or the same PID reused by a different process), and reads the
// values back when drawing.

#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"
#include "ProcessDetailsPanel_ResourceHelpers.h"
#include "UI/ChartSmoothing.h"
#include "UI/Format.h"

#include <algorithm>
#include <chrono>
#include <optional>

namespace App::Detail
{

/// Percent of system RAM per byte for this process's memory figures, from its own resident size and
/// resident percent (memoryPercent = memoryBytes / totalSystemMemoryBytes * 100). 0 when either is
/// zero, so every percent derived from it is 0 rather than a division by zero.
[[nodiscard]] inline double memoryPercentPerByte(const Domain::ProcessSnapshot& snapshot)
{
    const double usedPercent = std::clamp(snapshot.memoryPercent, 0.0, 100.0);
    if (usedPercent <= 0.0 || snapshot.memoryBytes == 0)
    {
        return 0.0;
    }
    return usedPercent / Domain::Numeric::toDouble(snapshot.memoryBytes);
}

/// The smoothed NowBar values for the selected process. Percents stay within [0, 100], byte counts,
/// counts and rates at or above 0, and virtual memory at or above resident memory.
struct ProcessSmoothedUsage
{
    double cpuPercent = 0.0;
    double cpuUserPercent = 0.0;
    double cpuSystemPercent = 0.0;
    double residentBytes = 0.0;
    double virtualBytes = 0.0;
    double threadCount = 0.0;
    double handleCount = 0.0;
    double pageFaultsPerSec = 0.0;
    double ioReadBytesPerSec = 0.0;
    double ioWriteBytesPerSec = 0.0;
    double netSentBytesPerSec = 0.0;
    double netRecvBytesPerSec = 0.0;
    double powerWatts = 0.0;
    double gpuUtilPercent = 0.0;
    double gpuMemoryBytes = 0.0;
    // Whether the latest sample's generation had these from the GPU probe (#1210): one it did not
    // leaves the value where it was and shows N/A, like the I/O and network readings above.
    bool gpuUtilAvailable = false;
    bool gpuMemoryAvailable = false;
    // Whether the latest sample had these readings (#1110): an unread one leaves its value where it
    // was and shows N/A, as its line shows a gap, like the GDI count below.
    bool handleCountAvailable = false;
    bool ioAvailable = false;
    bool networkAvailable = false;
    double gdiObjectCount = 0.0;
    // Whether the latest sample had a GDI reading. A missing one leaves gdiObjectCount where it
    // was (not eased toward 0) and the NowBar shows N/A, as the line shows a gap (#1148).
    bool gdiInitialized = false;
    // Memory bars, in bytes like the Memory chart (#1195); Used is residentBytes above. Their share
    // of system RAM is shown only in the hover text, via memoryPercentPerByte.
    double memorySharedBytes = 0.0;
    double memoryPercentPerByte = 0.0; ///< Latest, not smoothed: converts bytes to a share of RAM
    bool initialized = false;          ///< Set by the first update(); cleared by reset()

    /// Back to the defaults, so the next update() starts from its sample instead of easing from the
    /// previous process's values. Called when the selection changes or the PID is reused.
    void reset() noexcept
    {
        *this = {};
    }

    /// Eases every value toward @p snapshot's, @p deltaTimeSeconds after the last update, at the
    /// smoothing rate for @p refreshInterval. The first update after construction or reset(), and
    /// any with no time passed (@p deltaTimeSeconds <= 0), takes the sample's values outright.
    /// @p readings says which of the sample's I/O, network and GPU figures are readings, by its own
    /// generation (Detail::rateReadings()); the others leave their value where it was, unavailable.
    void update(const Domain::ProcessSnapshot& snapshot,
                SampleRateReadings readings,
                float deltaTimeSeconds,
                std::chrono::milliseconds refreshInterval)
    {
        using UI::Widgets::initializeOrSmooth;
        const double alpha = UI::Widgets::computeAlpha(deltaTimeSeconds, refreshInterval);

        const double targetCpu = UI::Format::clampPercent(snapshot.cpuPercent);
        const double targetResident = Domain::Numeric::toDouble(snapshot.memoryBytes);
        const double targetVirtual = Domain::Numeric::toDouble(std::max(snapshot.virtualBytes, snapshot.memoryBytes));
        const double targetCpuUser = UI::Format::clampPercent(snapshot.cpuUserPercent);
        const double targetCpuSystem = UI::Format::clampPercent(snapshot.cpuSystemPercent);
        const double targetThreads = Domain::Numeric::toDouble(snapshot.threadCount);
        const double targetFaults = std::max(0.0, snapshot.pageFaultsPerSec);
        const double targetPower = std::max(0.0, snapshot.powerWatts);
        const double targetGpuUtil = UI::Format::clampPercent(snapshot.gpuUtilPercent);
        const double targetGpuMem = Domain::Numeric::toDouble(snapshot.gpuMemoryBytes);
        const double targetMemShared = Domain::Numeric::toDouble(snapshot.sharedBytes);

        // Smooth only from a previous value, and only when time has passed; otherwise snap.
        const bool canSmooth = initialized && (deltaTimeSeconds > 0.0F);

        cpuPercent = UI::Format::clampPercent(initializeOrSmooth(cpuPercent, targetCpu, alpha, canSmooth));
        residentBytes = std::max(0.0, initializeOrSmooth(residentBytes, targetResident, alpha, canSmooth));
        virtualBytes = initializeOrSmooth(virtualBytes, targetVirtual, alpha, canSmooth);
        virtualBytes = std::max(virtualBytes, residentBytes);
        cpuUserPercent = UI::Format::clampPercent(initializeOrSmooth(cpuUserPercent, targetCpuUser, alpha, canSmooth));
        cpuSystemPercent = UI::Format::clampPercent(initializeOrSmooth(cpuSystemPercent, targetCpuSystem, alpha, canSmooth));
        threadCount = std::max(0.0, initializeOrSmooth(threadCount, targetThreads, alpha, canSmooth));
        pageFaultsPerSec = std::max(0.0, initializeOrSmooth(pageFaultsPerSec, targetFaults, alpha, canSmooth));
        // Handle/FD count, I/O and network rates the probe could not read (#1110) aren't smoothed toward 0:
        // their NowBars show N/A, as their lines show a gap, and the next reading starts afresh.
        const auto smoothReading = [alpha, canSmooth](double& value, bool wasAvailable, bool available, double reading)
        {
            const auto next = smoothOptionalReading(
                {.value = value, .available = wasAvailable}, available ? std::optional<double>(reading) : std::nullopt, alpha, canSmooth);
            value = next.value;
        };
        smoothReading(handleCount, handleCountAvailable, snapshot.handleCountAvailable, Domain::Numeric::toDouble(snapshot.handleCount));
        handleCountAvailable = snapshot.handleCountAvailable;
        // A rate the probe could not supply at all when the shown sample was taken is not a reading
        // either (#1210); judged with that sample's own generation, as its history point was.
        smoothReading(ioReadBytesPerSec, ioAvailable, readings.io, snapshot.ioReadBytesPerSec);
        smoothReading(ioWriteBytesPerSec, ioAvailable, readings.io, snapshot.ioWriteBytesPerSec);
        ioAvailable = readings.io;
        smoothReading(netSentBytesPerSec, networkAvailable, readings.network, snapshot.netSentBytesPerSec);
        smoothReading(netRecvBytesPerSec, networkAvailable, readings.network, snapshot.netReceivedBytesPerSec);
        networkAvailable = readings.network;
        powerWatts = std::max(0.0, initializeOrSmooth(powerWatts, targetPower, alpha, canSmooth));
        // GPU utilization and memory the GPU probe did not supply for the shown sample's generation are
        // not readings either (#1210): not smoothed toward 0, so once support arrives the first real
        // reading starts afresh rather than easing up from placeholder zeros.
        smoothReading(gpuUtilPercent, gpuUtilAvailable, readings.gpuUtilization, targetGpuUtil);
        gpuUtilPercent = UI::Format::clampPercent(gpuUtilPercent);
        gpuUtilAvailable = readings.gpuUtilization;
        smoothReading(gpuMemoryBytes, gpuMemoryAvailable, readings.gpuPerProcess, targetGpuMem);
        gpuMemoryBytes = std::max(0.0, gpuMemoryBytes);
        gpuMemoryAvailable = readings.gpuPerProcess;
        // A sample with no GDI reading isn't smoothed toward 0: the NowBar shows N/A for it instead,
        // matching the gap in the line, and the next reading starts afresh (#1148).
        const auto gdi = smoothOptionalReading(
            {.value = gdiObjectCount, .available = gdiInitialized},
            snapshot.gdiObjectCount.has_value() ? std::optional<double>(Domain::Numeric::toDouble(*snapshot.gdiObjectCount)) : std::nullopt,
            alpha,
            canSmooth);
        gdiObjectCount = gdi.value;
        gdiInitialized = gdi.available;
        memorySharedBytes = std::max(0.0, initializeOrSmooth(memorySharedBytes, targetMemShared, alpha, canSmooth));
        memoryPercentPerByte = Detail::memoryPercentPerByte(snapshot);
        initialized = true;
    }
};

} // namespace App::Detail
