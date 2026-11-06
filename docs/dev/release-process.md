# TaskSmack Release Process

How a TaskSmack minor release goes from plan to published, and how a patch release follows it. In force from
v0.10.0 (maintainer, 2026-10-28). The mechanics of `release.yml`, `changelog.yml` and `backport.yml` are in
[CONTRIBUTING.md](../../CONTRIBUTING.md#cutting-a-release). The rules every agent follows are in
[AGENTS.md](../../AGENTS.md).

## Overview

A release moves through nine stages, 0 to 8:

- **Building the release (stages 1–3):** foundation, then fixes, then features. The two lanes work in parallel,
  and this order decides what gets picked next.
- **Feature freeze:** comes after stage 3. An event triggers it, not a date.
- **After the freeze:** the code review, the docs, the candidate and the release, in that order. Each sits behind
  a gate.
- **Proving the pipeline:** the release pipeline is proven at the start of the cycle, and again on the exact
  candidate, so release day holds no surprises.

## Stages

### 0. Plan and scope

- **The milestone and a release tracker issue.** Open them first; for example, #3223 for v0.10.0.
- **The feature list.** Write down the features in scope.
- **Outside dependencies with lead time.** List every one, and start them first:
  - signing vendors and certificates;
  - token scopes;
  - account 2FA;
  - store or SmartScreen setup.

**Done when:** the milestone and tracker exist, the feature list is written down, and each lead-time item has an owner.

### 1. Foundation: dependencies, toolchain, CI, release pipeline

Get the systems into good shape before the code changes much. Label every CI, infra, build and dependency issue on
the milestone `pri:high`, so the conveyor picks them up first.

- **Dependency updates** from Renovate and Dependabot: compiler, libraries, Python and actions. Approved majors
  go here.
- **CI timeouts with headroom:** each job finishes in under about 70% of its limit on a cold cache.
- **A signed release dry run on main.** Fix whatever fails, and repeat it after any change to `release.yml`:

  ```bash
  gh workflow run release.yml --ref main -f dry_run=true
  ```

- **Baselines,** recorded on the tracker during quiet hours with `tools/measure-idle.ps1` and `tools/bench.ps1`
  (pinned to the P-cores):
  - idle CPU, visible and minimized;
  - memory over 90 minutes;
  - startup time;
  - benchmarks against the previous release.

- **A performance analysis** with the repo's tools, on Windows and Linux, run against those baselines. It files
  product and tooling perf bugs early, when a fix can still be a refactor in stage 2.
- **Fault-injection and regression tests** that protect stage 2's refactors.

**Done when:** main is green, the signed dry run passes, and the baselines are posted.

### 2. Fixes: refactors, bugs, test gaps

Priority first, then severity among bugs ([labels.md](labels.md), "Queue order"). Test gaps belong here, not at the end, because a test written before a refactor has to be redone.
Refactors come before features, so new features build on the cleaned-up structure. The two lanes may be in
different stages, but no feature merges before the refactor it depends on.

Finish with a short perf re-measure against stage 1's baselines, since refactors often touch hot paths. File any
regression as a bug for this milestone.

**Done when:** no CRITICAL or HIGH bug is open on the milestone. MEDIUM and LOW fixes may still land after the freeze.

### 3. Features, each with its tests

Every feature PR includes unit tests, and an on-display test when it adds UI. A large feature PR also posts its
local Linux coverage figure (`tools/coverage.sh`) before it merges.

**Done when:** the feature freeze is declared.

> **Gate: feature freeze.** From here, main takes only fixes, review findings, docs and release mechanics until
> the candidate is approved. See [Feature freeze](#feature-freeze).

### 4. Code review and hardening

- **Deep review:** sweeps of the freeze commit by both lanes, split by area. They look for what reviewing one PR
  at a time misses, such as how merged changes interact.
- **Findings:** fix them. A blocking finding stays on the milestone; the rest move to the next minor.
- **Baselines:** compare against stage 1's numbers. A performance or memory regression from this cycle is a bug
  for this milestone.
- **Performance and coverage:** measure, and fix what turns up. A fix the size of a refactor is decided case by
  case, on its size and benefit. Top up test coverage; most of it came in with stages 2 and 3.
- **Dependency and security sweep** before the candidate.
- **Dependencies:** frozen, except for security fixes.

**Done when:** the review sweeps are closed and no blocking finding is open.

### 5. Docs, website, screenshots, release notes

- **Docs:** update the user guide, the website and CONTRIBUTING for the new features.
- **Screenshots and animations:** regenerate them on the reviewed build, in Windows Sandbox with a signed build,
  plus the Linux set under Xvfb.
- **Release notes:** drafted on the tracker, with highlights, the "Which file should I download?" guide and known
  issues. `changelog.yml` generates the body; the draft checks it reads well.
- **`RELEASE.md` for users:** how to verify a download. That covers the Authenticode signer, `SHA256SUMS`, cosign
  bundles and build-provenance attestations. The notes' download guide links to it.

**Done when:** the maintainer has read the notes draft and the docs PRs have merged.

### 6. Candidate and validation

1. **Soak:** main green for 24 hours, with every required push check and the nightly jobs passing.
2. **Candidate:** one exact commit proposed on the tracker. Main already reads `VERSION X.Y.0` in `CMakeLists.txt`,
   from the bump right after the last branch cut (see [Versions](#versions)); if it doesn't, merge that bump first.
3. **Signed dry run on that exact SHA,** with every job green. If it fails: a fix PR, then a new candidate.
4. **Both lanes validate:**
   - Windows runs the smoke checks, Authenticode, and an installer install, upgrade and uninstall.
   - WSL installs the Linux packages on the supported distros.
5. **Manual checks** by the maintainer: UAC elevation, lock and virtual-desktop behaviour, and a real install of
   the installer.
6. **Go/no-go:** the maintainer approves the candidate on the tracker. After approval, nothing joins the scope. A
   new item restarts this stage.

**Done when:** the candidate commit is approved.

### 7. Release

1. Cut `release/X.Y` from the approved commit:

   ```bash
   git push origin <approved-sha>:refs/heads/release/X.Y
   ```

   The "Main Builds" ruleset requires status checks on `release/*`. Its **"Do not require status checks on
   creation"** setting must stay on: Dependency Review runs only on pull requests, so a branch cut from a main
   commit could never satisfy it, and the push would be refused (v0.10.0, 2026-11-03).

   Right after the cut, bump main to `X.(Y+1).0`: a PR that changes only `project(TaskSmack VERSION ...)` in
   `CMakeLists.txt`. From then on main's builds read `X.(Y+1).0-dev`, above every `X.Y.*` patch. Until that bump
   merges, the version guard shows a notice on main (see [Versions](#versions)).

2. Tag and push. Use an annotated tag; it isn't GPG-signed.

   ```bash
   git fetch origin
   git tag -a vX.Y.0 <approved-sha> -m "TaskSmack X.Y.0"
   git push origin vX.Y.0
   ```

3. Let the workflows run:
   - `release.yml` builds, signs, validates and publishes the release, titled "TaskSmack vX.Y.0".
   - `changelog.yml` opens CHANGELOG PRs against main and `release/X.Y`, and fills in the release body.
4. The maintainer merges both CHANGELOG PRs.
5. Post-publish verification by the Windows lane:
   - asset digests and `SHA256SUMS`;
   - cosign bundles and attestations;
   - Authenticode;
   - the download guide is present, and the notes aren't the placeholder.
6. Submit the installers to Microsoft SmartScreen.
   Check that `publish-winget` opened the winget-pkgs pull request (see [Publishing to WinGet](#publishing-to-winget)).
7. Close the milestone. Post a short announcement in GitHub Discussions: highlights, the download guide and how to
   verify.

### 8. Follow-up

Post a short retrospective comment on the tracker: what was late, what broke, and which step to change. Then plan
the next milestone.

## Feature freeze

The freeze starts when **every feature on the milestone is merged, moved to the next minor, or cut**, and the
maintainer posts "feature freeze" on the tracker. The lanes report readiness. Only the maintainer declares the
freeze.

- **Still merges:**
  - bug fixes and review findings;
  - performance and memory regressions from this cycle;
  - docs and tests;
  - release mechanics: version bumps, notes and pipeline fixes;
  - security updates.
- **Waits for the next minor:**
  - new features, and enhancements that change behaviour;
  - refactors a fix doesn't need;
  - dependency bumps that aren't security fixes.

  PRs for the next milestone stay open with auto-merge off until `release/X.Y` is cut.
- **Exceptions:** they need the maintainer's OK on the tracker. A feature that isn't finished slips to the next
  minor, or ships turned off.

## Late features

- **Default:** a feature proposed after stage 0 goes to the **next minor**.
- **Before the freeze:** the maintainer may pull it into this release.
- **After the freeze:** it needs an exception.
- **After the candidate is approved:** it can't join at all. It moves to the next minor, or to a patch release if
  it's a bug.

## Branches and tags

- **`main`:** all development lands here. It's frozen only from the feature freeze until the candidate is
  approved. The freeze is held by milestone; no repository variable is involved.
- **`release/X.Y`:** cut from the approved candidate.
  - It takes only release mechanics and fixes cherry-picked from main, upstream first, through the `backport:X.Y`
    label or a `/backport` comment.
  - Nothing is ever merged back from it.
  - It retires once the next minor ships, unless the maintainer keeps it for security fixes.
- **Who pushes tags:** only the maintainer pushes tags or creates release branches. Agents never do.
- **Versioning:** 0.x semver. A minor (`0.10.0`) carries features. A patch (`0.10.1`) carries fixes only, and is
  built from `release/0.10`.
- **Tags:** annotated, on the exact approved commit.
- **CHANGELOG:** generated by git-cliff through `changelog.yml`. Never edit it by hand.
- **Supply chain:**
  - Azure Artifact Signing (Authenticode);
  - cosign bundles and build-provenance attestations;
  - `SHA256SUMS`;
  - reproducibility checks;
  - actions pinned by SHA.

## Versions

`project(TaskSmack VERSION X.Y.Z)` in `CMakeLists.txt` is the only version (#3371).

- **Main reads the next minor.** It's bumped to `X.(Y+1).0` right after `release/X.Y` is cut (stage 7), so between
  releases it's already the version it will ship as.
- **Patch bumps stay on `release/X.Y`.** `X.Y.(N+1)` is merged there before its candidate, never on main.
- **The dev marker:** a build that isn't exactly a release says so. The shown version is `TASKSMACK_VERSION_FULL`
  (About, the log, the update check and its User-Agent):
  - Builds from `release.yml`, tag runs and dry runs alike, read exactly `X.Y.Z`. Every configure there passes
    `-DTASKSMACK_VERSION_SUFFIX=`.
  - A local or CI build reads `X.Y.Z-dev`, unless the tag `vX.Y.Z` points at its commit (`TASKSMACK_VERSION_SUFFIX`
    is `AUTO` by default).
  - `X.Y.Z-dev` orders above every earlier release and below `X.Y.Z`, as SemVer pre-releases do. So the update
    check never offers a dev build an older release, and offers `X.Y.Z` once it ships.
  - The Windows resource, manifest and installer version fields, and the package names, stay plain `X.Y.Z`: Windows
    needs numbers there.
- **The guard:** Pre-commit's "Version is not behind the latest release" job (`tools/check-version-ahead.py`) runs
  on every pull request and push to main and `release/*`, against the published `vX.Y.Z` tags:

  | Branch | Below the highest tag | Equal | Above |
  |---|---|---|---|
  | `main` (highest tag of all) | fails | passes with a notice: main's bump after the cut is due | passes |
  | `release/X.Y` (highest `vX.Y.*` tag) | fails | passes: the branch between patch releases | passes |

  A `release/X.Y` branch must read `X.Y.*`. It's compared only with its own line's tags, so `release/0.9` stays
  green after v0.10.0 ships.

## The Linux LLVM pin

The Linux release legs build with clang from apt.llvm.org. `LLVM_LINUX_RELEASE` in `release.yml` (`23.1.6`) is
the one place that pins it, for both architectures (#3393).

- **What's pinned:** the upstream release. Each leg fails at Setup LLVM unless its `clang-23` package version
  starts with `1:<release>~` and `clang-23 --version` reports that release. `lld-23` and `libc++` must be the same
  build as clang.
- **What isn't:** apt.llvm.org's snapshot inside the release (`~++<date>+<commit>-1~exp1~<build>`). It rebuilds
  the snapshot every few days and keeps only the newest, so pinning the whole build string blocked two releases on
  its own (#3307, #3392).
- **Where the snapshot shows:** every Linux leg records the exact build string of each LLVM package it installed in
  its `linux-build-inputs-<arch>` artifact. `verify-reproducibility` compares the two records and says when the two
  builds got different snapshots. That's informational: the package digests decide its result, and the job stays
  advisory.
- **The package cache:** Setup LLVM restores the release's `.deb`s from the Actions cache, keyed by release, OS
  image, architecture and package set. A warm run doesn't reach apt.llvm.org at all, and keeps installing the
  snapshot it saved after apt.llvm.org moves on. Release runs on `main` and `release/*` (dry runs) save it on a
  miss; a tag run only restores, from main's entry. So run a dry run from main before tagging, and both builds of
  the tag install the same snapshot.
- **When a bump is due:** apt.llvm.org's `llvm-toolchain-<codename>-23` follows LLVM's `release/23.x` branch, so
  it moves to each new point release too, and stops serving the old one. That's roughly every two weeks: it moved
  from 23.1.5 to 23.1.6 on 2026-11-03, between two jobs of this change's own dry run. A warm cache keeps building
  the pinned release regardless; a cold one (a new runner image, an evicted entry) fails at Setup LLVM until the
  release is bumped.
- **Retries:** apt.llvm.org's key, installer and repository each retry with backoff (4 tries), since runners have
  lost DNS to it for hours (2026-11-03).

To bump the release, when apt.llvm.org moves to a new one (Setup LLVM fails with "is not LLVM `<release>`") or on
purpose:

1. Check what it serves: `curl -fsSL
   https://apt.llvm.org/noble/dists/llvm-toolchain-noble-23/main/binary-amd64/Packages.gz | gunzip | grep -A2
   '^Package: clang-23$'`, and `binary-arm64` too.
2. Review the upstream release notes, then change `LLVM_LINUX_RELEASE` in a PR. The new key starts a new cache
   entry by itself.
3. Run a release dry run on the PR branch. Both Linux legs must pass. After the merge, run one from main, so the
   cache holds the new release before a tag needs it.

## Publishing to WinGet

TaskSmack is in the Windows Package Manager as `TaskSmack.TaskSmack` from v0.11.0 (#3486). Each version is
three manifests in [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs), under
`manifests/t/TaskSmack/TaskSmack/<version>/`, pointing at the release's two NSIS installers
(`TaskSmack-<version>-win64.exe` and `TaskSmack-<version>-windows-arm64.exe`) with their SHA-256. The templates
(schema 1.28.0, the newest winget-pkgs takes) are in
[`packaging/winget/`](https://github.com/mgradwohl/tasksmack/tree/main/packaging/winget), beside the other
packaging, with the reasons for the installer fields:
- per-machine, and `ElevationRequirement: elevationRequired`, since the installer always needs an administrator;
- `UpgradeBehavior: install`, because the installer upgrades in place and keeps Replace Task Manager and Start at
  sign-in (#3276);
- `AppsAndFeaturesEntries` matching the 32-bit installer's Uninstall key (`WOW6432Node\...\Uninstall\TaskSmack`,
  DisplayName `TaskSmack`, Publisher `TaskSmack Contributors`);
- no `InstallerSwitches` (winget's nullsoft defaults, `/S` and `/D=`, are the installer's) and no `Commands` (the
  installer doesn't put TaskSmack on `PATH`).

Change those templates when the installer changes. The automatic updates copy each version's manifests from the
one before, so a changed field also needs a hand-made update PR to winget-pkgs.

### The first submission (once, by hand, at v0.11.0)

An update needs the package to exist, so the maintainer submits v0.11.0's manifests under their own account, once
v0.11.0 is published. Get `wingetcreate` with `winget install Microsoft.WingetCreate`.

```powershell
gh release download v0.11.0 --repo mgradwohl/tasksmack --pattern SHA256SUMS --pattern SHA256SUMS.bundle --dir winget-0.11.0
# Optional: check SHA256SUMS first (docs/guide/user-guide.md, "Verifying a Release"):
cosign verify-blob --bundle winget-0.11.0/SHA256SUMS.bundle `
  --certificate-identity-regexp '^https://github\.com/mgradwohl/tasksmack/\.github/workflows/release\.yml@refs/tags/.+$' `
  --certificate-oidc-issuer https://token.actions.githubusercontent.com winget-0.11.0/SHA256SUMS
$date = gh release view v0.11.0 --repo mgradwohl/tasksmack --json publishedAt --jq '.publishedAt[:10]'
pwsh tools/winget-manifest.ps1 -Version 0.11.0 -ReleaseDir winget-0.11.0 -ReleaseDate $date
winget validate --manifest winget-0.11.0\winget\manifests\t\TaskSmack\TaskSmack\0.11.0
# Test it in Windows Sandbox now ("Testing in Windows Sandbox" below), then:
wingetcreate submit --prtitle "New package: TaskSmack.TaskSmack version 0.11.0" winget-0.11.0\winget\manifests\t\TaskSmack\TaskSmack\0.11.0
wingetcreate token --clear
```

The script takes the installers' hashes from `SHA256SUMS`, the Sigstore-signed list release.yml publishes, rather
than downloading them. The Sandbox test, and winget-pkgs' validation pipeline on the pull request, download both
installers and check them against those hashes, so a wrong hash fails before anything merges.

Called with no `--token`, `wingetcreate submit` signs you in to GitHub in the browser, forks winget-pkgs to your
account if needed and opens the pull request. `token --clear` then drops the sign-in it cached, so no token stays on
the machine. Fill in the pull request's checklist:
- sign the Microsoft CLA when its bot asks (once per account);
- confirm no other open pull request is for TaskSmack.TaskSmack 0.11.0, that the pull request holds this one
  version's three files, and that it passed `winget validate` and the Sandbox install;
- confirm it uses schema 1.28.

winget-pkgs' moderators and its validation pipeline review the pull request; answer their comments on it. Once it
merges, `winget install TaskSmack.TaskSmack` works, and every later Latest release is submitted automatically.

### Testing in Windows Sandbox

winget-pkgs asks for a local install test. Run it in Windows Sandbox, not on a working machine: it opens a Sandbox
window, so do it when nobody is using the desktop. It needs Windows Pro, Enterprise or Education on x64, with the
Sandbox feature on (`Enable-WindowsOptionalFeature -FeatureName Containers-DisposableClientVM -All -Online`, then
restart). SandboxTest.ps1 downloads WinGet into the Sandbox, validates the manifest, installs it there with
`winget install --manifest`, which downloads each installer and checks its SHA-256, and then runs `-Script`. Pin
the script to a winget-pkgs commit you've read; `1410e3eea47ed72934a257db21cf7a638a862b94` (2026-06-16) is the
one checked for this guide.

```powershell
$sandbox = (New-Item -ItemType Directory -Force "$env:TEMP\winget-sandbox").FullName
Invoke-WebRequest https://raw.githubusercontent.com/microsoft/winget-pkgs/1410e3eea47ed72934a257db21cf7a638a862b94/Tools/SandboxTest.ps1 -OutFile "$sandbox\SandboxTest.ps1"
# v0.10.0, the newest release today; for the real submission use v0.11.0's folder from above.
gh release download v0.10.0 --repo mgradwohl/tasksmack --pattern SHA256SUMS --dir "$sandbox\v0.10.0"
pwsh tools/winget-manifest.ps1 -Version 0.10.0 -ReleaseDir "$sandbox\v0.10.0" -ReleaseDate 2026-11-03
$manifest = "$sandbox\v0.10.0\winget\manifests\t\TaskSmack\TaskSmack\0.10.0"
Set-Location $sandbox
.\SandboxTest.ps1 -Manifest $manifest -MapFolder $sandbox -Script {
    $exe = "$env:ProgramFiles\TaskSmack\TaskSmack.exe"
    "Installed: $(Test-Path $exe)"
    winget list --id TaskSmack.TaskSmack --accept-source-agreements
    $app = Start-Process $exe -PassThru; Start-Sleep -Seconds 15
    "Running after 15 s: $(-not $app.HasExited)"; Stop-Process -Id $app.Id -ErrorAction SilentlyContinue
    winget uninstall --id TaskSmack.TaskSmack --silent --accept-source-agreements
    "Folder left behind: $(Test-Path "$env:ProgramFiles\TaskSmack")"
}
```

The expected result: the manifest validates, the install succeeds silently with no prompt in the Sandbox, and
`Installed: True`. `winget list` shows TaskSmack.TaskSmack at the manifest's version, which proves
`AppsAndFeaturesEntries` correlate. `Running after 15 s: True`, and the window shows TaskSmack. If it stops with an
OpenGL message instead, the Sandbox has no vGPU, which is the Sandbox's problem, not the manifest's. The uninstall
succeeds, and `Folder left behind: False`.

**The upgrade case**, once two releases with this installer exist: v0.10.0 is the first whose Uninstall key is
`TaskSmack` (#3276), and v0.9.x's was `TaskSmack 0.9.x`. So test with v0.10.0, then v0.11.0 once it's published and
before you submit it. Generate both folders as above, then:

```powershell
.\SandboxTest.ps1 -Manifest $manifest -MapFolder $sandbox -Script {
    winget upgrade --manifest "$env:USERPROFILE\Desktop\winget-sandbox\v0.11.0\winget\manifests\t\TaskSmack\TaskSmack\0.11.0" --silent --accept-source-agreements
    winget list --id TaskSmack.TaskSmack --accept-source-agreements
    (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\TaskSmack').DisplayVersion
}
```

(The Sandbox maps `$sandbox` to its desktop under the same folder name.) The expected result: the upgrade succeeds
silently, `winget list` shows one TaskSmack.TaskSmack at 0.11.0 with nothing to upgrade, and the Uninstall key's
DisplayVersion is 0.11.0: one entry, not two.

### Automatic updates

`release.yml`'s `publish-winget` job runs on a tag run after `verify-published-release` passes, for a release that
`create-release` made Latest. It skips a dry run, a pre-release and a patch to an older line (a v0.9.x after
v0.10.0), since each would make WinGet's newest version go backwards (#3277). It runs in the `release-signing`
environment, and in order it:

1. checks `WINGET_TOKEN` is set and belongs to `tasksmack-boop`. With no token, the job passes with a notice and
   submits nothing.
2. checks again that the release is published, not a pre-release, and still Latest, and takes its date for
   `ReleaseDate`.
3. checks that `TaskSmack.TaskSmack` is already in winget-pkgs, that this version isn't, and that no open pull
   request is for it. If any check fails, the job passes with a notice and submits nothing. That covers v0.11.0's
   own tag run, before the first submission merges, and a re-run after a successful submission. Only a confirmed
   HTTP 404 counts as "not there": a rate limit, a 5xx or no response is retried with backoff, then fails the job
   with GitHub's answer, so an outage never skips a release's submission.
4. downloads `wingetcreate.exe` at the pinned release (`WINGETCREATE_VERSION`) and checks its SHA-256
   (`WINGETCREATE_SHA256`).
5. runs `wingetcreate update TaskSmack.TaskSmack --version X.Y.Z --urls <x64 installer>|x64 <arm64 installer>|arm64
   --release-notes-url <release> --release-date <date> --prtitle "Update: TaskSmack.TaskSmack to X.Y.Z" --submit`.
   wingetcreate downloads both installers, hashes them, and copies every other field from the previous version's
   manifests. It then pushes a branch to `tasksmack-boop/winget-pkgs` and opens the pull request from it.
6. looks the pull request up on winget-pkgs (search, up to 6 tries 20 s apart) and links it in the step summary.
   Only an open or merged pull request counts, not an earlier rejected one. If none turns up, the job fails.

The pull request shows as `tasksmack-boop`, the machine account whose address is `tasksmack@uppercutwoodworks.com`
(#3493); the manifests have no e-mail field. The account's first pull request gets `Needs-CLA` until someone
signs the Microsoft CLA as `tasksmack-boop`. Nobody needs to act on later ones unless their validation fails or a
moderator asks something.

`winget-health.yml` follows the pull request to the merge. Once a day it checks that the newest Latest release's
folder is on winget-pkgs' default branch. If it isn't, it keeps one issue open, titled "WinGet: TaskSmack.TaskSmack
<version> isn't published" and labelled `release` and `agent:Windows` (found by that title; never `ci-red-main`,
which means main is red and holds merges), while any of these hold:
- the pull request carries a label that needs us (any `*Error*` or `*Failed*` label except `Waived-*`, or
  `Needs-Author-Feedback`, `Needs-CLA`, `Changes-Requested`, `Blocking-Issue` and the like);
- it was closed without merging, or none exists a day after the release;
- 7 days have passed.

It closes that issue once the version is there, and does nothing before the first submission. As in the job,
only a 404 means "not submitted yet". Any other failed call, the search included, fails the check instead.

If the job fails after the release is out, fix the cause and re-run the job, or submit that version by hand with
`wingetcreate update ... --submit` as above. wingetcreate is used rather than `vedantmgoyal9/winget-releaser`:
that action can be pinned by commit, but it installs `cargo-binstall@main` and the newest Komac at run time, so
code no pin covers would hold the token. To move to a newer wingetcreate, change both `WINGETCREATE_*` values in
one reviewed PR. Take the SHA-256 from the release's asset digest:
`gh api repos/microsoft/winget-create/releases/tags/<tag> --jq '.assets[] | select(.name == "wingetcreate.exe") | .digest'`.

### The token

`WINGET_TOKEN` is a classic personal access token of the `tasksmack-boop` account with only the `public_repo`
scope. It's stored in the `release-signing` environment, so only jobs that may enter that environment can read
it. The WinGet submission tools document a classic token for the pull request from the fork into
microsoft/winget-pkgs, which belongs to another owner. A leaked token reaches only that account's public
repositories, which hold nothing but the winget-pkgs fork. The current token expires on **2027-11-04**; rotate it
before then:

1. Sign in as `tasksmack-boop` and create a new classic token: scope `public_repo` only, expiry one year.
2. In this repository's **Settings → Environments → release-signing**, replace the `WINGET_TOKEN` secret's value.
3. Delete the old token on `tasksmack-boop`, update the expiry date above in a PR, and set a reminder a few weeks
   before the new one.

A missing or expired token doesn't fail a release. An empty secret skips the job, and an expired one fails only
`publish-winget`, after the release is published. To stop the automatic updates, delete the secret.
## Patch releases

A patch happens only when a shipped bug warrants one.

1. Fix on main and add the `backport:X.Y` label. If the automatic cherry-pick conflicts, open a hand cherry-pick PR
   (`git cherry-pick -x`).
2. Review the `vX.Y.(N-1)..release/X.Y` diff only.
3. Merge the version bump on `release/X.Y`. Run stage 6 on its head (soak, signed dry run, validation, approval),
   then stage 7. A patch gets no Discussions announcement.

## Who does what

| Role | Responsibilities |
|---|---|
| Maintainer | scope and late-feature calls; declares the freeze; approves the candidate; manual checks; cuts the release branch; pushes the tag; merges the CHANGELOG PRs; SmartScreen submission; announcement |
| Windows lane | Windows code and its review sweep; CI and the release pipeline; dry runs; Windows validation; post-publish verification; Windows screenshots; performance baselines |
| WSL lane | Linux code and its review sweep; the helper and broker; Linux packaging; Linux validation; the docs site; Linux screenshots |

## Why these steps

Each step answers something that went wrong in v0.7.0 to v0.9.1.

| What happened | Release | The step that prevents it |
|---|---|---|
| Tag runs failed, and v0.5.0, v0.6.1 and v0.7.0 shipped with no assets. | 0.5–0.7 | Signed dry runs in stages 1 and 6. |
| The first candidate failed its dry run on two bugs that didn't ship (#2784, #2785). | 0.9.0 | Stage 1's dry run finds these weeks earlier. |
| The release body was too large for `--generate-notes` (#2801), and a token lacked a scope. | 0.9.0 | Stage 1's dry run, and stage 0's lead-time list. |
| The release notes stayed a placeholder for about 9 hours. | 0.9.1 | Generated automatically (#3157), then checked in stage 7. |
| Signing slipped a whole release when a vendor rejected the application. | 0.9.0 | Stage 0's lead-time items. |
| The installer joined the scope after the candidate, adding about 17 hours. | 0.9.1 | Nothing joins after the candidate is approved. |
| A sanitizer run finished after the tag was pushed. | 0.9.1 | Stage 6: tag only on a fully green candidate. |
| `git tag -s` failed because there's no GPG key. | 0.9.1 | Stage 7 gives the exact `-a` command. |
| Jobs hit their timeouts under release load (#2798, #3052, #3148, #3171). | 0.9.x–0.10 | Stage 1's headroom check. |
| Feature merges pushed coverage under the floor (#3186). | 0.10 | Stage 3: a coverage figure in large PRs. |
| Memory growth showed up only in a long run (#3215). | 0.10 | Stage 1 baselines, compared again in stage 4. |
| Eight backports were cherry-picked by hand during hardening. | 0.9.1 | Main freezes until approval, so hardening needs no backports. |
| The SmartScreen submission and the CHANGELOG merges happened ad hoc. | 0.9.1 | Both are listed in stage 7. |
| Main read 0.9.0 for weeks (#2102 to #3370), below the shipped v0.9.1, so its builds looked like v0.9.0 and were offered "v0.9.1 available" (#3371). | 0.10 | Main is bumped right after each branch cut, non-release builds read `-dev`, and the version guard fails below a published tag. |

## Sources

- Karl Fogel, [*Producing Open Source Software*](https://producingoss.com/en/producingoss.html), chapter 7.
- [OpenStack project team guide: release management](https://opendev.org/openstack/project-team-guide/src/commit/647bcbdc1cc6456a09a9580b58e51a32fbe7f567/doc/source/release-management.rst).
- [RIOT: Managing a release](https://doc.riot-os.org/misc/managing-a-release/).
- [FOSSHub: release management for maintainers](https://www.fosshub.com/resources/maintainers/release-management/).
- [Apache BookKeeper: releases](https://bookkeeper.apache.org/community/releases).
- [Trunk-based development: branch for release](https://trunkbaseddevelopment.com/branch-for-release).
- [Drupal backport policy](https://drupal.org/node/767608).
- [Microsoft Release Flow](https://devblogs.microsoft.com/devops/?p=42595).
- [OpenSSF Scorecard](https://github.com/ossf/scorecard).
