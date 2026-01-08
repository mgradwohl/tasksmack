# Panel Refactoring Plan

## Problem Statement

`SystemMetricsPanel.cpp` is a **2,600+ line monolith** that contains all system metric tabs:
- Overview (~700 lines) - CPU, Memory, Power, Threads charts
- CPU Cores (~200 lines) - Per-core usage grid
- GPU (~450 lines) - GPU utilization, memory, temperature
- Network and I/O (~400 lines) - Interface status, throughput charts

This violates the single-responsibility principle and makes the code harder to:
- Navigate and understand
- Test in isolation
- Modify without risk of breaking unrelated features

Additionally, `NetworkPanel.cpp` exists as a **834-line orphaned file** that:
- Uses a non-existent type (`Domain::NetworkInterfaceSnapshot`)
- Is never instantiated or wired into the UI
- Contains duplicate helper functions

## Current Architecture

```
ShellLayer (owns top-level tabs)
├── ProcessesPanel (own file)
├── ProcessDetailsPanel (own file)
└── SystemMetricsPanel (MONOLITH - contains 4 internal tabs)
    ├── Overview tab (inline)
    ├── CPU Cores tab (inline)
    ├── GPU tab (inline)
    └── Network and I/O tab (inline)

NetworkPanel.cpp (ORPHANED - not wired into UI)
```

## Target Architecture

### Option A: Section Renderers (Recommended)

Keep `SystemMetricsPanel` as the model owner, extract rendering into section files:

```
SystemMetricsPanel.cpp (~400 lines)
├── Model ownership (SystemModel, StorageModel, GPUModel)
├── Lifecycle (onAttach, onDetach, onUpdate)
├── Tab orchestration
├── Smoothed state management
└── Calls section renderers

OverviewSection.cpp/h
└── renderOverviewSection(models, context)

CpuCoresSection.cpp/h
└── renderCpuCoresSection(models, context)

GpuSection.cpp/h
└── renderGpuSection(models, context)

NetworkSection.cpp/h
└── renderNetworkSection(models, context)
```

**Pros:**
- Models stay owned by SystemMetricsPanel (no ownership refactor)
- Each file is focused and testable
- Gradual extraction possible
- Lower risk

**Cons:**
- Not "true" panels (they're render functions, not Panel subclasses)
- Context struct may grow unwieldy

### Option B: Full Panel Split

Create independent panels with shared model ownership:

```
ShellLayer owns:
├── SystemModel (shared_ptr)
├── StorageModel (shared_ptr)
└── GPUModel (shared_ptr)

Panels receive model pointers:
├── OverviewPanel
├── CpuCoresPanel
├── GpuPanel
└── NetworkPanel
```

**Pros:**
- True separation of concerns
- Cleaner Panel hierarchy

**Cons:**
- Requires model ownership refactor
- Higher risk of introducing bugs
- More invasive changes to ShellLayer

## Shared Utilities

Created `src/App/Panels/NetworkUtils.h` containing:
- `cropFrontToSize()` - Crop history vector to aligned size (template)
- `isVirtualInterface()` - Detect loopback, Docker, WSL, etc.
- `isBluetoothInterface()` - Detect Bluetooth adapters
- `getInterfaceTypeIcon()` - Return FA icon for interface type
- `getSortedFilteredInterfaces()` - Sort/filter interface list

These are used by:
- `SystemMetricsPanel::renderOverview()`, `renderCpuSection()`, `renderPerCoreSection()`
- `NetworkSection::renderNetworkSection()`, `renderDiskIOSection()`
- `GpuSection::renderGpuSection()`

## Migration Plan

### Phase 0: Preparation (COMPLETE ✓)
- [x] Create `NetworkUtils.h` with shared helpers
- [x] Update `SystemMetricsPanel` to use `NetworkUtils::`
- [x] Mark `NetworkPanel.cpp/h` as deprecated
- [x] Document architecture and plan
- [x] Ensure build passes and tests pass

### Phase 1: Extract NetworkSection (COMPLETE ✓)
- [x] Create `NetworkSection.cpp/h` (629 lines)
- [x] Move `renderNetworkSection()` and `renderDiskIOSection()` to new file
- [x] Keep `SystemMetricsPanel` calling the new functions
- [x] All tests pass (507/507)
- [x] SystemMetricsPanel reduced from ~2,400 to 1,748 lines

### Phase 2: Extract GpuSection (COMPLETE ✓)
- [x] Create `GpuSection.cpp/h` (484 + 42 lines)
- [x] Move `renderGpuSection()` and `updateSmoothedGPU()` to new file
- [x] Move `cropFrontToSize()` to shared `NetworkUtils.h`
- [x] All tests pass (507/507)
- [x] SystemMetricsPanel reduced from 1,748 to 1,330 lines

### Phase 3: Extract CpuCoresSection
- Create `CpuCoresSection.cpp/h`
- Move `renderPerCoreSection()` to new file
- Update tests

### Phase 4: Extract OverviewSection
- Create `OverviewSection.cpp/h`
- Move `renderOverview()` and helper functions to new file
- Update tests

### Phase 5: Cleanup
- Delete deprecated `NetworkPanel.cpp/h`
- Verify all tests pass
- Update documentation

## Files Changed (This PR)

| File | Change |
|------|--------|
| `src/App/Panels/NetworkUtils.h` | **NEW** - Shared utilities (interface filtering, cropFrontToSize) |
| `src/App/Panels/NetworkSection.h` | **NEW** - Network/Disk section rendering context and functions |
| `src/App/Panels/NetworkSection.cpp` | **NEW** - renderNetworkSection, renderDiskIOSection (629 lines) |
| `src/App/Panels/GpuSection.h` | **NEW** - GPU section rendering context and types (42 lines) |
| `src/App/Panels/GpuSection.cpp` | **NEW** - renderGpuSection (484 lines) |
| `src/App/Panels/SystemMetricsPanel.h` | Uses GpuSection::SmoothedGPU, removed local SmoothedGPU struct |
| `src/App/Panels/SystemMetricsPanel.cpp` | Delegates to NetworkSection and GpuSection (1,330 lines) |
| `CMakeLists.txt` | Added NetworkSection.cpp and GpuSection.cpp |
| `docs/panel-refactor-plan.md` | **NEW** - This document |

## Testing Strategy

Each phase should:
1. Pass all existing tests
2. Add tests for extracted sections (if feasible)
3. Pass clang-tidy
4. Pass clang-format

## Success Criteria

- `SystemMetricsPanel.cpp` reduced to ~400 lines
- Each section in its own file (~200-700 lines each)
- No code duplication between sections
- All tests pass
- clang-tidy clean

---

## Helper Rationalization Plan

### Problem Statement

Helper utilities are scattered across multiple files with inconsistent organization:
- `UI::Numeric` (45 lines) re-exports `Domain::Numeric` plus adds UI helpers
- `HistoryWidgets.h` name is confusing (sounds like Domain::History)
- `NetworkUtils.h` contains generic `cropFrontToSize` that isn't network-specific
- No consistent place for chart-related utilities

### Current Helper Inventory

| File | Namespace | Lines | Purpose |
|------|-----------|-------|---------|
| `src/Domain/Numeric.h` | `Domain::Numeric` | 36 | Foundation type conversions (toDouble, narrowOr, clampPercentToFloat) |
| `src/UI/Format.h` | `UI::Format` | 777 | String formatting (bytes, percent, time, locale, alignment) |
| `src/UI/Widgets.h` | `UI::Widgets` | 122 | Basic ImGui drawing (bars, overlays) |
| `src/UI/HistoryWidgets.h` | `UI::Widgets` | 511 | ImPlot chart helpers (smoothing, axis formatters, time axis) |
| `src/UI/Numeric.h` | `UI::Numeric` | 45 | UI numeric conversions (percent01, toFloatNarrow, checkedCount) |
| `src/App/Panels/NetworkUtils.h` | `App::NetworkUtils` | 217 | Interface filtering + cropFrontToSize |

### Proposed Changes

| Current | Proposed | Rationale |
|---------|----------|-----------|
| `UI::HistoryWidgets.h` | `UI::ChartWidgets.h` | Name reflects actual content (ImPlot helpers) |
| `cropFrontToSize` in NetworkUtils | Move to `UI::ChartWidgets.h` | Used for chart data alignment |
| `UI::Numeric.h` | Merge into `UI::Format.h` | Consolidate UI numeric utilities |
| `App::NetworkUtils` | `App::NetInterfaceUtils` | Clearer name (network interface filtering) |
| — | Add `PlotThemeGuard` to ChartWidgets | Ensure consistent chart theming |

### Layer Responsibilities (Post-Rationalization)

```
Domain::Numeric     - Foundation type conversions (used by Domain layer)
UI::Format          - String formatting + UI numeric helpers (merged)
UI::Widgets         - Basic ImGui drawing helpers
UI::ChartWidgets    - ImPlot helpers, smoothing, cropFrontToSize
App::NetInterfaceUtils - Network interface filtering/sorting policy
```

### Implementation Steps

1. **Rename HistoryWidgets.h → ChartWidgets.h**
   - Update namespace comments
   - Update all includes

2. **Add cropFrontToSize to ChartWidgets.h**
   - Move from NetworkUtils.h
   - Update namespace to `UI::Widgets`

3. **Merge UI::Numeric into UI::Format**
   - Move `clampPercent`, `percent01`, `checkedCount`, `toFloatNarrow`
   - Remove `using` re-exports (callers use Domain::Numeric directly)
   - Delete UI/Numeric.h

4. **Rename NetworkUtils.h → NetInterfaceUtils.h**
   - Update namespace to `App::NetInterfaceUtils`
   - Remove cropFrontToSize (moved to ChartWidgets)
   - Update all includes

5. **Add PlotThemeGuard to ChartWidgets.h**
   - RAII guard for consistent chart styling
   - Push/pop ImPlot style vars

6. **Update all call sites and includes**

7. **Build and test**
