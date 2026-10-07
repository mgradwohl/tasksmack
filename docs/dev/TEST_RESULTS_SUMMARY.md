# Ring Buffer History Redesign - Test Execution Summary

## Overview
Issue #583: Domain history storage unified on ring buffers. The design uses a
runtime-capacity `Domain::HistoryBuffer<T>` (in `src/Domain/History.h`) sized from the
configured history window via `Sampling::historyCapacityForSeconds()`, so the full
user-configured window (10-1800 s) is honored even at the fastest refresh cadence
(100 ms). Time-based trimming remains the retention source of truth and is O(1) per
sample via `discardFront()` (head-index advance, no copies or rebuilds).

## Test Suite Execution (win-benchmark, clang++ -O3 LTO)
- **Total Tests**: 1,106 across 68 suites
- **Passed**: 1,104
- **Skipped**: 2 (GPU hardware-dependent: NVML probe, multi-GPU LUID)
- **Failed**: 0
- **Duration**: ~24 seconds

## Design Summary
- `HistoryBuffer<T>`: single `std::vector<T>` backing store; `push` auto-evicts oldest;
  `setCapacity` preserves newest elements; `discardFront(count)` is O(1);
  `copyTo`/`toVector` emit chronological output in at most two `std::copy_n` chunks.
- Capacity = `ceil(maxHistorySeconds * 1000 / REFRESH_INTERVAL_MIN_MS) + 1`, recomputed
  in each model constructor and `setMaxHistorySeconds`.
- All series in a model push in lockstep with a shared timestamp ring; one
  `HistoryUtils::discardBefore()` call trims every aligned ring.
- Fixed-capacity `History<T, N>` retained unchanged for GPUModel.

## Behavioral Semantics (unchanged from original deque design)
- **Time-window trimming**: entries older than `now - maxHistorySeconds` are dropped on
  every sample. Verified by `SystemModelTest.NetworkHistoryTrimmedByTime` (10 s window,
  samples t=1..15 trim to t=5..15) and `StorageModelTest.MaxHistorySecondsLimitsHistory`
  (0.5 s window at ~100 ms sampling keeps <= 7 samples).
- **Zero retention**: `setMaxHistorySeconds(0)` keeps only the current sample
  (`ProcessModelTest.ZeroHistoryRetentionKeepsOnlyCurrentSample`).
- **Per-disk alignment**: newly discovered disks are backfilled with 0.0 (clamped to ring
  capacity) and absent disks receive 0.0 placeholders, so every per-disk series has the
  same length as the timestamp axis
  (`StorageModelTest.PerDiskHistoryNewDiskAppearsBackfillsPlaceholders`).
- **Publication contract**: publications still expose `std::vector` copies; UI untouched.

## New Unit Coverage
`tests/Domain/test_History.cpp` adds a `HistoryBufferTest` suite: push/evict, multiple
wraparounds, `discardFront` (clamped, zero, and on a wrapped buffer where more than
capacity items were pushed before discarding), `setCapacity` shrink/grow preserving
newest, capacity-1 minimum, chronological `copyTo` after wrap, clear/reuse,
non-trivial element types, and `HistoryUtils::discardBefore` alignment.

`tests/Domain/test_SystemModel.cpp` adds `PerCoreHistoryStaysAlignedOnCoreCountDecrease`:
establishes two core rings, simulates a sample where only one core is reported, and
verifies every retained core ring stays the same length as the timestamp axis with the
absent core receiving a 0.0F placeholder; also verifies the series resumes real values
on the next sample when both cores return.

## Benchmark Spot Checks (win-benchmark)
- `BM_History_Push` ~6.4 ns; `BM_History_CopyTo(Wrapped)` ~25 ns
- `BM_SystemModel_Refresh` ~1.73 ms; `BM_StorageModel_Sample` ~72 us
- `BM_ProcessModel_Refresh` ~9.2 ms (303 processes)
No regressions versus the deque baseline; steady-state sampling performs no per-sample
allocations in the history paths.

## Status
All tests passing; review feedback on capacity semantics, O(1) trimming, and stale
documentation addressed.
