# TaskSmack Code Review: October 15, 2026 (follow-up)

**Scope:** `main` at `8ad96cc8` (2026-10-15), compared with the previous review (`docs/CODE_REVIEW_2026-10.md`, `main` at `90ca6e5`, 2026-10-03). Between the two: **569 commits**, 1,090 files changed, about 250k lines added. Of those, about 107k lines are under `src/` and about 124k are tests.

**Method:** a deep multi-agent pass, like October's, on top of a single-reviewer draft.
1. **Draft:** the WSL-lane agent built a first version from four sources:
   - static checks of current `main`: greps for layer boundaries, singletons, statics, suppressions and test hooks, plus file sizes;
   - the nightly coverage data (Linux lcov, function- and line-level);
   - the issue tracker (state of every October finding, flaky-test history);
   - what the conveyor work itself showed: about 70 coverage PRs, the Linux dump and systemd-action features, and the CI incidents.
2. **Eight area reviewers**, each reading the source of one area in full and checking the draft's claims:
   - process views and actions;
   - App: Services, Startup, System Information, settings, config;
   - Linux platform;
   - Windows platform;
   - Domain and the shared Platform interfaces;
   - Core and UI;
   - tests and CI;
   - cross-cutting architecture.
3. **An adversarial verifier** for every CRITICAL or HIGH finding: it tried to refute each one and set the final severity. It kept one HIGH and lowered the other to MEDIUM.
4. **A completeness critic:** it checked what the reviewers had not covered, reviewed the largest unreviewed merges itself (icon pipeline, strace launch, I/O priority, affinity, PSS/USS, NPU, public IP, ETW, Windows Startup actions, Vulkan, LocaleSetup, the selective clang-tidy gate), and added three findings.

Each finding carries a confidence:
- **verified:** confirmed against the source, with the lines quoted;
- **likely:** the code path is clear, but nobody ran it;
- **needs runtime:** it depends on OS or daemon behaviour that only a live test can confirm.

No code was changed and nothing was built for this document. Line numbers refer to `8ad96cc8`.

**Output:**
- this document;
- one GitHub issue per finding (#2114–#2173), labelled `code-review`, with the severity, an area, and its lane (`agent:WSL` or `agent:Windows`). Issue numbers are in the FINDINGS INDEX.

Two pairs of findings described the same defect, so each pair was merged into one issue. That makes 60 issues from 62 findings.

---

## WHERE THINGS STAND SINCE THE OCTOBER REVIEW

- **All 99 October findings are closed** (#1088–#1187, excluding #1182): 1 CRITICAL, 8 HIGH, 45 MEDIUM, 45 LOW. So are all 11 `code-review` issues filed after them.
  - Closure was checked by issue state. This pass did not re-audit each fix, but none of the eight reviewers found an October defect come back.
  - The fixes I can vouch for from the conveyor work:
    - ROCm ABI (#1088);
    - clang-tidy and UBSan gates (#1089, #1090);
    - Application tests in CI (#1132);
    - atomic config writes (#1122);
    - "unavailable" as NaN (#1110, #1111, #1146);
    - the dead-code sweep (#1185).
- **The quality gates are real now:**
  - clang-tidy fails on warnings;
  - ASan/UBSan and TSan block;
  - CodeQL runs on every PR;
  - the nightly Heavy Checks fail on a Linux line-coverage drop below 95% (#2111, merged 2026-10-15);
  - **6 required checks**, 22 workflows.
- **Coverage:** Linux 95.65% of lines (98.06% of functions, 86.81% of branches); Windows 91.54% of lines. Both are up from 54% at the low point that #1395 reported.
- **Tests:** about 6,460 `TEST`/`TEST_F` cases in 375 files (158k lines); 11 rapidcheck properties; 17 fuzz targets.
- **Size:** `src/` has grown from 202 to 546 files, about 139k lines:

| Layer | Files | Lines |
|---|---|---|
| Platform (Linux, Windows, shared, Synthetic) | 244 | 59.5k |
| App | 169 | 47.3k |
| UI | 52 | 13.8k |
| Domain | 43 | 11.0k |
| Core | 33 | 6.8k |
| Training (PGO driver, separate target) | 5 | 1.1k |

- **New since October, by area:**
  - Services tab on both platforms, with Linux systemd actions over D-Bus;
  - Startup tab;
  - System Information page (OS, firmware, memory modules, storage/SMART, graphics, security, sensors, devices, drivers, boot performance, crashes, public IP);
  - Create dump file (Windows MiniDumpWriteDump, and Linux through gdb's gcore with a free-space check and folder picker);
  - set affinity, I/O priority, syscall trace (strace);
  - NPU support;
  - per-process PSS/USS, private and swap;
  - synthetic large-UI scenarios;
  - a headless PGO training driver;
  - a large performance programme (#843, still open).

## OVERALL ASSESSMENT

The October verdict ("not production-ready until the CRITICAL and HIGH issues are fixed and the CI gates are made real") **no longer holds**.
- Every October finding is closed, the gates block, and coverage is high and now guarded.
- This pass found **0 CRITICAL, 1 HIGH, 26 MEDIUM and 33 LOW** findings, against October's 1 CRITICAL, 8 HIGH, 45 MEDIUM and 45 LOW, on a codebase almost three times the size.
- **No reviewer found a path that acts on the wrong process.**
  - Every destructive action on both platforms captures (PID, start time) when it is requested.
  - The Linux layer re-checks it through a pidfd before acting; the Windows layer re-checks it through the open handle's creation time.
  - That was the worst risk for a task manager, and it is handled consistently.

The main risk has moved from *known defects* to *how the code was produced*.

1. **Review debt (HIGH, process).** On 2026-10-08 the Copilot review quota ran out, and the maintainer chose merge on green CI. Since then **249 PRs have merged with no code review at all**: no Copilot pass and no human reviewer. That is about 44% of everything since October 3.
   - **This review confirms the risk with data:** the defects cluster in code merged after 2026-10-08.
     - The Linux systemd Services backend alone has six findings, including the only HIGH (F01, F05, F10, F11, F12, F33).
     - The gcore dump path has three (F09, F34, F35). F09 is in the conveyor's own #2107.
     - The System Information page has four (F06, F07, F22, F31).
   - These are design and behaviour mistakes that CI cannot see: a start-type mapping the tests lock in, a privacy toggle that leaves global IP addresses in the report, a guard that misses two of systemd's dependency kinds.
   - Earlier in this session, two test bugs of the same kind were caught only by luck before merge: a dangling pointer that only Windows CI's crash exposed (#2083), and a "coverage" test that passed without reaching its target.
2. **Breadth outran depth.** Feature count roughly tripled in two weeks. The newest areas lean hardest on fakes, which encode our own reading of each external interface:
   - D-Bus systemd, udisks2, NetworkManager, networkd and resolved;
   - gcore and strace;
   - DRM ioctls;
   - Windows WMI and PDH.

   The tests prove the code matches the fake, not that the fake matches the real service. F01 shows exactly this: the tests assert the wrong mapping, so they pass. See TEST GAPS.
3. **The platform you can least test is the one users will hit first.** Native Wayland, real multi-monitor DPI and stock-desktop `ptrace_scope=1` are all either untestable in CI or only manually tracked:
   - #865 and #593, still open;
   - #2088, found only because a second monitor happened to be attached;
   - F27: on the default GNOME terminal, a root TaskSmack reports success for a trace that cannot attach.
4. **Composition is the architecture debt** (F23, F24). The layering holds at the include level, but no single place decides what a run is made of. Every panel picks its own probes, and the PGO training shell copies the shell's wiring by hand and has already drifted from it.

**Verdict:** the code is in good shape for an early release, on three conditions:
1. Fix F01 and the MEDIUM Linux Services and privacy findings (F05, F06, F07, F09, F10, F11).
2. Pay down the review debt for user-facing and platform-boundary code.
3. Give the fakes for external interfaces a real-environment smoke test each.

## WHAT IS IN GOOD SHAPE

The reviewers were asked to name strengths as well as defects. These held up under a full read:

- **Process identity.**
  - Every kill, signal, priority, I/O priority, affinity, trace and dump captures `{pid, startTimeTicks}` at request time.
  - Linux opens a pidfd, checks the start time and refuses an unknown one (`checkProcessIdentity`). Where a call has no pidfd form, it confirms afterwards per thread.
  - Windows opens the process and compares `GetProcessTimes` on the same handle.
  - Confirm dialogs close themselves when their target moves.
- **Publication.**
  - `PublicationSlot` and `SharedHistoryBuffer` give immutable generations, with writers serialised and a non-throwing commit.
  - The stage/commit split in `SystemModel`, `StorageModel` and `GPUModel` gives a real strong guarantee against `bad_alloc`, and tests back it up.
- **PID reuse across samplers.** The GPU merge, the service merge, energy attribution and socket attribution all key by PID plus start time, or reject a process first seen after the other sampler's capture.
- **`UserConfig::save()`.**
  - It follows symlinks by hand and stages through `O_EXCL`.
  - It sets owner, ACL and mode on the descriptor before the rename.
  - It refuses to replace a file it can't parse, and it merges only the keys TaskSmack changed.
- **`Platform::DetachedSpawn`:**
  - `close_range`, with a `getdents64` fallback;
  - only async-signal-safe calls after `fork`;
  - signal reset;
  - an exec-failure report pipe;
  - a bounded wait.

  It is the model the other launch paths should use (F25).
- **The `/proc/<pid>/stat` parser** handles crafted `comm` names. Enumeration reads through one `openat` dirfd per process, so a PID reused mid-pass can't mix two processes' data.
- **The Windows layer:**
  - RAII for handles, COM, SetupAPI and registry;
  - defensive parsing of variable-size kernel buffers;
  - a dump path that loads `dbghelp` from System32 only and deletes partial files on cancel;
  - an open-files reader that can't hang the UI.
- **The frame loop.**
  - The idle-wake event is dispatched before the next poll.
  - Pacing lives in a pure, unit-tested `FramePacing.h`.
  - `guardLayerCall()` wraps every layer callback.
- **CI:**
  - the CI Success gate reads step outcomes, so a skipped required check can't count as a pass;
  - configure fails on any unregistered `test_*.cpp`;
  - suite discovery fails on an empty binary;
  - the clang-tidy cache keys on every included file;
  - workflows use `permissions: {}` and SHA-pinned actions.

## RISKS

Ordered by likelihood times impact.

1. **No review on 249 merged PRs (HIGH).** See above. The cheapest mitigation:
   - a targeted human or ultrareview pass over the platform-boundary and user-facing changes since 2026-10-08, rather than all of them. This review is a first instalment of that.
   - restore some review for anything touching `src/Platform/**`, settings, or destructive actions.
2. **Linux Services shows wrong state, and its safeguards are Windows-only (HIGH).**
   - F01: "Manual" writes `disabled`, which reads back as "Disabled".
     - Every installed-but-not-enabled unit is listed as Disabled.
     - Choosing Manual reports success and then changes nothing.
     - The real Disabled choice (mask) is greyed out for those units.
   - F05: there is no critical-unit list on Linux. Restarting `dbus`, `systemd-logind` or the display manager is one confirm away, and the confirm text doesn't warn.
   - F11: the "never stop running dependents" guard misses `PartOf=` and `Requisite=`.
   - F10: a oneshot service reports a failed start after it ran.
   - F12: ListUnitFiles plus one D-Bus call per running service goes to PID 1 every 2 s.
3. **Fakes that mirror our own assumptions (HIGH).**
   - `FakeSystemBus` answers every property read itself, and only fails one when told to (`failProperty()`, #2103).
   - The gcore and strace tests use scripts that write what we expect gcore to write.
   - The NVML, ROCm and DRM mocks reproduce our structs.
   - F20: the 64 D-Bus fake tests can skip or compile out with nothing noticing.
   - An interface change upstream (a renamed systemd property, a different `gcore -o` naming, a new udisks2 type) is invisible until a user reports it.
4. **Privacy in System Information (MEDIUM).** The "Show identifiers" toggle promises that Copy and Save report leave out identifying data. With identifiers hidden, the report still carries:
   - globally routable adapter addresses (F06), the same address that "Public IPv6" hides;
   - the user's login name, in mount points and crash paths (F07);
   - credentials embedded in proxy URLs, in the Environment view (F28).
5. **Destructive actions that are hard to test for real (MEDIUM-HIGH).**
   - Kill, suspend, priority, affinity, and systemd start/stop/restart/enable/disable/mask are all tested through fakes or own-process targets.
   - Their identity checks are sound (see WHAT IS IN GOOD SHAPE), but the refusal messages have never been exercised against a real privileged daemon in CI.
   - F19: the "nonexistent process" tests send real signals to PID 99999, which can exist.
   - F09: cancelling a gcore dump can leave the target stopped.
6. **Linux UX on stock desktops (MEDIUM).** With Ubuntu's default `ptrace_scope=1`, Create dump file and Trace system calls are offered but almost always refuse.
   - The refusal is clear (verified in the 2026-10-15 visual pass, #1397), but users see a feature that doesn't work for them. F27 makes this worse for root.
   - F08: following the elevation notice's own `sudo -E` advice leaves a root-owned config that later normal runs can't save.
   - Consider hiding or greying the action, with the reason as a tooltip, or a one-click explanation of `ptrace_scope`.
7. **CI blind spots (MEDIUM).**
   - F17: a PR that touches only one platform's directory skips the other OS's build, although about 20 header-only parsers compile on both.
   - F18: main-health closes its "main is red" issue on a passing push run, even though push runs skip the nightly-only jobs that failed. Since merges happen on green with no review, that issue is the only alarm for those jobs.
   - F21: Windows CI skips every display suite.
8. **Flaky tests under load (MEDIUM).**
   - 27 flake issues were filed since 2026-10-01, and #2054 is still open.
   - A new one appeared on 2026-10-15: `UiLayersOnDisplayTest.TitleBarDragsAndResizesStepWhileTheButtonIsHeld` saw no window movement once on a release runner.
   - 129 `sleep_for` calls in 38 test files remain.
   - F54: a fixed temp folder name races across parallel build trees.
9. **Coupling of tests to ImGui internals (MEDIUM).**
   - 43 test files reach into `GImGui`, `ImHashStr` IDs, `ActivateItemByID` or `ImGuiTable` internals.
   - An ImGui upgrade, or a harmless widget restructure, will break dozens of tests at once.
10. **Process-global state (MEDIUM, carried over from October #9).**
    - `Theme::get()` is used 152 times in 52 files, `Application::get()` 53 times in 14, and `UserConfig::get()` 33 times in 11.
    - There are 38 non-`const` function-local statics in `src/UI` and `src/App`, including the frame-keyed chart caches.
    - F50: these caches, and Theme's font pointers, are not reset when the ImGui context changes.
    - F48: one chart path leaves process-wide pending strip markers for the next chart.
11. **Linux-only and Windows-only drift (LOW-MEDIUM).** The lanes have worked in parallel, and Windows coverage trails Linux by about 4 points. On Windows:
    - F14: service actions ignore the wait hint;
    - F15: SeDebugPrivilege is never enabled, so even an elevated TaskSmack is likely refused on SYSTEM services, although the messages advise running as administrator.

## ARCHITECTURAL OBSERVATIONS

1. **Layer boundaries hold at the include level.** **(verified by two reviewers)**
   - No UI file includes Platform or App.
   - Domain includes nothing above it.
   - Core includes no UI, App or Domain.
   - No `gl*` calls exist outside Core and UI.
   - There are no orphaned headers or sources: every header is included, and every `.cpp` is in CMake.
2. **Platform reaches *up* into Domain for shared constants (LOW).** **(verified)** Seven includes:
   - `Domain/PriorityConfig.h` from `Linux/IoPriority.h`, `LinuxProcessActions.cpp`, `WindowsProcessActions.cpp` and `WindowsProcessActionsMath.h`;
   - `Domain/SamplingConfig.h` from `LinuxProcessProbe.cpp` and `LinuxSystemProbe.cpp`;
   - `Domain/Numeric.h` from `WindowsProcessProbe.cpp`.

   The constants are fine; their home isn't. Move priority ranges, sampling TTLs and numeric helpers into a dependency-free header that both layers include (for example `src/Common/` or `Platform/Config/`). Then Platform genuinely depends on nothing above it. F55 adds a duplicated Windows priority table to the same clean-up.
3. **There is no single composition root (MEDIUM, F24).**
   - Correction to the draft: Processes and System do **not** get their probes from `ShellLayer`. Every panel default-constructs its own platform objects:
     - `ProcessesPanel.cpp:951`, `SystemMetricsPanel.cpp:189`, `ProcessDetailsPanel.cpp:72`;
     - Services, Startup and System Information.
   - Each panel also makes the real-or-synthetic choice itself, inconsistently, so a synthetic run mixes real and fake data.
   - `Core/PathService.cpp:47` also calls `Platform::makePathProvider()`.
   - Every panel does have an injectable factory seam, which is good. What's missing is one `makePanelPlatforms(scenario)` function that `ShellLayer` calls and passes in.
4. **The PGO training shell is a hand copy of `ShellLayer` (MEDIUM, F23).**
   - `UiTrainingDriver.cpp` re-implements the shell's wiring.
   - It has already drifted: no service model, no crash history, no selection count, no background demand.
   - So PGO never trains the code paths the real app runs on every update.
   - It also crashes on its last workload, because System Information calls `getWindow()` without checking for one (F22).
   - Fix: extract a window-independent `ShellPanels` that both use. This goes together with item 3.
5. **Three ways to launch a program on Linux (MEDIUM, F25).**
   - `Platform::DetachedSpawn` (strace) is careful.
   - The gcore path uses `posix_spawn` and leaks non-CLOEXEC fds (F35).
   - `App/PlatformOpen.cpp` does a raw fork/fork/execlp with no fd hygiene, and from App, where the Platform mocks can't reach it.
   - Move "open with the system handler" behind Platform onto `DetachedSpawn`.
6. **Eight hand-rolled `std::async` worker slots in App panels (MEDIUM, F26).**
   - The same cross-thread exception fix (#1685, #1706) was applied site by site in 10 files.
   - A `BackgroundTask<Result>` helper (start, `takeIfReady`, `wait`, a destructor that waits) would remove about 300 lines, and make that crash class impossible to reintroduce.
7. **Very large headers and translation units (MEDIUM, refactoring).**
   - `UI/ChartWidgets.h` is **3,749 lines, header-only, with static caches inside**. Every chart-drawing TU recompiles it, and its function-local static caches are shared across the program.
   - `App/Panels/ProcessesPanel.cpp` is 3,000 lines.
   - `App/Panels/SystemInfoSections.cpp` is 2,213, `ProcessDetailsCharts.cpp` 1,817, `SystemdBus.cpp` 1,600 and `LinuxProcessProbe.cpp` 1,575.
   - See REFACTORING.
8. **Test hooks live in product code (LOW-MEDIUM).**
   - There are 23 `…ForTesting` hooks in headers, plus function-pointer syscall tables (`LinuxProcessSyscalls`, which now includes `ptraceContext`, #2107).
   - There are 11 `TASKSMACK_*` environment variables that change behaviour at runtime: `TASKSMACK_SYNTHETIC`, `_TAB`, `_SELECT_PID`/`_NAME`, `_DETAILS_TAB`, `_WINDOW`, `_OPEN`, `_CONFIG_DIR`, `_LOCALE`, `_LOG_LEVEL` and `_TRACE_RESIZE_PERF`.
   - They are parsed four different ways, with six copies of `trim()` (F57).
   - The synthetic probes (`Platform/Synthetic/`, about 1.6k lines) are linked into the shipping binary and switched on by an environment variable.
   - None of this is wrong, but it is attack and support surface. Parse the env hooks in one place and document them there.
9. **"Unsupported" defaults in every Platform interface (LOW).**
   - Correction to the draft: most `Unsupported*` readers are **live**. `SyntheticScenario.cpp:359-404` returns them so that synthetic PIDs never reach real `/proc` readers. Deleting them would be a correctness regression.
   - Only these are unused in `src/`, because no factory returns them: `UnsupportedSystemInfoProbe`, `UnsupportedServiceProbe` and `UnsupportedServiceActions` (F56).
   - `Platform/Factory.h:86-92` still says Linux returns the Unsupported service classes "until systemd support lands".
10. **D-Bus readers repeat one pattern (LOW, refactoring).** `SystemdBus.cpp` hand-writes the same steps for systemd, udisks2, NetworkManager, networkd and resolved:
    - open the bus;
    - call;
    - map errors to "isn't running", "didn't answer in time", "doesn't manage this", or pass-through;
    - remember "stopped" for the session.

    The error maps differ only in strings. A small typed helper (`busCall<T>(service, path, iface, member, ErrorTexts)`) would remove several hundred lines, and make the timeout and remembering behaviour uniform. Today they differ:
    - udisks2 remembers a timeout, but not on the SmartSupported probe (F13);
    - networkd remembers a stop;
    - resolved and NetworkManager each do their own thing.

    The file's name no longer fits either: it is the sd-bus client for five services (F58).
11. **A naming smell from #2107 (LOW).** `SyscallTrace::Tools` now carries `coreDumperPath` for gcore. The struct is "external tools LinuxProcessActions runs", not syscall-trace tools. Rename it to `ExternalTools` (or similar), and move `ptraceRefusal()`/`PtraceTool` out of `SyscallTrace.h` into a `Ptrace.h`.

## DEAD CODE

**Verified dead:** no callers in `src/` or `tests/`. Remove them (F56).
- `App::ProcessesPanel::setColumnSettings()` (`ProcessesPanel.h:227`): declared inline, never called.
- `App::ProcessesPanel::hasPerProcessGpuMetrics()` (`ProcessesPanel.cpp:1888`): defined, never called.
- `App::ProcessIconView::drawAt()`: declared and defined, never called.
- `UI::Format::getLocaleThousandSep()` (`Format.h:91`).
- `Platform::NetlinkSocketStats::parseSocketMessage()` (private static wrapper, `NetlinkSocketStats.cpp:679`). Everything calls `parseSocketMessageImpl()` directly.
- `Platform::UnsupportedServiceProbe` and `UnsupportedServiceActions`: Linux's factory now returns the systemd classes. `UnsupportedSystemInfoProbe` is constructed only by tests.
- `Core::Event::toString()` and its 12 overrides: never called, including tests (F51).

**Dead at runtime but still costing work:**
- The monospace font (F47).
  - `loadAllFonts()` adds a system monospace face for all six size presets, at startup and on every DPI rebuild.
  - Nothing draws with `Theme::monospaceFont()`.
  - The Linux paths are also Debian-only.
- `BackgroundSampler::recordPass()` updates metrics under a mutex every pass. Only tests and a debug summary read them (F41).

**Test-only:** called from tests and nowhere in `src/`. Delete them, or move them behind a test accessor.
- `BackgroundSampler::requestStop()`, `hasThreadExited()` and `metrics()`, added for #801 and #1416 (F41).
- The `GPUModel` per-process read API from before #1417 (F42):
  - `readProcessGPUCounters()` and `readProcessGPUData()`, plus the `m_NoPerProcessSourceKnown` flag that exists only to speed the latter up;
  - `snapshots()`, `gpuInfo()`, `acceleratorInfo()`, `capabilities()` and `processGPUPublicationVersion()`.

  The same applies to:
  - `ProcessModel::findSnapshotWithVersion()`, `updateFromCounters()` and `rowHistoryMetrics()`;
  - `StorageModel::latestSnapshot()`, `capabilities()` and `maxHistorySeconds()`.
- `UI::Format`: `formatUIntLocalized`, `formatDoubleLocalized`, `formatUptimeShort`, `splitBytesPerSecForAlignment` (production uses the `…Fast` variant) and `formatCpuAffinityMask`. Also `DialogMetrics::fitActionButtonPairWidth` and `Theme::currentThemeIndex` (F51).
- The `barsOnly` path of `renderHistoryWithNowBars()` (F48). It is also buggy: it never drains the pending strip markers it pushes.
- `Domain::ProcessRowHistory::Generation::metrics()` (8 test uses, 0 in `src/`).

**Not dead, despite the draft:**
- `UI::RenderMetrics::renderOverlay()`: Ctrl+Shift+M toggles it (`ShellLayer.cpp:510-518`), and line 655 draws it.
- `App::ProcessPriorityView::ioPriorityView()`: a test accessor for `test_ProcessIoPriorityView*`. Keep it.
- `Platform::nvidiaAdapterFingerprint()`: called at `WindowsGPUProbe.cpp:215`.
- The `IServiceProbe::forgetCachedConfig()` default: `ServiceModel.cpp:63` calls it, and Linux uses the no-op.
- `Unsupported{ProcessConnections,ProcessModules,ProcessOpenFiles,ProcessSecurity,ProcessIconSource}Reader`: used by `SyntheticScenario`.
- `Core::*Event::getName()`: used by `test_Application.cpp:232`.

**Unreachable defensive branches.** These were found by the #1891 coverage work and confirmed by reading the code. Each is guarded upstream, so it can never run.
- `NetlinkSocketStats.cpp:117` (the length check in `parseTcpInfo()`): `parseSocketMessageImpl()` checks the same length first.
- `SystemdBus.cpp` mid-array read failures in `readUnits()` and `readUnitFiles()` (about lines 1138 and 1170): sd-bus has validated the signature when the container is entered.
- `ProcessesPanel::requestSelectionAction()`, its zero- and one-target branches (about lines 2878–2886): the selection is pruned on the same refresh that F9 resolves from.
- `LinuxProcessProbe::readSystemEnergy()`, its empty-path guard: `readPackageEnergy()` checks `m_HasPowerCap` first.
- Windows `dumpBitness()`: its 32-bit TaskSmack branch can't run in a 64-bit-only build. The ARM64-target case it should cover is missed instead (F39).

Keep a defensive branch only if its comment says why. Otherwise turn it into an `assert`, so coverage stops counting it.

## REFACTORING OPPORTUNITIES

Ordered by value for effort.

1. **One composition function, and a shared `ShellPanels`** (F24, F23). This is the highest-value refactor:
   - it fixes the synthetic-run inconsistencies;
   - it makes PGO training follow the real shell by construction;
   - it lets CLAUDE.md name one composition root again.
2. **`BackgroundTask<Result>`** for the eight `std::async` worker slots (F26).
3. **Split `UI/ChartWidgets.h` (3.7k lines, header-only).**
   - Move the non-template parts (axis maths, formatters, the reduction cache, eased bounds, strip layout) into `.cpp` files.
   - Group what's left by concern: `ChartAxes`, `ChartReduction`, `HistoryChart`, `ChartTooltip`, `ChartStrip`.
   - This should noticeably cut incremental build times for every panel, and it moves the static caches into one TU with an explicit owner.
   - Replace the ten positional parameters of `renderHistoryWithNowBars()`, three of them `bool`, with an options struct (F48).
4. **Split `ProcessesPanel.cpp` (3k lines)** into four parts:
   - the table (columns, cells, widths);
   - selection and keyboard;
   - row menu and actions (it already delegates to `ProcessActionsView`, `ProcessDumpView` and the affinity and priority dialogs);
   - the toolbar and filter.

   The panel class keeps orchestration only.
5. **Make `SystemInfoSections.cpp` (2.2k lines) data-driven.**
   - Most sections are a list of `row(label, value, unavailableReason, isIdentifier)` with per-platform reason strings.
   - A table of row descriptors per section, plus a few custom formatters, would cut it roughly in half.
   - It would make "Windows says X, Linux says Y" visible in one place.
   - It would give one place to apply the identifier redaction (F06, F07).
6. **A typed D-Bus call helper** for `SystemdBus.cpp`, and a rename (ARCHITECTURE 10, F58).
7. **One launch path on Linux**: route `PlatformOpen` and gcore through `DetachedSpawn` (F25, F35).
8. **Windows: one open-and-check-identity helper.** The block is copied in five places, and the error mapping has drifted (F36). On Linux, `startTimeTicksAt(pidDirFd)` is copied in five readers.
9. **One home for shared constants** (ARCHITECTURE 2, F55), and a rename of `SyscallTrace::Tools` (ARCHITECTURE 11).
10. **Exact keys instead of `uniqueKey`.**
    - #1503 moved selection and actions to exact `(pid, startTime)` identity.
    - The row-format cache (F02), the sparkline rings (F45) and the tree collapse sets still trust the hash.
    - Keying them by the pair costs the same and is exact.
11. **NaN, not sentinels**, for unavailable values. Two places still store a sentinel:
    - battery charge history uses -1, so the System tab rebuilds the series every frame (F46);
    - the row sparklines store 0 (F45).
12. **Make the mocks thread-safe by default.** `TestMocks::MockProcessProbe::setCounters()` and `enumerate()` aren't synchronised. Add a mutex to the mutable state of the mocks that cross threads (process, system, GPU).
13. **Audit the suppressions.**
    - There are 439 `NOLINT` and 67 `catch (...)` in `src/`.
    - Most are justified, but some predate the clang-tidy gate.
    - Group them by check name, and drop the ones a small refactor removes.
14. **Replace sleeps in tests** with the fake clocks introduced for #1136: 129 `sleep_for` calls in 38 files.

## TEST GAPS

The most valuable missing tests, ranked. "Glaring" means a user-visible failure that the current suite structurally cannot catch.

1. **No real-service smoke tests for the D-Bus integrations (glaring).**
   - Every systemd, udisks2, NetworkManager, networkd and resolved test runs against `FakeSystemBus`.
   - The real-service tests (`SystemdBusTest.Reads…OrSaysWhyNot`) skip when the service is missing, which is always the case on CI runners.
   - F20: nothing notices if the 64 fake tests skip or compile out either.
   - F01 is the proof: the fake and the tests share the wrong start-type mapping.
   - Add one CI job, in a container or VM with systemd as PID 1. It should run the readers and the safe actions against the real daemons:
     - status and list;
     - a start and stop of a test unit;
     - a set-start-type round-trip.
   - Add a guard that fails when the D-Bus suites report zero tests.
2. **No end-to-end test of gcore or strace succeeding (glaring for #807 and #182).**
   - Both are tested only with fake scripts. CI runs under `ptrace_scope=1` without `CAP_SYS_PTRACE`.
   - Add a privileged job, or one with `ptrace_scope=0` in a container. It should:
     - dump a real child process and open the core with `gdb -batch`;
     - cancel a dump mid-write and assert the child isn't left stopped (F09);
     - strace a child for a second.
   - This is the only way to catch a `gcore` output-name change or a gdb attach regression.
3. **Native Wayland is untested (glaring, long-standing).**
   - #865 and #593 are manual.
   - Window placement, drag and resize, maximise, title-bar button routing and DPI scaling on GNOME and KDE Wayland are exercised only by hand.
   - `Core/Window.cpp` (223 uncovered lines) and `App/TitleBarLayer.cpp` (153) are the two least-covered files that users see.
   - A headless Weston or `cage` job running the existing `UiLayersOnDisplay` tests would cover a lot.
4. **Windows display suites never run in CI (glaring for Windows-only window code, F21).** The window, title bar, HWND and GL icon upload suites always skip on the headless runners. Give the Windows job a desktop session, or run them nightly on a runner that has one.
5. **Destructive actions against a real other process.**
   - Kill, suspend and resume, priority, affinity and dump are tested on the test process itself or through fakes.
   - The "nonexistent process" tests use a fixed PID that can exist (F19).
   - Add tests that target a real child: spawn it, act, verify through `/proc`, reap.
   - Use a reaped child's PID as "nonexistent".
   - Cover PID reuse explicitly.
6. **Tests that assert nothing (F52).** Several real-probe tests have names that claim a behaviour, but no assertion about it. Give each an assertion, or rename it to what it checks.
7. **Fuzzing for the newest parsers (F59).** The network-facing DNS reply parser (`DnsMessage.h`) and the user-writable `.desktop` parser are not in the 17-target fuzz set. Add both to ClusterFuzzLite.
8. **Vendor and interface conformance for the fakes.** Record real replies once and replay them through the parsers. That turns "our fake" into "a captured real answer". Sources:
   - `busctl --json`;
   - real `gcore` output names;
   - NVML and udisks samples from a few machines.
9. **Branch coverage is at 86.8%, against 95.7% for lines.** The gap is mostly error branches in the platform readers. Show branch coverage in the nightly summary next to lines, even without a floor.
10. **Windows-only UI and platform branches** still trail (91.5% of lines). Windows has no failing floor; consider one at 90%.
11. **Multi-monitor and DPI.** #2088 was found because a second monitor happened to be attached. Add a CI display configuration with two screens of different DPI for the window-geometry tests.
12. **Long-running soak.** Nothing runs the app for hours at the maximum history and sampling settings with processes churning. The synthetic scenario (#1413) can drive this: run for 30–60 minutes nightly and assert bounded RSS and frame time.
13. **Locale.** Run the whole UI capture suite under a decimal-comma locale with digit grouping (for example `de_DE.UTF-8`), not just the formatter unit tests.
14. **Test hygiene.**
    - The LSan suppression `leak:UI::loadTexture` hides any leak in TaskSmack's own loader, not just Mesa's (F53).
    - `ProcessDumpViewTest` uses a fixed temp-folder name (F54).

## PERFORMANCE

There is no new measurement in this pass. #843 (performance plan v2: idle-first, a sustained 60 FPS) is still open.

The reviewers found four concrete costs:
- **F12:** the Services refresh asks PID 1 for a full ListUnitFiles plus one `MainPID` call per running service, on a new bus connection, every 2 s.
  - This is load on the init system, not just on TaskSmack.
  - Cache the unit-file list and refresh it on systemd's `UnitFilesChanged` signal.
- **F03:** the Environment section reads `/proc/<pid>/environ` on the UI thread. The other Process Details sections moved that read to workers.
- **F29:** batch actions and batch priority run synchronously on the UI thread over every selected process.
- **F43:** ProcessModel's aggregated system histories still deep-copy the whole window per generation on the UI thread.

Features added since October draw per frame (System Information, Services, Startup, NPU charts) and have not all been through the per-frame allocation discipline of #1171 and #1172. A RenderMetrics capture of each tab at the synthetic 5,000-process scenario would show which ones regressed.

## TOP 10 NEXT ACTIONS

1. **Fix the Linux Services start types (F01)**, then the rest of the systemd backend:
   - the critical-unit guard and the Manual confirm (F05);
   - oneshot (F10);
   - PartOf and Requisite dependents (F11);
   - the refresh cost (F12).
2. **Fix the System Information identifier leaks (F06, F07)** before anyone attaches a "redacted" report to a public bug.
3. **Fix the gcore cancel race (F09)** and the root-owned config (F08).
4. **Pay down review debt.** This review covered the platform-boundary and user-facing code merged without review since 2026-10-08. Still unreviewed:
   - `benchmarks/`;
   - the Help, About and status-bar views;
   - the System Information sub-readers on both platforms.

   Restore at least one reviewer for `src/Platform/**`.
5. **Real-service CI smoke job** for D-Bus (a systemd PID 1 container), plus a `ptrace_scope=0` job that runs a real gcore and strace (TEST GAPS 1–2).
6. **Close the CI blind spots:**
   - cross-OS builds for shared headers (F17);
   - main-health event tracking (F18);
   - a zero-test guard for the D-Bus suites (F20).
7. **One composition function and a shared `ShellPanels`** (F24, F23, F22). This also fixes PGO training.
8. **Windows lane:** service wait hints (F14) and SeDebugPrivilege (F15).
9. **Wayland CI job** (headless Weston or cage) for the window and title-bar tests (TEST GAPS 3); that also unblocks closing #865 and #593.
10. **Delete the verified dead code (F56, F41, F42, F47, F51)**, then split `ChartWidgets.h` and `ProcessesPanel.cpp`.

## FINDINGS INDEX

60 findings: **0 CRITICAL, 1 HIGH, 26 MEDIUM, 33 LOW**.
- By lane: 36 `agent:WSL`, 24 `agent:Windows`. Linux-only work (systemd, gcore, strace, D-Bus, Linux CI) stays on WSL. Platform-neutral Domain, UI and App findings went to Windows to keep both lanes fed; either lane can take them.
- By kind: 27 bug, 8 test gap, 7 refactor, 7 dead code, 4 performance, 4 architecture, 3 security.
- Each has its own issue with the full problem, failure scenario, recommended fix and regression test.
- Two merges:
  - F01 includes the app-side report of the same mapping (a "Manual" no-op that also blocks masking).
  - F56 includes the reviewers' corrections to the draft's dead-code candidates.

| ID | Sev | Lane | Kind | Finding | Where | Issue |
|---|---|---|---|---|---|---|
| F01 | HIGH | WSL | bug | Choosing startup type 'Manual' writes `disabled`, which reads back as 'Disabled', so systemd start types don't round-trip | `Platform/Linux/SystemdServices.h:90-107` | #2114 |
| F02 | MEDIUM | Windows | bug | uniqueKey hash collisions make the Processes table show another process's values in a row | `App/Panels/ProcessRowFormat.h:542-562` | #2115 |
| F03 | MEDIUM | WSL | performance | Environment section reads /proc/<pid>/environ on the UI thread, which can freeze the app | `App/Panels/ProcessEnvironmentView.h:126-141` | #2116 |
| F04 | MEDIUM | Windows | bug | Process Details stops drawing the dump progress/result and confirm modals when the target exits, leaving a stale popup that turns off all shortcuts | `App/Panels/ProcessDetailsPanel.cpp:342-365` | #2117 |
| F05 | MEDIUM | WSL | bug | Linux service actions have no critical-unit guard, and 'Manual' turns off boot start with no confirm | `App/Panels/ServiceActionsView.cpp:38-61` | #2118 |
| F06 | MEDIUM | Windows | security | System Information: adapter IP addresses (including global IPv6) are not identifiers, so Copy and Save report leak the address that 'Public IPv6' hides | `App/Panels/SystemInfoSections.cpp:1568-1585` | #2119 |
| F07 | MEDIUM | Windows | security | System Information: the user's login name leaks through mount points and crash executable paths when identifiers are hidden | `App/Panels/SystemInfoSections.cpp:655-665` | #2120 |
| F08 | MEDIUM | WSL | bug | Running as root with HOME kept (sudo -E), as the elevation notice advises, leaves a root-owned config dir/file that later normal launches can't read or save | `App/UserConfig.cpp:721-734` | #2121 |
| F09 | MEDIUM | WSL | bug | Cancelling a gcore dump can leave the target process stopped: the SIGCONT check runs before the orphaned gdb has detached | `Platform/Linux/LinuxProcessActions.cpp:1004-1011` | #2122 |
| F10 | MEDIUM | WSL | bug | Starting a oneshot service (no RemainAfterExit) reports 'It didn't start (inactive)' after it ran successfully | `Platform/Linux/SystemdActions.h:77-96` | #2123 |
| F11 | MEDIUM | WSL | bug | The 'never stop running dependents' guard misses PartOf= (ConsistsOf) and Requisite= (RequisiteOf) dependents | `Platform/Linux/SystemdBus.cpp:1374-1398` | #2124 |
| F12 | MEDIUM | WSL | performance | Services refresh asks PID 1 for a full ListUnitFiles plus one MainPID call per running service on a new bus connection every 2 s | `Platform/Linux/SystemdBus.cpp:1178-1236` | #2125 |
| F13 | MEDIUM | WSL | bug | A udisks2 timeout or access error on the SmartSupported probe is shown as 'no SMART data' and defeats the #1689 timeout short-circuit | `Platform/Linux/SystemdBus.cpp:428-445` | #2126 |
| F14 | MEDIUM | Windows | bug | Service actions give up after a fixed 10 s and ignore dwWaitHint/dwCheckPoint; a slow Restart leaves the service stopped | `Platform/Windows/WindowsServiceActions.cpp:79-109` | #2127 |
| F15 | MEDIUM | Windows | bug | SeDebugPrivilege is never enabled, so even an elevated TaskSmack is likely refused on SYSTEM-service processes | `Platform/Windows/Factory.cpp:56-58` | #2128 |
| F16 | MEDIUM | Windows | bug | Per-interface network history records a false 0 B/s for a new interface's first sample and for a counter reset (StorageModel records NaN) | `Domain/SystemModel.cpp:731-758` | #2129 |
| F17 | MEDIUM | WSL | architecture | A PR that only changes one platform's directory skips the other OS's build, but that OS compiles those headers into its tests | `.github/workflows/ci.yml:69-74` | #2130 |
| F18 | MEDIUM | WSL | bug | main-health closes the 'main is red' issue when a push run passes, even though push runs skip the nightly-only jobs that failed | `.github/workflows/main-health.yml:62-75` | #2131 |
| F19 | MEDIUM | WSL | test-gap | The 'NonExistentProcess' tests send real terminate, kill, stop, resume and priority calls to PID 99999, which can be a live process | `tests/Platform/test_LinuxProcessActions.cpp:241-291` | #2132 |
| F20 | MEDIUM | WSL | test-gap | The D-Bus fake suites (64 tests, all of systemd actions, services and NetworkManager) can skip or compile out silently in CI | `CMakeLists.txt:280-293` | #2133 |
| F21 | MEDIUM | Windows | test-gap | Windows CI skips every display suite (window, title bar, HWND, GL icon upload), so Windows-only window code never runs in CI | `.github/workflows/reusable-build-test.yml:287-290` | #2134 |
| F22 | MEDIUM | WSL | bug | System Information panel calls getWindow() unguarded; headless PGO training throws during its last workload, and pgo.sh exits early | `App/Panels/SystemInfoPanel.cpp:224-231` | #2135 |
| F23 | MEDIUM | WSL | architecture | PGO TrainingShell is a hand-copied ShellLayer that has already drifted (missing service model, crash history, selection count, background demand) | `Training/UiTrainingDriver.cpp:164-330` | #2136 |
| F24 | MEDIUM | WSL | architecture | The composition root is spread across every panel, and the real/synthetic choice is made inconsistently panel by panel | `App/Panels/ProcessesPanel.cpp:951-957` | #2137 |
| F25 | MEDIUM | WSL | architecture | Linux launches external programs three different ways; App/PlatformOpen does a raw fork/fork/execlp with no fd hygiene, bypassing the hardened Platform::DetachedSpawn | `App/PlatformOpen.cpp:75-132` | #2138 |
| F26 | MEDIUM | Windows | refactor | Eight hand-rolled std::async worker slots in App panels; the same cross-thread exception fix (#1685/#1706) was applied site by site in 10 files | `App/Panels/StartupActionsView.cpp:93-191` | #2139 |
| F27 | MEDIUM | WSL | bug | strace is launched through client/server terminals (gnome-terminal, ptyxis, Ubuntu's x-terminal-emulator), so the ptrace pre-check is wrong and a root TaskSmack reports success for a trace that cannot attach | `Platform/Linux/SyscallTrace.h:48-62` | #2140 |
| F28 | LOW | WSL | security | Environment masking uses only the variable name, so credentials embedded in proxy and service URLs are shown in clear | `App/Panels/ProcessEnvironmentMasking.h:51-67` | #2141 |
| F29 | LOW | Windows | performance | Batch actions and batch priority run synchronously on the UI thread over every selected process | `App/Panels/ProcessesPanel.cpp:2910-2936` | #2142 |
| F30 | LOW | Windows | refactor | ProcessBatchPriorityDialog is implicitly copyable while m_Listed holds pointers into m_Targets | `App/Panels/ProcessBatchPriorityDialog.h:84` | #2143 |
| F31 | LOW | Windows | bug | Save report adds an extension after the dialog, which can overwrite an existing file unasked; the filter can also put JSON in a .txt name | `App/Panels/SystemInfoPanel.cpp:194-205` | #2144 |
| F32 | LOW | WSL | bug | Settings > 'Edit config file' does nothing on a first run, because config.toml is only written at exit | `App/SettingsLayer.cpp:95-97` | #2145 |
| F33 | LOW | WSL | bug | Setting 'Automatic' on a static unit (no [Install] section) reports success but changes nothing | `Platform/Linux/SystemdBus.cpp:1400-1431` | #2146 |
| F34 | LOW | WSL | bug | Dump file name reservation isn't atomic: two concurrent dumps can share a name, the failing one deletes the other's file, and rename() can overwrite | `Platform/Linux/LinuxProcessActions.cpp:1176-1197` | #2147 |
| F35 | LOW | WSL | bug | runCancellable leaks TaskSmack's non-CLOEXEC fds into gcore/gdb and can leave the child running on a waitpid error; the gcore privilege probe reads the script's file caps instead of gdb's | `Platform/Linux/LinuxProcessActions.cpp:1046-1131` | #2148 |
| F36 | LOW | Windows | refactor | The open-and-check-identity block is copied in five places, with drifting error mapping | `Platform/Windows/WindowsProcessActions.cpp:60-101` | #2149 |
| F37 | LOW | Windows | bug | Open-files hung-handle list is keyed by (pid, handle) with no start time and never pruned | `Platform/Windows/WindowsProcessOpenFiles.h:149` | #2150 |
| F38 | LOW | Windows | bug | Battery capacity and health come from the first battery only, while charge percent covers all batteries | `Platform/Windows/WindowsBatteryInfo.cpp:96-143` | #2151 |
| F39 | LOW | Windows | dead-code | Dump bitness check: the 32-bit-TaskSmack branch is unreachable, and the ARM64 cross-architecture case is missed | `Platform/Windows/WindowsProcessDump.cpp:172-176` | #2152 |
| F40 | LOW | Windows | bug | GPUModel::computeSnapshot reads m_GPUInfo without m_Mutex, which contradicts the header's claim that racing refreshes are safe | `Domain/GPUModel.cpp:398-419` | #2153 |
| F41 | LOW | WSL | dead-code | BackgroundSampler::requestStop(), hasThreadExited() and metrics() are only called by tests | `Domain/BackgroundSampler.h:93-109` | #2154 |
| F42 | LOW | WSL | dead-code | GPUModel's per-process read API from before #1417 is test-only, and so is a lock-free flag that exists only to speed it up | `Domain/GPUModel.h:231-255` | #2155 |
| F43 | LOW | Windows | performance | ProcessModel's aggregated system histories still deep-copy the whole window per generation on the UI thread (not migrated to SharedHistoryBuffer, #1412) | `Domain/ProcessModel.h:297-302` | #2156 |
| F44 | LOW | Windows | bug | CrashHistory orders publications by wall-clock seconds, so a backward clock step silently discards every crash read until the clock catches up | `Domain/CrashHistory.cpp:24-28` | #2157 |
| F45 | LOW | WSL | bug | Sparkline rings are carried over by the uniqueKey hash, and unavailable values are stored as 0 | `Domain/ProcessRowHistory.cpp:53-80` | #2158 |
| F46 | LOW | Windows | refactor | Battery charge history uses a -1 sentinel instead of NaN, so the System tab rebuilds the series every frame to convert it | `Domain/SystemModel.cpp:309-310` | #2159 |
| F47 | LOW | WSL | dead-code | Monospace font is loaded for every preset and on every DPI rebuild but never used | `UI/FontLoading.cpp:113` | #2160 |
| F48 | LOW | WSL | dead-code | renderHistoryWithNowBars() bars-only path is test-only and never drains pending strip markers | `UI/ChartWidgets.h:3566-3656` | #2161 |
| F49 | LOW | Windows | bug | Exceptions can escape the SDL file-dialog C callback, possibly on SDL's dialog thread | `Core/SaveFileDialog.cpp:28-45` | #2162 |
| F50 | LOW | WSL | test-gap | Frame-keyed static caches and Theme font pointers are not reset when the ImGui context changes | `UI/ChartWidgets.h:1079-1090` | #2163 |
| F51 | LOW | Windows | dead-code | Test-only formatters and Event::toString overrides ship in product headers | `UI/Format.h:90` | #2164 |
| F52 | LOW | WSL | test-gap | Several real-probe tests assert nothing, despite names that claim a specific behavior | `tests/Platform/test_NetlinkSocketStats.cpp:122-127` | #2165 |
| F53 | LOW | WSL | test-gap | The LSan suppression 'leak:UI::loadTexture' hides any leak in TaskSmack's own texture loader, not just Mesa's | `tests/sanitizer-suppressions/lsan.supp:21-25` | #2166 |
| F54 | LOW | WSL | test-gap | ProcessDumpViewTest's temp folder has a fixed name, so test runs in parallel build trees or worktrees race on it | `tests/App/test_ProcessDumpView.cpp:760-797` | #2167 |
| F55 | LOW | Windows | refactor | The Windows priority-class/nice table is duplicated in App and Platform with magic numbers | `App/Panels/ProcessDetailsPanel_PriorityHelpers.h:324-345` | #2168 |
| F56 | LOW | WSL | dead-code | Remove the verified dead code (ProcessIconView::drawAt, UnsupportedServiceProbe/Actions, setColumnSettings, hasPerProcessGpuMetrics, getLocaleThousandSep, parseSocketMessage) and fix the stale Factory.h comment | `App/Panels/ProcessIconView.h:80-81` | #2169 |
| F57 | LOW | WSL | refactor | Env-var test hooks are parsed four different ways, with six copies of trim() | `App/SelectOverride.h:80` | #2170 |
| F58 | LOW | WSL | refactor | The name 'SystemdBus' no longer matches its job: it is the sd-bus client for five services | `Platform/Linux/SystemdBus.h:43-108` | #2171 |
| F59 | LOW | WSL | test-gap | The network-facing DNS reply parser and the user-writable .desktop parser are not in the fuzz set | `Platform/DnsMessage.h:157-232` | #2172 |
| F60 | LOW | Windows | bug | Windows multi-group affinity uses soft CPU sets but reports 'Set CPU affinity'; a process with a hard affinity mask or thread-selected CPU sets keeps running where it was | `Platform/Windows/WindowsProcessActions.cpp:423-476` | #2173 |

## APPENDIX: NUMBERS

| | 2026-10-03 (`90ca6e5`) | 2026-10-15 (`8ad96cc8`) |
|---|---|---|
| `src/` files | 202 | 546 |
| `src/` lines | (not recorded) | about 139k |
| Test cases | (not recorded) | about 6,460 |
| Linux line coverage | 54–80% (#1395) | 95.65% (nightly 95.51%) |
| Windows line coverage | not measured | 91.54% |
| Review findings | 99 (1 C, 8 H, 45 M, 45 L) | 60 (0 C, 1 H, 26 M, 33 L) |
| Open `code-review` issues before this review | 99 filed | 0 |
| PRs merged without review | 0 | 249 (since 2026-10-08) |
| Required checks | 6 | 6 |
| Linux coverage floor | 85% warn | 95% failing |
