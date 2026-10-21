# Developer Guide

This page is a short map for contributors. The detail lives in two files in the repository, so it's kept
next to the code it describes:

- **[`CONTRIBUTING.md`](https://github.com/mgradwohl/tasksmack/blob/main/CONTRIBUTING.md):** prerequisites,
  automated setup, CMake presets, tests, formatting, static analysis, benchmarks, profiling, packaging, CI, and
  the pull-request process.
- **[`tasksmack.md`](https://github.com/mgradwohl/tasksmack/blob/main/tasksmack.md):** the architecture: the
  Platform, Domain, Core, UI and App layers, dependency rules, sampling threads, capabilities, platform
  backends, configuration and the current engineering direction.

## Set up and build

### Linux

```bash
./tools/setup-dev.sh
source .venv/bin/activate
cmake --workflow --preset dev
```

### Windows

```powershell
# In PowerShell 7; a clean Windows first needs: winget install Git.Git, winget install Microsoft.PowerShell
pwsh tools/setup-dev.ps1
# Open a new PowerShell 7 terminal (for PATH and LLVM_ROOT), then from the repository root:
.venv\Scripts\Activate.ps1
cmake --workflow --preset win-dev
```

The `dev` and `win-dev` workflows configure, build and test a debug build. If an existing environment fails to
configure, run the prerequisite checker:

```bash
./tools/check-prereqs.sh       # Linux
pwsh tools/check-prereqs.ps1   # Windows
```

List the presets with `cmake --list-presets` and the workflows with `cmake --list-presets=workflow`.
[CONTRIBUTING.md's Build section](https://github.com/mgradwohl/tasksmack/blob/main/CONTRIBUTING.md#build) has
the preset tables, sanitizers, coverage, benchmarks, profiling, PGO and packaging.

## Architecture

Layers depend in one direction: `Platform` reads raw OS counters, `Domain` turns them into rates, histories
and immutable snapshots, `Core` owns the SDL3 window and main loop, `UI` draws with ImGui and ImPlot, and
`App` holds the panels. App is the only place that creates platform probes. See
[Layer Model](https://github.com/mgradwohl/tasksmack/blob/main/tasksmack.md#layer-model) and
[Dependency Rules](https://github.com/mgradwohl/tasksmack/blob/main/tasksmack.md#dependency-rules).

## Metrics pipeline

Platform probes collect raw counters, Domain models calculate rates and bounded histories, and App panels
render cached publications. Process enumeration and the heavier system metrics run on separate
`BackgroundSampler` workers. System, Storage and GPU publish immutable, versioned snapshot-and-history
generations, and process consumers use `ProcessModel` snapshot versions, so render loops do no probe I/O,
take no model locks and copy no history.

History is stored in `Domain::HistoryBuffer<T>` rings (`src/Domain/History.h`), sized from the configured
history window by `Sampling::historyCapacityForSeconds()` so the whole window (10 seconds to 30 minutes) fits
even at the fastest 100 ms refresh. Entries older than the window are trimmed on every sample in O(1), and all
the series in one model share one timestamp ring and are trimmed together.

The full description is
[Sampling and Publication](https://github.com/mgradwohl/tasksmack/blob/main/tasksmack.md#sampling-and-publication)
in `tasksmack.md`.

## What's implemented

[Implemented Features](completed-features.md) lists what TaskSmack does today, by area and platform.
