---
name: code-review
description: Reviews TaskSmack pull requests for C++23 compliance, memory/exception safety, thread safety, layered architecture boundaries, naming/include conventions, clang-tidy pitfalls, CI and release workflows, and how to file issues from a codebase audit. Use when reviewing any pull request, code change or range of commits in this repository.
---

# TaskSmack Code Review

Apply these checks when reviewing pull requests or diffs in this repository. Report only genuine issues; skip style nits already enforced by clang-format.

## 1. Architecture boundaries (highest priority)

TaskSmack has a strict layered architecture: App → UI → Core → Domain → Platform.

- Platform probes (`src/Platform/`) must return **raw counters** only (CPU ticks, bytes). Flag any computation of rates, deltas, or percentages in a probe.
- Domain (`src/Domain/`) must not depend on UI, Core, ImGui/ImPlot, or graphics libraries.
- UI code must never call Platform directly, include `Platform/Factory.h`, or call `Platform::make*()`. Path access goes through `Core::Application::get().paths()`.
- App is the composition root: `Platform::make*Probe()` / `Platform::makeProcessActions()` are called only in `App/PanelPlatforms.cpp` (and `App/SyntheticScenario.cpp`), which a panel calls at attach time from its sampler's start hook (`SamplerConfig::onThreadStart`), so probes are built and seeded on their sampler thread, never on the UI thread and never in render loops (#2731). The factory stays the only caller: flag a probe built anywhere else, and `Platform/Factory.h` included anywhere else in `App/` or `UI/`.
- OpenGL calls belong in `Core/` and `UI/` only.

## 2. C++23 compliance

- Suggest modern replacements for legacy patterns **only when the replacement preserves behavior and improves clarity**: `find()` used purely as a containment/prefix check → `contains()`/`starts_with()` (not when the position is needed, e.g., for `substr` parsing); raw loops → `std::ranges`/`std::views` when the loop has no early exit or iterator dependence (not when iterators are needed, e.g., map erase patterns); `fmt::format` → `std::format`; raw threads → `std::jthread`; C-style arrays → `std::array`/`std::span`.
- Flag C-style casts; require the narrowest named cast with a comment explaining why it is safe.
- Flag `using namespace` in headers.
- Prefer `enum class` over plain `enum`, with an explicit underlying type (e.g., `: std::uint8_t`) when size matters. Do not flag C-style enums that intentionally mirror an external ABI (e.g., `src/Platform/NVMLTypes.h`), especially when marked with NOLINT and a rationale.

## 3. Memory and exception safety

- Flag raw `new`/`delete`; require smart pointers and RAII.
- Rule of 5: if any of destructor/copy/move ops is user-defined or deleted, all five must be handled.
- Destructors, move constructors, and swap should be `noexcept`.
- Check `std::optional` access is guarded (`has_value()`).

### Per-frame caches (#843, #2937)

- **The key names every input:** a cache that skips per-frame work must key on everything the uncached code read:
  data generation, series identity and size, window start, axis length, scale or units, selection. Flag any input
  left out, and any series whose contents can change while its size and generation stay the same.
- **Identities stay unique across resets:** a stamp that records "gathered from this cache" must compare a
  process-unique id, not a per-instance counter that restarts at 0 when the cache is reset, replaced or pruned.
- **Draw counts stay in bounds:** when x and the y buffers are rebuilt at different times, the draw count is the
  shortest buffer's length, never `x.size()` alone.

## 4. Thread safety

- Verify mutex usage and `std::atomic` correctness in `BackgroundSampler` and snapshot publication paths.
- Snapshots published to UI must be immutable and versioned; flag shared mutable state crossed between sampler threads and render code.
- A new background thread uses `Domain::Worker` (`src/Domain/Worker.h`, #3343); a raw `std::jthread` or detached `std::thread` needs a stated reason at the thread.

### Timing and cadence (#2731)

- **The Settings interval rules:** data is sampled at the refresh interval the user set in Settings. Flag anything
  that rescales or pauses that interval from UI state: tab visibility, interaction, resizing, a minimized window,
  or the frame rate.
- **The UI thread draws, it doesn't sample:**
  - flag any probe call, blocking syscall, registry or file read, PDH/WMI/COM query or file write in `onUpdate` or
    render paths;
  - flag probe construction or seed sampling in `onAttach`: probes are built and seeded on their sampler thread.
- **Locks:** flag a lock that a sampler holds across I/O when the UI thread also takes it. The UI may only wait for
  a pointer swap.
- **Separate cadences:**
  - unrelated data domains must not share one sampler thread when one of them can stall (GPU/NVML/DXGI, disk PDH);
  - slow facts use a named constant in `SamplingConfig.h`;
  - flag timing that counts frames instead of using a clock.
- **Tests:** new timed work needs a test that its period doesn't depend on UI state or frame rate, and that a stalled
  sampler doesn't delay frames.
- **Settings saves (#2916):** a save triggered from the UI goes through `UserConfig::saveInBackground()`, which a
  worker coalesces. Flag a synchronous `save()` anywhere else. Only two are allowed: the shutdown save in
  `ShellLayer::onDetach()`, which flushes the worker first, and the first-run save before Settings opens config.toml
  in an editor.

## 5. Conventions

- Naming: `PascalCase` classes, `camelCase` functions, `m_PascalCase` members, `UPPER_SNAKE_CASE` constants, `PascalCase` enumerators.
- Include order: matching header → project headers → third-party → stdlib, blank line between groups; `#pragma once` in headers.
- GLAD before SDL: `#include <glad/gl.h>` then `#include <SDL3/SDL.h>`.
- Reuse constants from `src/Domain/SamplingConfig.h`; flag re-declared sampling literals.
- Windows code must call wide-character (W) APIs and convert to UTF-8 at the boundary.
- Windows/Linux parity (from v0.11.0, AGENTS.md): a feature for one platform needs one of three things.
  It ships on the other platform too; or the PR says why the other platform can't support it and
  adds a Platform Differences row in the user guide; or the PR links a parity issue filed for the
  other lane. Flag a one-platform feature that has none of these.

## 6. Testing

- New Domain logic should have Google Test coverage in `tests/` (mirroring `src/`) using mocks from `tests/Mocks/MockProbes.h`.
- Flag `EXPECT_EQ` on floating-point values (use `EXPECT_DOUBLE_EQ`).
- Flag mocks defined inside anonymous namespaces when used with `std::make_unique`.
- **On-display UI tests:** they derive from `TestSupport::ScratchConfigDirTest` (`tests/App/ScratchConfigDir.h`,
  #2932), so each test gets its own empty config dir. Flag a test that reads or writes the shared config, or that
  only passes because an earlier `--gtest_repeat` run saved a layout.
- **No frame-timing assumptions:** the event drain stops after `DRAIN_BUDGET_MS` (`src/Core/Application.cpp`), so two
  events pushed together can be handled in different frames under load (#2901). A test waits on the state it
  observes, not on a frame count. A scenario step retries from fresh geometry until it takes (#2906).
- **Real OS paths (#3489):** a change to a real OS call that tests reach only through fakes (a process action, the
  shell, the elevated helper, a system figure) adds a `CiRealSystem*` test (`tests/Support/CiRealSystem.h`; CI's
  "Real-system tests" step runs it on a disposable runner), or the PR says why only a manual check can cover it (a
  UAC prompt, a sign-out, hardware). Flag a PR that does neither.
- **Fuzz builds:** when a source a fuzzer compiles gains a dependency, the fuzzer's `build_fuzzer` line in
  `.clusterfuzzlite/build.sh` needs it too. A missing one fails "Fuzz changed parsers" at link time (#2930).

## 7. clang-tidy pitfalls

Flag: uninitialized members, missing `override`, `NULL`/`0` for pointers, `const` return by value, unnecessary copies (use `const&`), missing parentheses in math expressions, and exceptions escaping `noexcept` functions.

- **spdlog in a `noexcept` function:** spdlog catches its own formatting and sink errors inside `log()`, so the
  repo's convention is a `NOLINTNEXTLINE(bugprone-exception-escape)` with that reason, not a try/catch around each
  call. See `src/App/CoalescingWriter.h`.
- **Newer clang-tidy:** checks that only a newer LLVM than CI's runs, such as `readability-trailing-comma` on LLVM 23
  while CI is on 22, aren't findings until CI's LLVM moves (#945).

## 8. CI and release workflows

Changes under `.github/workflows/` need the maintainer's explicit OK before they're pushed (AGENTS.md).

- **Pinning and permissions:**
  - every third-party action is pinned by full commit SHA with a version comment;
  - each job declares only the permissions it uses;
  - signing and cloud access go through OIDC, never a stored client secret;
  - flag a secret that can reach a log, or an untrusted input (`github.event.*`, a PR title or branch name)
    interpolated into a `run:` script.
- **release.yml is proven by a dry run:** `gh workflow run release.yml --ref <branch> -f dry_run=true`. The PR states
  the run and each affected job's result. Tag runs and dry runs differ only where they must: publishing, attesting,
  and the signing gate.
- **Skipped dependencies:** a job whose `needs:` include one that can be skipped (signing while it's off) uses
  `if: ${{ !cancelled() && needs.<job>.result == 'success' }}`. The implicit `success()` treats a skipped
  dependency as a failure and silently skips the job (#2721).
- **Backports:** a change labelled `backport:X.Y` must not depend on anything that is on `main` but not on
  `release/X.Y`. Check the release branch, not your memory of it: a hook, test seam or helper added after the cut
  isn't there. The backport action can't push workflow changes, so those are cherry-picked by hand.
- **Signatures:** match the signer's certificate subject (CN), never a thumbprint or serial. Azure Artifact Signing
  certificates rotate every few days (#2302).

## 9. Auditing the codebase and filing issues

For a review of the codebase or of a range of commits, not a single diff. The findings go in GitHub issues, not in a
document.

- **Never commit a review document.** No audit report, findings list or notes file goes into the repo. Each finding
  is its own issue, and an optional tracker issue links them.
- **Check before filing.** Search open and closed issues and PRs first (`gh issue list --search "<words>" --state all`).
  If one already covers the finding, comment there instead of filing a duplicate.
- **Verify each finding.** Trace the code path, or reproduce it in a test or under the synthetic machine
  (`TASKSMACK_SYNTHETIC`). File only what you can show, and say how sure you are.
- **One self-contained issue per finding**, from `.github/ISSUE_TEMPLATE/bug_report.md` or `feature_request.md`, with:
  - a title that states the defect;
  - the location as a permalink at a commit SHA (`https://github.com/mgradwohl/tasksmack/blob/<sha>/<path>#L<n>`);
  - a concrete failure scenario: inputs or state, then the wrong result;
  - a suggested fix and a size (S, M or L);
  - a body that ends with `🤖 Generated with [Claude Code](https://claude.com/claude-code)` when an agent wrote it.
- **Labels** (the full scheme is `docs/dev/labels.md`):
  - the kind: `bug`, `enhancement`, `refactoring`, `performance`, `security` or `testing`;
  - the area: `windows`, `linux`, `ui`, `gpu`, `ci`, `tooling` and so on;
  - a lane if the platform is clear: `agent:Windows` or `agent:WSL`;
  - on a **bug**, one severity, which says how bad it is: `sev:high` (a crash, data loss, a security hole, or a
    broken build or release), `sev:medium` (a wrong or misleading result users notice, or broken on a common
    setup) or `sev:low` (a cosmetic or minor defect). Features, test gaps and perf ideas get no severity;
  - priority is the maintainer's, and says when: `pri:high` (do first this cycle; every `sev:high` bug by default),
    `pri:medium` (normal work for the milestone) or `pri:low` (only if there's time). Every open issue on a release
    milestone carries one. An agent may suggest one in the issue, and never raises it;
  - `code-review` on every issue an audit files, so the batch can be triaged with one query: a general audit's
    have no milestone (`gh issue list --label code-review --search "no:milestone"`), and a release sweep's are on
    its milestone (`gh issue list --label code-review --milestone vX.Y.0`);
  - no milestone for a general audit. The maintainer sets those, and agents never put an issue on a
    release milestone to get it in. The exception is a release's own code-review sweep (the stage 4 review in
    `docs/dev/release-process.md`, tracked by a `[RELEASE X.Y] Code-review sweep` issue): its findings go on
    that release's milestone so blockers are tracked with the release, each saying whether it blocks
    (maintainer, 2026-10-31).
- **Skip** style nits that clang-format or clang-tidy already enforce, and anything a section above calls fine.
- **Running TaskSmack to check a finding:**
  - give it its own `TASKSMACK_CONFIG_DIR`, and launch it at most once;
  - never inject input unless TaskSmack is the foreground window;
  - never launch it on the maintainer's desktop without asking.
