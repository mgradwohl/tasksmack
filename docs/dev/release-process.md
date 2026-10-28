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

- **Dependency updates** from Renovate and Dependabot: compiler, libraries, Python and actions. Approved majors
  go here.
- **CI timeouts with headroom:** each job finishes in under about 70% of its limit on a cold cache.
- **A signed release dry run on main.** Fix whatever fails, and repeat it after any change to `release.yml`:

  ```bash
  gh workflow run release.yml --ref main -f dry_run=true
  ```

- **Baselines,** recorded on the tracker during quiet hours with `tools/measure-idle.ps1` and `tools/bench.ps1`
  (pinned to a P-core):
  - idle CPU, visible and minimized;
  - memory over 90 minutes;
  - startup time;
  - benchmarks against the previous release.

**Done when:** main is green, the signed dry run passes, and the baselines are posted.

### 2. Fixes: refactors, bugs, test gaps

Severity first. Test gaps belong here, not at the end, because a test written before a refactor has to be redone.

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
2. **Candidate:** one exact commit proposed on the tracker, with the version bump merged (`VERSION X.Y.0` in
   `CMakeLists.txt`).
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
