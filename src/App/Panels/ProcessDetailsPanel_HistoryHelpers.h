#pragma once

// How ProcessDetailsPanel takes the selected process's samples into its history, extracted so it is
// unit-testable without a live ImGui context (ProcessDetailsPanel.cpp is not linked into the tests),
// following CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include "Domain/ProcessSnapshot.h"
#include "ProcessDetailsLayout.h"

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

/// Where the pane is in the selected process's samples (ProcessModel::watchedSamplesSince(), #1098).
struct SampleIntake
{
    std::uint64_t lastVersion = 0; ///< Newest generation taken in; 0 = none since the selection
    bool present = false;          ///< Whether that generation listed the selected process
};

/// Takes @p samples (oldest first) into @p intake for the process selected as @p selectedPid /
/// @p selectedKey, adopting the key from its first sample when it was selected by PID alone.
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
                        std::uint64_t& selectedKey,
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
        intake.present = (snapshot != nullptr) &&
                         ProcessDetailsLayout::snapshotIsSelectedProcess(selectedPid, selectedKey, snapshot->pid, snapshot->uniqueKey);
        if (!intake.present)
        {
            continue;
        }
        if (selectedKey == 0)
        {
            selectedKey = snapshot->uniqueKey;
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
