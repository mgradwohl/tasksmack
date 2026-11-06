# Issue and PR labels

This is the label scheme the maintainer approved on 2026-11-03. Every agent, and every person filing or triaging,
follows it. [AGENTS.md](https://github.com/mgradwohl/tasksmack/blob/main/AGENTS.md) has a short version. A label's colour tells you its family, and its GitHub
description gives its meaning.

## The families

| Family | Colour | Labels | Who sets it |
|---|---|---|---|
| **Kind**: what it is | GitHub defaults | `bug`, `enhancement`, `documentation`, `refactoring`, `performance`, `security`, `testing` | whoever files it |
| **Severity**: how bad (bugs only) | orange, dark to light | `sev:high`, `sev:medium`, `sev:low` | the filer or the triager |
| **Priority**: when | greyscale, dark to light | `pri:high`, `pri:medium`, `pri:low` | the maintainer |
| **Lane and planning** | purple | `agent:Windows`, `agent:WSL`, `parity` | the triager |
| **Area** | teal | `windows`, `linux`, `ui`, `gpu`, `networking`, `architecture`, `accessibility`, `globalization` | the filer, or the PR labeler |
| **Automation** | green | `ci`, `ci:arm64`, `tooling`, `release`, `dependencies` | the PR labeler, Dependabot, Renovate, or by hand |
| **Alert** | red | `ci-red-main` | `main-health.yml` only |
| **Process** | magenta | `code-review`, `manual close`, `merging`, `question` | see below |
| **Outside contributors** | GitHub defaults | `good first issue`, `help wanted` | the maintainer |

## Severity: bugs only

Severity describes the impact when the bug happens, not when we'll fix it.

- `sev:high`: a crash, data loss, a security hole, or a broken build or release.
- `sev:medium`: a wrong or misleading result users notice, or something broken on a common setup.
- `sev:low`: a cosmetic or minor defect, such as wrong text, a misaligned control, or a slow path that is still correct.
  A missing test, a perf idea or a docs improvement on its own is not a bug, so it gets no severity.

**Features (`enhancement`) never get a severity.** Size and value go in the issue body.

## Priority: the maintainer's call

Priority says when the work happens this cycle. **Every open issue on a release milestone carries exactly one.**

- `pri:high`: do first this cycle. This covers the cycle's systems work (CI, infra, build, dependencies at the start
  of a release), anything blocking other work, and every `sev:high` bug by default.
- `pri:medium`: normal work for the milestone. A new milestone item defaults to this.
- `pri:low`: only if there's time. It's the first candidate to move to the next milestone at the freeze.

Agents put `pri:medium` on new milestone items, and `pri:high` on a `sev:high` bug and on the cycle's stage-1 CI,
infra, build and dependency issues ([release-process.md](release-process.md), stage 1). An agent may *suggest* a
different priority in a comment, but **never raises one** the maintainer set, and never lowers one either.

## Queue order

Each lane takes work in this order (maintainer, 2026-11-04):
1. **CI, infra, tooling, build and dependencies** (`ci`, `tooling`, `dependencies`, `release`).
2. **Bugs** (`bug`), with the test gaps that go with fixing them.
3. **Refactors** (`refactoring`), before any feature, so features build on the cleaned-up structure.
4. **Parity features** (`parity`).
5. **New features** (`enhancement` without `parity`).

Priority orders the work *within* each step: `pri:high`, then `pri:medium`, then `pri:low`. It never moves a
feature ahead of a refactor or a bug. Severity breaks ties among bugs.

No feature merges before the refactor it depends on. The release tracker (`[RELEASE X.Y]`) lists the cycle's own
order.

## Lane, parity and areas

- **A lane label is required** once the platform is clear:
  - `agent:Windows` for Windows, UI design, Core/Window and the release pipeline;
  - `agent:WSL` for Linux probes, Linux packaging and the docs site.
  - Work with a half on each platform carries both, and its body says who does which half.
  - Keep the filer's lane unless it's clearly wrong, and say why in a comment if you change it.
- **`parity`** marks a Windows/Linux gap. It goes ahead of new features (maintainer, 2026-11-03). Each parity issue
  names the platform that has the feature (file:line) and the half to build. The AGENTS.md parity rule says when a
  gap needs an issue and when it needs a Platform Differences row instead.
- **Areas:** `windows` and `linux` are applied by the PR labeler from the paths under `src/Platform/`. Add the
  others by hand.

## Automation and process labels

- **PR labeler** (`.github/labeler.yml`): `windows`, `linux`, `testing`, `documentation`, `ci`, `tooling`,
  `dependencies`, `security` and `release`, by changed path.
- **Dependabot** (`.github/dependabot.yml`): `dependencies` plus `ci` on its GitHub Actions PRs, and `dependencies`
  plus `tooling` on its pip PRs.
- **Renovate** (`.github/renovate.json5`): `dependencies` plus `ci` on every PR. Its Actions and pip managers are
  off, since Dependabot covers those (`enabledManagers` is `custom.regex` and `pre-commit`). So a Renovate PR
  bumps a version pinned by a custom regex (CMake FetchContent and toolchain pins) or a pre-commit hook.
- **`ci:arm64`:** add it to a PR to run the ARM64 build and test jobs (`arm64.yml`).
- **`ci-red-main`:** `main-health.yml` opens, rewrites and closes this tracking issue. Don't add it by hand.
- **`winget-health.yml`:** opens, rewrites and closes one issue, "WinGet: TaskSmack.TaskSmack <version> isn't
  published", labelled `release` and `agent:Windows` and found by that title among the open `release` issues
  (#3486). It never uses `ci-red-main`: a WinGet moderation delay isn't a red main.
- **`merging`:** the cross-lane merge lock. A PR in its final merge stage carries it, and the other lane waits up to
  30 minutes. The backport action doesn't copy it.
- **`code-review`:** every issue a code-review sweep or audit files, so each batch can be triaged with one query:
  a general audit's findings have no milestone (`gh issue list --label code-review --search "no:milestone"`), and
  a release sweep's are on that release's milestone (`gh issue list --label code-review --milestone vX.Y.0`).
- **`manual close`:** a person must verify it by hand (a real desktop, hardware or a compositor) before it's closed.
- **`question`:** it needs the maintainer's answer before work continues. Triage adds it when an issue's claim
  looks wrong; agents never close those issues.
- **`good first issue` and `help wanted`** are kept for outside contributors. Agents don't pick up, plan or
  milestone them. They give an outside contributor's PR on one a normal review, and don't take it over.

## Backport labels

A `backport:X.Y` label exists only while `release/X.Y` takes backports. It's created when the branch first needs one
and deleted when the branch retires. [AGENTS.md](https://github.com/mgradwohl/tasksmack/blob/main/AGENTS.md) ("Branches and backports") describes the flow. A
release with no patch line (v0.10, maintainer 2026-11-03) has no backport label.

## Adding or removing a label

Ask the maintainer first. A new label must fit a family above: its colour and a description saying what it means
and who sets it. Before deleting a label, check that nothing creates it: `labeler.yml`, `dependabot.yml`,
`renovate.json5` and the workflows. Otherwise it comes back. Update this page in the same change.
