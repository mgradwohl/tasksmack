#pragma once

// The headless PGO UI training driver (#880): TaskSmack's real panels, on its real (or synthetic,
// #1413) probes, rendered by ImGui/ImPlot for a fixed, weighted number of frames with no window and
// no GPU, then shut down cleanly so an instrumented build writes its profile.
//
// It is a second composition root beside main(): it builds the panels as ShellLayer does (TrainingShell
// in the .cpp mirrors ShellLayer's attach, update and tab rendering, without the window chrome), on a
// headless Core::Application that only carries events and the clock. ImGui gets no input at all, so
// no button is ever pressed: no process, service, startup or priority action can be taken.
//
// What it isolates, for determinism and so it never touches the user's TaskSmack:
//   - TASKSMACK_CONFIG_DIR is pointed at a fresh temporary directory, removed at exit, so the run
//     starts from the default settings and never reads or writes the user's config.toml;
//   - TASKSMACK_SYNTHETIC is set from --synthetic (synthetic probes) or cleared (real probes).

#include "Training/UiTrainingPlan.h"

namespace Training
{

/// Runs @p options' plan (allocateFrames(options.frames, WORKLOADS)) and returns the process exit
/// code: 0 when every frame rendered and the panels detached.
[[nodiscard]] int run(const Options& options);

} // namespace Training
