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

- **The milestone and a release tracker issue.** Open them first; for example, #3223 for v0.10.0. Title the
  tracker `[RELEASE X.Y] vX.Y.0 release tracker` and put it on the milestone: [the release
  dashboard](#the-release-dashboard) finds it by that title.
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
- **The newcomer check on both platforms:** each lane follows the documented contributor path on a clean machine,
  from `git clone` to a first build, tests and benchmarks. Its `results.md` goes on the tracker, and each FINDING or
  FAIL becomes a cycle issue ([The newcomer check](#the-newcomer-check)). Budget a night for each: the Linux run takes
  an hour or more, and the Windows Sandbox run takes hours.

**Done when:** main is green, the signed dry run passes, the baselines are posted, and both newcomer checks are on the
tracker.

### 2. Fixes: refactors, bugs, test gaps

Take the work in the lanes' queue order ([labels.md](labels.md), "Queue order"): priority within each step, then severity among bugs. A test gap that guards a fix belongs with that fix, not at the end, because a test written before a refactor has to be redone.
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
- **Verifying a download, for users:** the user guide's "Verifying a Release" section covers it: the Authenticode signer,
  `SHA256SUMS`, cosign bundles and build-provenance attestations. Check it is current for the new features.

**Done when:** the maintainer has read the notes draft and the docs PRs have merged.

### 6. Candidate and validation

1. **Soak:** main green for 24 hours, with every required push check and the nightly jobs passing.
2. **Candidate:** one exact commit proposed on the tracker. Main already reads `VERSION X.Y.0` in `CMakeLists.txt`,
   from the bump right after the last branch cut (see [Versions](#versions)); if it doesn't, merge that bump first.
3. **Signed dry run on that exact SHA,** with every job green. If it fails: a fix PR, then a new candidate.
4. **Both lanes validate:**
   - Windows runs the smoke checks, Authenticode, and an installer install, upgrade and uninstall, plus an upgrade
     and an uninstall while TaskSmack.exe is in use, which must exit with 32 and change nothing (#4436). The
     install, run and uninstall part is `pwsh tools/sandbox-install-check.ps1 -Version X.Y.Z` (or `-Installer
     <path>` for a candidate's build): it writes the Sandbox file, starts the Sandbox, waits, closes it and prints
     `results.md` with a `RESULT: PASS` or `RESULT: FAIL` line. It needs no typing in the Sandbox. Run it when
     nobody is using the desktop, never from CI. `-WinGet` runs it through `SandboxTest.ps1` and winget (see
     "Testing in Windows Sandbox"). The Sandbox enforces Smart App Control, which GitHub's runners don't: it is
     the check that the installer's NSIS plugins load, which needs them signed (`sign-nsis-plugins`, #4864).
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
   creation"** setting must stay on: Dependency Review runs only on pull requests and merge groups, so a branch
   cut from a main commit could never satisfy it, and the push would be refused (v0.10.0, 2026-11-03).

   The ruleset's **Require linear history** also refuses the cut (GH013, "This branch must not contain merge
   commits"), because main's history keeps merge commits from before squash-only merging, such as `e5537d15`
   (#502). Before the push, the maintainer adds **Repository admin, Always allow** to the ruleset's bypass list,
   then removes it right after the push (v0.11.0, 2026-11-23). Pushes to the existing branch are checked only
   for their new commits, so backports pass without the bypass.

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
5. Post-publish verification by the Windows lane: `pwsh tools/release.ps1 -Step Verify -Version X.Y.Z`. It
   downloads the release and checks:
   - asset digests and `SHA256SUMS`;
   - the build-provenance attestation of every package, and the cosign bundle when `cosign` is installed;
   - Authenticode on both installers and on the exes in both zips;
   - the notes aren't the placeholder.

   It prints a Markdown summary to post on the tracker. `-Step Status` shows the release run, the CHANGELOG PRs,
   the winget-pkgs PR, the WinGet health issue and the milestone's open count.
6. The maintainer submits the installers to Microsoft SmartScreen (see [SmartScreen submission](#smartscreen-submission)).
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

## The release dashboard

<https://mgradwohl.github.io/tasksmack/release/> shows, for one release, what still has to happen before it
ships, where each of those things stands and how fast it's moving (#4524). It opens on the release in
progress; the picker at the top shows any earlier or planned one, and `?m=v0.10.0` links to one directly.

It keeps nothing of its own. Every figure is read from what this process already keeps, so a wrong figure is
fixed at its source:

| The page shows | It reads |
|---|---|
| Scope, and what's open | the milestone's issues. The tracker itself isn't counted, and neither are pull requests. GitHub's milestone page counts both, so it reads higher: by one while the tracker is open, and by one more for each pull request on the milestone. The page says so under the count. |
| The open count by lane, and "Open issues" listed by lane | each open issue's `agent:*` labels: one label is that lane, two or more are "Both lanes". The figures add up to the open count, and each lane's links to the same search on GitHub ("No lane" has no link: its search would list the tracker). A lane's list runs in the lanes' queue order ([labels.md](labels.md), "Queue order") (#4585), and ends with a group of its own, "Waiting on the maintainer", for the issues the lane can't take (#4718). |
| In progress, in review | the `in progress` label or a draft pull request; a pull request that's ready for review |
| "No high-severity bug open" | open `sev:high` and `sev:medium` issues on the milestone |
| "Every feature merged, moved or cut" | open `enhancement` issues that aren't CI, tooling or security work |
| "Feature freeze declared" | the first one-line tracker comment that declares the feature freeze |
| "Code-review findings closed" | the milestone's `code-review` issues |
| "Performance issues closed" | the milestone's `performance` issues: stage 4's measure against the baselines files its regressions there |
| "Tracker checklist" | the task list in the tracker's body |
| "Nothing waiting on the maintainer" | open issues labelled `question` or `manual close`. Listed by lane, they come after the lane's own work in a "Waiting on the maintainer" group, and the lane's count says how many (#4718). Under that heading and under this row the issues don't repeat it; anywhere else one is listed, it carries a "Waiting on the maintainer" chip. |
| "main green for 24 hours" | `main-health.yml`'s `ci-red-main` issues: each one's open time is a red period. The workflow list under it shows each watched workflow's latest push run and latest nightly run, as `main-health.yml` judges them |
| "Pull requests merged" | the open pull requests on the milestone or closing one of its issues: how long each has been ready, and what it's waiting on (failing checks, a conflict, open review threads, a catch-up with its base, running checks) |
| "Signed release dry run on main" | `release.yml`'s latest manual run on main, and how many commits main is past it |
| "Candidate approved" | the first one-line tracker comment after the freeze that approves the candidate or release |
| "Release published" | the GitHub release tagged with the milestone's title |

What that asks of the tracker:

- **Start a checklist item with its issue number** (`- [ ] #3378 Update dependencies`) and the page ticks it when
  that issue closes, so the box doesn't need ticking by hand. An item with no leading number is as its box says.
- **Declare the freeze and approve the candidate in one line each,** as on #3223: "I declare the feature freeze",
  "Approved for v0.10.0 release". The page links the comment it read. What it takes for each:
  - the freeze: a line that says "feature freeze" and "declare" or "declared", or that opens with "Feature freeze";
  - the approval: a line after the freeze that says "approved" and names the candidate, the release or the version;
  - neither: a longer comment (the lanes' readiness reports), a line that hedges ("ready for the freeze?"), a line
    that names an issue or an exception ("Approved: #4600 exception"), or a comment from someone who can't write
    to the repository.
- **A due date on the milestone** is the release's goal. The page shows it under "Released" beside the date it
  really shipped, says what net closing rate the goal asks for, and marks it on the burn-up chart.

Three figures need a word on what they measure:

- **Ready to merged** is the wait a finished change has: from when its pull request was last marked ready for
  review, or opened if it never was a draft, to its merge.
- **Pull request CI** counts `ci.yml`'s pull request runs in the cycle, passed and failed, and divides them by
  the pull requests merged in that time: the CI runs the cycle spent for each merge. The runs are every pull
  request's, including those still open, so pull requests held through a freeze raise it, and the comparison
  shows it as context, not as better or worse. A cancelled run was superseded by a push and isn't counted.
  It's the whole repository's figure, so a patch release has none.
- **The open work, by where it stood** stacks the open issues as not started, in progress and in review over the
  cycle. Before the `in progress` label (2026-11-19) an issue shows as started only once it had a pull request.

The pace table is arithmetic on the last 1, 3 and 7 days, not a forecast. A review sweep that files a hundred
findings in a day turns the net negative until they're worked off.

The comparison table sets each release against the one before it on its line: a minor against the minor before,
a patch against the release before it on its branch. A cycle runs from that earlier release's publication to its
own. "Work started" there is the first linked pull request's open time for every release, since releases before
2026-11-19 have no `in progress` label.

How it's made:

- `tools/release-dashboard.py` reads the API and writes one JSON file per `vX.Y.Z` milestone, plus an index. A
  shipped release is written once and reused.
- `.github/workflows/release-dashboard.yml` runs it and publishes the files to the `dashboard-data` branch, which
  holds that one commit and nothing else. A run starts when an issue closes, reopens or changes milestone (a new
  issue on a milestone is one of those), when `main` moves, when Main Health opens or closes a `ci-red-main`
  issue, and on a schedule (#4604, #4700). GitHub starts scheduled runs late and skips some when it's busy, so
  one schedule line alone left the page up to 37 minutes behind: there are two, five minutes apart. Both
  lines' first slots of an hour still come about ten minutes late together, so Main Health also starts a run
  when the dashboard has none going and none has finished for six minutes (#4739). Main Health runs after
  each watched workflow on `main` finishes, so this shortens the gap while `main` is busy and leaves it in a
  quiet hour. A run that an issue, a push or the schedule started waits until the published data is five minutes
  old; Main Health's and a person's don't. Labels, edits, comments and pull request events start none (#4620):
  every event is a run, GitHub lists each one that wasn't needed as cancelled, and the next run reads them
  anyway. So a new label can take until the next run to show.
- A run stops before it reads anything when it has nothing to add (#4620): the published figures were read
  after the run was created (the second run of a merge, which is a push and an issue closing, and a run that
  waited behind another), or the workflow token's hourly budget is used up, before the run's wait or during it
  (#4717). That last one is a warning on the run, not a failure, and it stops a manual run too, so "Refresh
  now" publishes nothing new then. Every workflow here shares the budget (#4617), and the figures stay as they
  were until the hour turns over.
- When the budget is used up, the page says so and when it returns (#4726): "GitHub's API budget for this
  repository is used up until 1:18 PM", in place of the warning about old data. The run that finds the budget
  gone publishes the last data again as it is, with that time in `index.json`'s `budget_until`, which takes no
  API call. It does that once for each hour the budget is gone, and the next run that reads writes an index
  without the field. `generated_at` doesn't change, so "Updated" still gives the time the figures were read.
  While the note shows, the page offers no "Refresh now": a run could add nothing. The note's commit on
  `dashboard-data` keeps the time of the data it repeats, and its message and README say the figures are an
  earlier run's. With both budgets gone the time is the later of the two hours' ends. A note that can't be
  written, or that names a time over 75 minutes ahead, is a warning on the run and nothing is published.
- A run on `main` makes no artifact (#4717): its read-only job hands the JSON to the job that publishes as a
  job output. That job takes it as text that can't become commands, checks it's a flat set of JSON files
  before pushing, and tries the push three times. A run that can't publish (a pull request's, or a manual one
  on a branch) keeps its JSON as an artifact instead (`release-dashboard-data`, three days) for the reviewer.
- To refresh now: "Refresh now" on the page opens the workflow on GitHub, where "Run workflow" starts a run, or
  `gh workflow run release-dashboard.yml`. The page can't start one itself, since that takes a token. A manual
  run doesn't queue: it cancels the run in progress and publishes what it reads. New data shows within about
  five minutes: the data host can serve the old file that long.
- `docs/release/` is the page: static, no library, no CDN. It asks for new data every five minutes, and when the
  tab comes back to the front. If the last run was more than 25 minutes ago it says so, unless the budget note
  above already gives the reason. A planned release's own figures are up to an hour old by design, and a
  shipped release's never change, so neither has "Refresh now".
- A late page says why (#4734). When its figures are over ten minutes old it asks GitHub's public API, without
  a token. First for the published `index.json`, past the data host's five-minute cache: when that is newer
  than the page's, only the page's copy is old, so it asks for the data again and says nothing (#4741: the
  run list can't tell a run that published from one that only checked and stopped). Otherwise for the
  workflow's newest runs, and it says one of four things: a refresh has been waiting for a runner since a
  given time (other workflows hold them all, and a manual run would only cancel it, so the page offers none),
  a refresh is running, the last refresh failed at a given time (with a link to the runs), or nothing has been
  published since the last run and none is running (then "Refresh now" helps). It says nothing when the API
  can't be read. That limit is 60 requests an hour for each visitor's address: the page asks at most every
  five minutes, two requests at the most each time, only while it is late and on screen, and after a refusal
  not until the limit resets. The answer is about the published figures, so it stands while another release
  is looked at, and falls when new figures arrive. A pull request's run isn't counted.
- The workflow's token shares one hourly API budget with every other workflow here, so the generator reads
  once an hour what changes slowly (the cycle's pass counts, a planned release). Each run's log says what it
  used and what's left, and warns under a quarter.
- To look at the data locally: `python -I -X utf8 tools/release-dashboard.py --out <folder>` (a read-only token
  from `gh auth login` is enough).

Coverage stays on Codecov, and the page links to it.

## SmartScreen submission

Microsoft SmartScreen warns about a new, signed installer until it has seen enough downloads of it. The maintainer
tells Microsoft it's a good file, once per release, after the release is published and verified. One submission is
one file, so send both installers.

1. Open the file submission portal, <https://www.microsoft.com/en-us/wdsi/filesubmission>, and sign in.
2. Choose **Software developer**.
3. For the product, choose **Microsoft Defender SmartScreen**.
4. Company: `TaskSmack Contributors`.
5. Upload the file from the release's assets: `TaskSmack-X.Y.Z-win64.exe`, then do it all again for
   `TaskSmack-X.Y.Z-windows-arm64.exe`. Optionally also send `TaskSmack.exe` from each zip.
6. Choose **Incorrectly detected** (the file is clean).
7. For the detection, enter `SmartScreen: Windows protected your PC (unrecognized app)`.
8. Paste this into the additional information, with the file's own name and hash:

   ```text
   TaskSmack X.Y.Z, an open-source system monitor for Windows. This is the file
   TaskSmack-X.Y.Z-win64.exe from https://github.com/mgradwohl/tasksmack/releases/tag/vX.Y.Z.
   It is signed with Azure Artifact Signing, certificate subject CN=Matt Gradwohl, and carries
   a build-provenance attestation. SHA-256: <the file's line in SHA256SUMS>
   SmartScreen shows "Windows protected your PC" for it as an unrecognized app. Please
   mark it as not malicious.
   ```

The SHA-256 comes from the release's `SHA256SUMS` (the line for that file name), which `release.ps1 -Step Verify`
has just checked against the downloaded file. The portal's labels can differ a little from the ones here; pick the
nearest. Microsoft answers by e-mail. Note the submission on the tracker.

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

## The newcomer check

Nothing else tests the path a new contributor takes: clone, `setup-dev`, `check-prereqs`, the first build, the tests
and the benchmarks. Release validation in stage 6 checks what ships, and the dev-tool tests check the scripts' logic
without provisioning anything. `tools/newcomer-check.sh` and `tools/newcomer-check.ps1` (#2618) follow README.md and
CONTRIBUTING.md on a machine with nothing installed. Before each step they check that the docs still give the command
they're about to run. They record every step as PASS, FINDING, FAIL or SKIP in `results.md`. Six provisioning bugs
filed in one day during v0.11.0 (#4704 to #4708, #4711) were in exactly this path, and no other check could see them
(#4713).

**When it runs** (maintainer, 2026-11-21):
- **Stage 1 of every cycle,** on both platforms.
- **On every toolchain bump:** LLVM ([the Linux LLVM pin](#the-linux-llvm-pin), or the Windows LLVM version), the CMake
  minimum, the Ubuntu base, or the Windows SDK. The lane whose platform the bump touches runs it, or both lanes when it
  touches both.
- **A patch release** runs it only when the patch bumps a toolchain.

**Where the results go:**
- **The stage 1 runs:** `results.md` goes on a comment on the cycle's release tracker, from a checklist item that
  starts with the cycle's newcomer-check issue number, so [the release dashboard](#the-release-dashboard) ticks it.
- **A toolchain bump's run:** `results.md` goes on the bump's PR, before it merges.
- **Each FINDING or FAIL** becomes an ordinary issue on the cycle's milestone, with the step's log attached. A FAIL is
  a doc that no longer matches the script, or the other way round. A FINDING is a step that breaks as written.

**Windows (agent:Windows):**
- **Where:** in Windows Sandbox, which starts clean and is thrown away when it closes.
- **The `.wsb`:**
  - maps a folder holding only `tools/newcomer-check.ps1`, read-only, to `C:\newcomer-in`;
  - maps an empty writable folder to `C:\newcomer-out`;
  - sets `Networking` on, `MemoryInMB` 8192 and `VGpu` Disable.
- **Its logon command:** the one in the script's `.EXAMPLE`, which runs it with
  `-WindowsSandbox -ContinueOnFinding -SetupTimeoutMinutes 360 -ShutdownWhenDone`.
- **Watching it:** follow `progress.log` in the output folder on the host, and wait for `DONE`.
- **How long:** hours, not minutes. Windows Installer verifies each signed `.msi` in the Sandbox, and the Windows SDK
  installs 48. Little CPU shows meanwhile.
- **When:** run it overnight, outside the maintainer's day. It needs the Sandbox's 8 GB beside the WSL lane's VM, so
  the WSL lane keeps to `-j 2` and no benchmarks while it runs. Agree the night on #4135.

**Linux (agent:WSL):**
- **Where:** in a fresh WSL distro of the current Ubuntu base (26.04; `newcomer-check.sh` was retargeted from 24.04
  in #4712), with none of TaskSmack's tools installed: `wsl --install Ubuntu-26.04 --name tasksmack-newcomer`.
- **How:** copy only the script into the distro and run it there. It clones the repository itself, asks for sudo once,
  and does several full builds, so expect an hour or more.
- **When:** in the WSL lane's window, on a night agreed on #4135, so that it doesn't overlap the Windows run.

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
  the tag install the same snapshot. GitHub evicts an entry no run has read for 7 days, so `llvm-cache-warm.yml`
  restores it on both architectures every Monday and Thursday (#3825). When it finds the entry gone, it fails,
  and a dry run from main saves it again.
- **Every other Linux job (#3635):** Setup LLVM caches their `.deb`s too, keyed by LLVM major, OS image,
  architecture, package set and ISO week. The week's first run on `main` refreshes from apt.llvm.org and saves;
  later runs install from the cache. A run that finds only an earlier week's entry tries apt.llvm.org for a fresh
  one and, if it's down, installs the older entry with a warning. Only a job with no entry at all needs
  apt.llvm.org up. `cache-prune.yml` keeps each set's newest week.
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
4. Run [the newcomer check](#the-newcomer-check) on Linux against the PR branch (`--ref`), and put its `results.md` on
   the PR before it merges. A bump of the LLVM major also runs it on Windows.

## Publishing to WinGet

### WinGet: what the maintainer does

- **Per release:** nothing. `publish-winget` opens the winget-pkgs pull request after `verify-published-release`
  passes, and `winget-health.yml` follows it to the merge.
- **Once, at v0.11.1:** the first automatic pull request comes from `tasksmack-boop` and waits at `Needs-CLA`.
  Sign the Microsoft CLA as `tasksmack-boop` when the bot asks on that pull request. Later ones need nothing.
  `release.ps1 -Step Status -Version 0.11.1` shows that pull request.
- **Which path a release is on:** `pwsh tools/release.ps1 -Step WinGet -Version X.Y.Z [-Rerun]` reads winget-pkgs with the
  checks `publish-winget` makes and prints one of three. **A:** the package is in winget-pkgs; it links the version's pull
  request with its labels, and says to sign the CLA as `tasksmack-boop` when `Needs-CLA` is on it. **B:** the package
  isn't there yet, and it names the open first submission. **C:** the first submission closed unmerged, and it prints the
  hand-submission commands. With `-Rerun`, and only on path A when the version isn't in winget-pkgs and no pull
  request for it is open, it re-runs the `Submit the WinGet Update` job of the `vX.Y.Z` tag run, waits, and prints
  the pull request it opened. In any other state it says why it won't and re-runs nothing. If a release's tag run
  skipped the job because the first submission was still open, run it with `-Rerun` once that merges.
- **If the health issue opens** ("WinGet: TaskSmack.TaskSmack <version> isn't published"): open the pull request it
  links and read the label that needs us (see [Automatic updates](#automatic-updates) for which ones count). Fix
  what it names, re-run the failed `publish-winget` job, or submit that version by hand as in the first
  submission below. The issue closes itself once the version is on winget-pkgs.

The rest of this section is the reference behind those lines.

TaskSmack is in the Windows Package Manager as `TaskSmack.TaskSmack` from v0.11.0 (#3486). Each version is
three manifests in [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs), under
`manifests/t/TaskSmack/TaskSmack/<version>/`, pointing at the release's two NSIS installers
(`TaskSmack-<version>-win64.exe` and `TaskSmack-<version>-windows-arm64.exe`) with their SHA-256. The templates
are in [`packaging/winget/`](https://github.com/mgradwohl/tasksmack/tree/main/packaging/winget), beside the other
packaging, with the reasons for the installer fields:
- schema **1.12.0**, not the newest, 1.28.0. wingetcreate v1.12.13.0, the newest release and the one that submits
  every update, reads, validates (with its bundled WinGetUtil 1.12.350) and writes 1.12.0, and `update` rewrites
  every manifest to it. It would refuse 1.28.0 headers. winget-pkgs recommends 1.28.0 and accepts 1.12.0 ("1.12.0 is
  also accepted", its `doc/ValidationFailureGuide.md`). `WINGETCREATE_MANIFEST_VERSION` in `release.yml` names the
  version, beside the pin; `tools/winget-manifest.ps1` refuses templates on any other, and `publish-winget` checks
  what wingetcreate wrote before submitting. Move all of them to 1.28.0 together with a wingetcreate release that
  writes it (its main branch already does);
- per-machine, and `ElevationRequirement: elevationRequired`, since the installer always needs an administrator;
- `UpgradeBehavior: install`, because the installer upgrades in place and keeps Replace Task Manager and Start at
  sign-in (#3276);
- `AppsAndFeaturesEntries` matching the 32-bit installer's Uninstall key (`WOW6432Node\...\Uninstall\TaskSmack`,
  DisplayName `TaskSmack`, Publisher `TaskSmack Contributors`);
- no `InstallerSwitches` (winget's nullsoft defaults, `/S` and `/D=`, are the installer's) and no `Commands` (the
  installer doesn't put TaskSmack on `PATH`).
- `ExpectedReturnCodes` mapping 32 to `packageInUse` (#4436): the installer and its uninstaller exit with 32, before
  they change anything, while TaskSmack.exe or TaskSmackHelper.exe in the folder is running, in any session
  (`TASKSMACK_IN_USE_EXIT_CODE` in `cmake/CPackProjectConfig.cmake.in`). winget then says the application is running
  rather than report a failed install. `tools/check-winget-manifests.ps1` refuses a manifest without it.

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
- for the schema item (the template asks for 1.28), say it's 1.12.0, which winget-pkgs accepts, because that's what
  wingetcreate validates and submits.

winget-pkgs' moderators and its validation pipeline review the pull request; answer their comments on it. Once it
merges, `winget install TaskSmack.TaskSmack` works, and every later Latest release is submitted automatically.

### Testing in Windows Sandbox

winget-pkgs asks for a local install test. Run it in Windows Sandbox, not on a working machine: it opens a Sandbox
window, so do it when nobody is using the desktop. It needs Windows Pro, Enterprise or Education on x64, with the
Sandbox feature on (`Enable-WindowsOptionalFeature -FeatureName Containers-DisposableClientVM -All -Online`, then
restart). SandboxTest.ps1 downloads WinGet into the Sandbox and validates the manifest. It then installs the
manifest there with `winget install --manifest`, which downloads each installer and checks its SHA-256, and runs
`-Script`. Pin the script to a winget-pkgs commit you've read; `1410e3eea47ed72934a257db21cf7a638a862b94`
(2026-06-16) is the one checked for this guide.

`pwsh tools/sandbox-install-check.ps1 -Version X.Y.Z -WinGet` does the first check below with no typing: it
downloads that pinned `SandboxTest.ps1`, writes the release's manifests with `tools/winget-manifest.ps1`, and runs
a check script in the Sandbox through `-Script`. `SandboxTest.ps1` has no confirmation prompt, so it runs
unattended, and the script waits for the result file, closes the Sandbox and prints it. The commands below are
what it replaces, and the upgrade and by-ID checks still use them.

**Before publication, check by what's installed, not by package ID.** Until `TaskSmack.TaskSmack` is merged into
winget-pkgs, a copy installed from a local manifest isn't tied to any source. So
`winget list --id TaskSmack.TaskSmack`, `winget uninstall --id ...` and `winget upgrade --id ...` find nothing, even
when the install is right. The checks below read the Uninstall (ARP) entry, which is
`HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\TaskSmack` with its `DisplayName`,
`DisplayVersion` and `Publisher`, plus the install folder and `winget list --name TaskSmack`. They match by name,
the way winget's own correlation does through `AppsAndFeaturesEntries`. The by-ID checks are at the end, for after
publication.

```powershell
$sandbox = (New-Item -ItemType Directory -Force "$env:TEMP\winget-sandbox").FullName
Invoke-WebRequest https://raw.githubusercontent.com/microsoft/winget-pkgs/1410e3eea47ed72934a257db21cf7a638a862b94/Tools/SandboxTest.ps1 -OutFile "$sandbox\SandboxTest.ps1"
# v0.10.0, the newest release today; for the real submission use v0.11.0's folder from above.
gh release download v0.10.0 --repo mgradwohl/tasksmack --pattern SHA256SUMS --dir "$sandbox\v0.10.0"
pwsh tools/winget-manifest.ps1 -Version 0.10.0 -ReleaseDir "$sandbox\v0.10.0" -ReleaseDate 2026-11-03
$manifest = "$sandbox\v0.10.0\winget\manifests\t\TaskSmack\TaskSmack\0.10.0"
Set-Location $sandbox
.\SandboxTest.ps1 -Manifest $manifest -MapFolder $sandbox -Script {
    $folder = "$env:ProgramFiles\TaskSmack"
    $arp = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    function Get-TaskSmackArp { @(Get-ChildItem $arp | Get-ItemProperty | Where-Object DisplayName -Like 'TaskSmack*' |
        Select-Object PSChildName, DisplayName, DisplayVersion, Publisher) }
    "Installed: $(Test-Path "$folder\TaskSmack.exe")"
    Get-TaskSmackArp | Format-Table -AutoSize
    winget list --name TaskSmack --accept-source-agreements
    $app = Start-Process "$folder\TaskSmack.exe" -PassThru; Start-Sleep -Seconds 15
    "Running after 15 s: $(-not $app.HasExited)"; Stop-Process -Id $app.Id -ErrorAction SilentlyContinue
    winget uninstall --name TaskSmack --exact --silent --accept-source-agreements
    # The NSIS uninstaller finishes from a copy of itself, so wait for the folder to go.
    $deadline = (Get-Date).AddSeconds(60)
    while ((Test-Path $folder) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
    "Folder left behind: $(Test-Path $folder)"
    "Uninstall entries left: $((Get-TaskSmackArp).Count)"
}
```

The expected result:
- The manifest validates, and the install succeeds silently with no prompt in the Sandbox: `Installed: True`.
- The table shows exactly one entry: `PSChildName` `TaskSmack`, `DisplayName` `TaskSmack`, `DisplayVersion` `0.10.0`
  and `Publisher` `TaskSmack Contributors`. Those are the values the manifest's `AppsAndFeaturesEntries` and
  `ProductCode` name.
- `winget list --name TaskSmack` shows TaskSmack 0.10.0 with a local ID (`ARP\Machine\X86\TaskSmack`) and no source.
  That's expected before publication.
- `Running after 15 s: True`, and the window shows TaskSmack. If it stops with an OpenGL message instead, the Sandbox
  has no vGPU, which is the Sandbox's problem, not the manifest's.
- The uninstall succeeds: `Folder left behind: False` and `Uninstall entries left: 0`.

**The upgrade case**, once two releases with this installer exist. v0.10.0 is the first whose Uninstall key is
`TaskSmack` (#3276); v0.9.x's was `TaskSmack 0.9.x`. So test from v0.10.0 to v0.11.0, once v0.11.0 is published and
before you submit it. Generate both folders as above, then install v0.11.0's manifest over v0.10.0.
`winget install --manifest` runs the new installer over the old one, which is what `UpgradeBehavior: install`
does. Before publication, `winget upgrade` can't tie the installed copy to the manifest.

```powershell
.\SandboxTest.ps1 -Manifest $manifest -MapFolder $sandbox -Script {
    $arp = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    winget install --manifest "$env:USERPROFILE\Desktop\winget-sandbox\v0.11.0\winget\manifests\t\TaskSmack\TaskSmack\0.11.0" --silent --accept-source-agreements
    @(Get-ChildItem $arp | Get-ItemProperty | Where-Object DisplayName -Like 'TaskSmack*' |
        Select-Object PSChildName, DisplayName, DisplayVersion) | Format-Table -AutoSize
    "TaskSmack.exe: $((Get-Item "$env:ProgramFiles\TaskSmack\TaskSmack.exe").VersionInfo.ProductVersion)"
    winget list --name TaskSmack --accept-source-agreements
}
```

(The Sandbox maps `$sandbox` to its desktop under the same folder name.) The expected result: the install succeeds
silently. The table shows exactly **one** entry, `TaskSmack` at `DisplayVersion` `0.11.0`: one entry, not two.
TaskSmack.exe reports 0.11.0, and `winget list --name TaskSmack` shows one TaskSmack at 0.11.0.

To check the running case (#4436), start the installed TaskSmack in the Sandbox before the `winget install
--manifest` line: winget should report "Application is currently running" (the installer's exit code 32), and the
table should still show the old version. Close TaskSmack and run it again to upgrade.

**After publication, check by ID.** Once the winget-pkgs pull request merges, run these in a fresh Sandbox:
- `.\SandboxTest.ps1 -Script { winget install --id TaskSmack.TaskSmack --exact --silent --accept-source-agreements; winget list --id TaskSmack.TaskSmack --accept-source-agreements }`
  should show TaskSmack.TaskSmack with source `winget`.
- Once a second version is published, install the older one with `--version`, then run
  `winget upgrade --id TaskSmack.TaskSmack --silent`. It should move to the newest version, leave one ARP entry, and
  leave `winget upgrade` with nothing pending.
- `winget uninstall --id TaskSmack.TaskSmack --silent` should remove it.

Only these by-ID checks prove that winget ties an installed TaskSmack to the published package.

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
   --release-notes-url <release> --release-date <date> --out <folder>`. wingetcreate downloads both installers,
   hashes them, copies every other field from the previous version's manifests, and writes the new version's
   manifests to the folder, without submitting. The job checks that every file is on `WINGETCREATE_MANIFEST_VERSION`,
   then runs `wingetcreate submit --prtitle "Update: TaskSmack.TaskSmack to X.Y.Z" <folder>`, which validates them,
   pushes a branch to `tasksmack-boop/winget-pkgs` and opens the pull request from it.
6. looks the pull request up on winget-pkgs (search, up to 6 tries 20 s apart) and links it in the step summary.
   Only an open or merged pull request counts, not an earlier rejected one. If none turns up, the job fails.

A dry run exercises the manifest half without the means to submit (#3822). `rehearse-winget` has no `environment:`
and no secret, so it can't read `WINGET_TOKEN`, and it runs no `submit`; `tests/tools/test_workflow_run_steps.py`
fails if that changes. It takes the previous published release's installers (verified by
`fetch-previous-windows-release`), builds that release's manifests from `packaging/winget/`'s templates with
`tools/winget-manifest.ps1`, and checks them with `tools/check-winget-manifests.ps1`: every file on
`WINGETCREATE_MANIFEST_VERSION`, the package identifier and version, both installers' release URLs, and each
`InstallerSha256` against the installer's own hash. Once `TaskSmack.TaskSmack` is in winget-pkgs it also runs
`wingetcreate update` (anonymously, no `--submit`) for that release and checks what it wrote the same way. Until
then, the update half notices and skips, as `publish-winget` does. `create-release` waits for the rehearsal, so a
failing one fails the dry run.

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

A patch happens only when a shipped bug warrants one. `tools/release.ps1` does the hand steps; each step can be run
again, and prints what to do next. Its `-Step Tag` only prints the commands: agents never push a tag.

| # | Who | What | Command |
|---|---|---|---|
| 1 | Lane that owns the fix | Merge the fix to main and add the `backport:X.Y` label. `backport.yml` opens the cherry-pick PR against `release/X.Y`. If it conflicts, cherry-pick by hand (`git cherry-pick -x <squash SHA>`) and open that PR. | `/backport` comment on the merged PR, to retry |
| 2 | Lane that owns the fix | Merge each backport PR when CI is green and its review threads are resolved. | |
| 3 | Reviewer | Review the `vX.Y.(N-1)..release/X.Y` diff only. | `git diff vX.Y.(N-1)..origin/release/X.Y` |
| 4 | Windows lane | Open the version bump PR against `release/X.Y`: one changed line, `project(TaskSmack VERSION X.Y.N ...)`. Merge it before the candidate. | `pwsh tools/release.ps1 -Step Bump -Version X.Y.N` |
| 5 | Windows lane | Soak, then the signed dry run on the branch's head, with every job green. | `pwsh tools/release.ps1 -Step DryRun -Ref release/X.Y` |
| 6 | Both lanes | Validate (stage 6): Windows runs the Sandbox install check, WSL installs the Linux packages. | `pwsh tools/sandbox-install-check.ps1 -Installer <path>` on the dry run's installer |
| 7 | Maintainer | Approve the candidate on the tracker. | |
| 8 | Maintainer | Tag the approved commit and push the tag. | `pwsh tools/release.ps1 -Step Tag -Version X.Y.N -Sha <sha>` prints the commands |
| 9 | Maintainer | Merge both CHANGELOG PRs (one against `release/X.Y`, one against main). | `pwsh tools/release.ps1 -Step Status -Version X.Y.N` |
| 10 | Windows lane | Post-publish verification, summary on the tracker. | `pwsh tools/release.ps1 -Step Verify -Version X.Y.N` |
| 11 | Maintainer | [SmartScreen submission](#smartscreen-submission). | |
| 12 | Windows lane | Check `publish-winget` opened the winget-pkgs PR. For v0.11.1, the first automatic one, the maintainer signs the CLA as `tasksmack-boop` on it. If 0.11.0's first submission was still open when the tag run reached the job, it submitted nothing: once that merges, `-Rerun` re-runs the job. | `pwsh tools/release.ps1 -Step WinGet -Version X.Y.N [-Rerun]` |
| 13 | Maintainer | Close the milestone. A patch gets no Discussions announcement. | |

A patch to an older line (a v0.10.x after v0.11.0) gets no WinGet pull request: `publish-winget` skips it, since
WinGet's newest version would go backwards.

## Who does what

| Role | Responsibilities |
|---|---|
| Maintainer | scope and late-feature calls; declares the freeze; approves the candidate; manual checks; cuts the release branch; pushes the tag; merges the CHANGELOG PRs; SmartScreen submission; announcement |
| Windows lane | Windows code and its review sweep; CI and the release pipeline; dry runs; Windows validation; post-publish verification; Windows screenshots; performance baselines; the Windows newcomer check |
| WSL lane | Linux code and its review sweep; Linux packaging; Linux validation; the docs site; Linux screenshots; the Linux newcomer check |
| agentDash | [the release dashboard](#the-release-dashboard): its generator, page and workflow (`agent:Dash`) |

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
| The `release/0.11` cut was refused for an old merge commit in main's history (Require linear history). | 0.11 | Stage 7 step 1: a one-time admin bypass for the cut, removed after it. |

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
