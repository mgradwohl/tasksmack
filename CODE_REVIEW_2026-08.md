# TaskSmack Full Code Review — 2026-08-31

Senior-C++23-engineer-level review of the entire repository: architecture, correctness, concurrency,
performance, cross-platform behavior, build system, and tests. Conducted via five parallel deep-dive
passes (Domain, Platform/Linux, Platform/Windows, App/UI/Core, Build/CI/Tests), each required to cite
exact file:line evidence rather than speculate, plus direct verification of every P0/P1 candidate before
filing.

**Headline result: no P0s were found anywhere in the repository.** Two new P1s were found (both now
fixed-or-tracked as individual issues) on top of two P1s already tracked from a prior architecture pass.
The codebase is materially better-disciplined than average at this scale — Rule-of-5 compliance, RAII,
Domain-layer purity, and the sampling/publication concurrency design were all independently re-verified
as genuinely correct, not just documented.

## Individual issues filed from this pass

| # | Severity | Title |
|---|----------|-------|
| [#706](https://github.com/mgradwohl/tasksmack/issues/706) | P1 | WindowsDiskProbe ignores PDH `CStatus`, can silently report garbage/zero disk rates |
| [#707](https://github.com/mgradwohl/tasksmack/issues/707) | P1 | No exception boundary around per-frame layer dispatch — a throwing panel crashes the app |
| [#708](https://github.com/mgradwohl/tasksmack/issues/708) | P2/P3 (batched) | Domain: counter-rollback and publication-consistency gaps |
| [#709](https://github.com/mgradwohl/tasksmack/issues/709) | P2/P3 (batched) | Platform/Linux: concurrency and lifetime hardening |
| [#710](https://github.com/mgradwohl/tasksmack/issues/710) | P2/P3 (batched) | Platform/Windows: buffer/bounds hardening and cleanup |
| [#711](https://github.com/mgradwohl/tasksmack/issues/711) | P2/P3 (batched) | App/UI: dead code, magic strings, and redundant copies |
| [#712](https://github.com/mgradwohl/tasksmack/issues/712) | P2/P3 (batched) | Build system: packaging, dependency, and sanitizer hygiene |

Carried over from a prior architecture-focused pass, still open: [#705](https://github.com/mgradwohl/tasksmack/issues/705)
(the `ShellLayer::findSnapshot` full-vector-copy bug, the `TitleBarLayer` raw-OpenGL boundary violation,
the `BackgroundSampler` destruction-order hazard, and the App/Panels test-coverage gap). This review does
not repeat those findings; see #705 for detail.

---

## Architecture Review

The layered model (`Platform → Domain → Core/UI → App`, App as composition root) is not just documented —
it is enforced in the actual code. Every agent independently re-verified this from scratch rather than
trusting `tasksmack.md`:

- **Domain-layer purity is real.** Every include in `Domain/*.h`/`*.cpp` was traced and references only
  `Platform/I*.h` interfaces and `Platform/*Types.h` — never a concrete Linux/Windows header, Core, or UI
  header, across all 19 files.
- **Composition-root discipline holds.** All `Platform::make*Probe()`/`makeProcessActions()` call sites are
  confined to constructors/`onAttach()`; none were found in `onUpdate`/`render` across any panel.
  `Platform/Factory.h` inclusion is confined to Panels + `Core::PathService`.
- **No circular headers or layering leaks** were found across `App → UI → Core → Domain → Platform`.

Where the architecture is weaker is not in the boundary rules themselves but in a handful of **local
API-design and ownership issues** inside the App/UI layer:

- `ProcessDetailsPanel.cpp` (2264 lines, the largest file in the repo) has three individual functions in
  the 270–320 line range (`renderActions`, `renderGpuUsage`, `renderResourceUsage`) that mix unrelated
  concerns (dialog UI, action dispatch, layout) in one body — a maintainability risk independent of its
  already-tracked lack of tests (#711).
- A magic-string (`std::string m_ConfirmAction`) is used for action dispatch where an `enum class` would
  give compile-time exhaustiveness checking (#711).
- Two small instances of dead public API surface exist (`ProcessesPanel::snapshots()`, three unused
  `*Layer::instance()` singleton accessors that duplicate an already-working event-based path) — harmless
  but worth removing before they accumulate more callers by accident (#711).
- `ShellLayer`'s tab-management code requires touching 7 separate locations to add a new tab — not yet a
  God-class problem at 3 panels, but worth collapsing to a registration list before a 4th/5th tab is added
  (#711).

None of this rises above P2/P3 — the architecture's bones are sound; these are refinements, not repairs.

## Performance Review

**No accidental algorithmic blow-ups (O(n²) or worse) were found** in any sampling or render hot path.
The project's own "cache render snapshots, rebuild only on version change" discipline is real and applied
in the hottest path (`ProcessesPanel::renderProcessRow`), just not applied with full consistency everywhere:

Ranked by actual measured/inferable impact:

1. **Two full-vector copies per frame while a process is selected** (`ShellLayer.cpp` → `findSnapshot()` →
   `ProcessModel::snapshots()`, already tracked as #705's headline finding, *plus* a second redundant copy
   layered on top of it at `ShellLayer.cpp:156-160`, newly found in this pass and added to #711). Together
   these are the single most concrete, fixable performance finding in the whole review — a two-line fix
   (search the cached vector instead of the live one, and `std::move` instead of copy-assign) removes both.
2. **`SystemModel::snapshot()`'s torn-read window** (#708) is a correctness issue more than a performance
   one, but the fix (single locked section) has zero performance cost, so there's no tradeoff to weigh.
3. **Netlink `recv()` with no timeout** (#709) is a *tail-latency* risk, not a steady-state performance
   issue — it doesn't slow every frame, but it can turn one bad kernel interaction into a permanently
   stalled sampler thread, which reads to the user as "the app froze."
4. Everything else performance-adjacent found in this pass (fixed 256-entry NVML buffers, a 256-`wchar_t`
   registry buffer, PDH `CStatus` handling) affects **correctness under load/edge conditions**, not
   steady-state speed, and is covered under Concurrency/Correctness above.

Nothing found here warrants a micro-optimization pass — the codebase already reserves capacity, caches
formatted strings at data-refresh cadence rather than render cadence, and avoids `std::function`/virtual
dispatch overhead in the render-row hot path.

## Concurrency Review

This is the strongest area of the codebase. The core design — expensive probe I/O outside any lock, a
brief exclusive lock only for the final struct-swap-and-version-bump, and an atomic-version fast path so
the render thread can skip locking/copying entirely when nothing changed — was independently re-verified
correct in `ProcessModel`, `SystemModel`, `StorageModel`, and `GPUModel`. `BackgroundSampler`'s
`stop_token`/`condition_variable_any` usage is textbook: predicate-based waits correctly handle spurious
wakeups, `stop_requested()` is checked at every relevant point, and no samplable is invoked after
cancellation is observed. No ImGui state is ever touched from a background thread — the sampler API is
strictly sample-only, with no completion callback into App/UI.

The gaps found are narrow and none are P0/P1:

- **`SystemModel::computeCpuUsage()`** is the one place in the whole Domain layer that skips the
  counter-rollback guard used everywhere else, meaning a single regressed counter field silently pins
  displayed CPU% at 100% instead of being caught the way every other rate calculation in the file catches
  it (#708).
- **`SystemModel::snapshot()`** has a genuine torn-read window across two separate lock acquisitions
  within one `refresh()` call — not reached by the current UI (which uses the version-gated publication
  path instead), but a live gap in the public API contract that the next caller or test will hit (#708).
- **`publish()` in three models** advances a private version counter before doing allocation-heavy work
  that could throw `std::bad_alloc`, which can desync the version counter from what's actually published
  — no torn *data* is ever visible (the `shared_ptr` swap is atomic), just a silently skipped version
  number (#708).
- **Two Linux-specific races**: an unbounded blocking `recv()` on the Netlink socket with no timeout,
  reachable from the sampler thread, and a `setSocketStatsCacheTtl()` API that reassigns a `unique_ptr`
  with no synchronization while another thread dereferences it — currently safe only because the sole
  caller invokes it once before the sampler starts (#709).

No deadlocks, no lock-ordering violations across multiple mutexes, and no missing `noexcept` on
move/swap/destructor paths were found anywhere in the codebase.

## C++23 Modernization Opportunities

Deliberately short — the codebase already makes strong, consistent use of `std::jthread`/`std::stop_token`,
`std::span`, `std::string_view`, `std::optional`, `constexpr`, `enum class` with explicit underlying types,
designated initializers, `[[nodiscard]]`, structured bindings, and `std::ranges`/`std::views` throughout.
Only two opportunities surfaced that would provide tangible (not cosmetic) value:

1. **`ProcessDetailsPanel`'s string-keyed action dispatch → `enum class ProcessAction`** (#711) — this is
   a real safety/clarity win (compiler-checked exhaustiveness, no typo risk), not modernization for its
   own sake.
2. **`Numeric::counterDelta`/`counterRate` adoption in `SystemModel::computeCpuUsage()`** (#708) — the
   codebase already has the right helper; this is a consistency fix, not new API surface.

No case was found where `std::expected`, concepts/`requires`, or newer ranges adaptors would materially
improve an existing interface over what's there today — introducing them speculatively was deliberately
not recommended per this review's scope.

## Build / Tooling Recommendations

The build system is unusually mature for this project's stage: modern target-based CMake throughout (no
directory-scoped legacy commands found anywhere), every third-party dependency except `stb`/`glad` pinned
to an immutable commit SHA with a tag-naming comment, well-justified (not bug-masking) sanitizer
suppressions, and a CI gate design that correctly treats the advisory ASan/UBSan PR check as
advisory-on-PR/blocking-on-main by design rather than by oversight.

Concrete, worth-doing gaps, all filed in #712:

- **`stb` is the one FetchContent dependency missing `SYSTEM`**, contradicting the project's own stated
  convention and currently papered over with manual pragma guards in `IconLoader.cpp` — a future stb
  version bump introducing a new warning class will break the `-Werror` build the way `SYSTEM` would have
  silently prevented.
- **The `msan` preset has no CI coverage** and requires an MSan-instrumented libc++ the repo doesn't build
  — it's likely bit-rotted; either wire it into CI for real or clearly label it "not CI-verified."
- **The Debian package declares a `libsdl3-0` runtime dependency that shouldn't exist** (SDL3 is statically
  linked) and may not even be installable on some target systems — a packaging correctness bug, not just
  hygiene.
- A dead `<variant>` header in the precompiled-header list adds compile-time cost to all 54 translation
  units for zero benefit (`grep` confirms zero usage in `src/`).

None of these block a release; all are cheap, well-scoped fixes.

## Testing Gaps

The existing suite is stronger than a first pass would assume — it already has explicit, well-named tests
for exactly the scenarios this kind of review usually has to ask for: PID reuse (`NewProcessWithSamePidGetsZeroCpu`,
`UniqueKeyDiffersForPidReuse`), counter overflow/wraparound (`IntegerOverflowInCpuCounters`,
`IoRatesHandleCounterWrapAround`), `BackgroundSampler` interval-clamping at 0ms/1ms, concurrent interval
changes, concurrent `addSamplable`, and samplables that throw (`std::exception`-derived *and* non-derived,
e.g. `throw 42`) continuing to run afterward. This significantly narrows what's actually missing:

Specific, concrete additions worth making (not "add more tests" in general):

1. **A regression test for `SystemModel::computeCpuUsage()`'s missing rollback guard** — feed it a
   counter set where one field regresses while the total doesn't, and assert the result is clamped/flagged
   sanely rather than silently pinned at 100% (once #708 is fixed, this test should fail on the old code
   and pass on the new).
2. **A destruction-order regression test** for `SystemMetricsPanel`/`ProcessesPanel` (already flagged in
   #705) — construct, start sampling, destroy the panel under TSan, and assert no use-after-free is
   reported, since the current safety is manual-discipline-only rather than type-enforced.
3. **A concurrent `setSocketStatsCacheTtl()` test** (#709) — call it from one thread while `enumerate()`
   runs on another under TSan, to convert the currently-latent race into either a caught regression or a
   documented single-call-site-only contract.
4. **`ProcessDetailsPanel`/`SystemMetricsPanel`/`TitleBarLayer` unit tests** — already tracked in #705 and
   the pre-existing #415/#412; this review adds no new angle beyond confirming these are still the three
   largest untested files in the repo.
5. **An exception-path test for the render loop** — once #707's per-layer try/catch lands, add a test
   layer whose `onUpdate`/`onRender` throws, and assert the application logs and continues rather than
   terminating.

## Recommended Action Plan

### 1. Fix immediately
- [#706](https://github.com/mgradwohl/tasksmack/issues/706) — WindowsDiskProbe PDH `CStatus` check (small, isolated diff; matches an existing correct pattern in the same subsystem)
- [#707](https://github.com/mgradwohl/tasksmack/issues/707) — Exception boundary around per-frame layer dispatch (small diff, meaningfully improves crash resilience for a long-running tool)
- The two-line fix in [#705](https://github.com/mgradwohl/tasksmack/issues/705)/[#711](https://github.com/mgradwohl/tasksmack/issues/711) combined: `findSnapshot()` search the cached vector + `std::move` instead of copy — cheapest real perf win in the whole review

### 2. Fix soon
- [#708](https://github.com/mgradwohl/tasksmack/issues/708) — `computeCpuUsage()` rollback guard, `SystemModel::snapshot()` torn read, `publish()` version-ordering in three models
- [#709](https://github.com/mgradwohl/tasksmack/issues/709) — Netlink `recv()` timeout, `setSocketStatsCacheTtl()` synchronization
- [#710](https://github.com/mgradwohl/tasksmack/issues/710) — per-core CPU query growth loop, `imageName` bounds validation
- [#712](https://github.com/mgradwohl/tasksmack/issues/712)'s `stb` `SYSTEM` keyword and the `libsdl3-0` packaging fix

### 3. Refactor when convenient
- `ProcessDetailsPanel`'s oversized functions and magic-string action dispatch (#711)
- `DXGIGPUProbe`'s manual COM `Release()` → `ComPtr` (#710)
- Dead public API removal (#711), `ShellLayer` tab-registration cleanup (#711)
- `NVMLGPUProbe`'s comment/code mismatch on the 256-process buffer, `FdGuard` deduplication (#709/#710)

### 4. Optional improvements
- `msan` preset CI wiring or explicit "not CI-verified" labeling (#712)
- Dead `<variant>` PCH header removal, `Core::Layer` copy/move deletion, TSan suppression re-audit (#711/#712)
- Parsing-style consistency across Linux GPU/disk/power probes (#709)

---

*This report and the issues it links were produced by a full-repository automated review pass. Every
finding above was independently verified against the actual source before being written up — nothing here
is speculative or inferred from documentation alone.*
