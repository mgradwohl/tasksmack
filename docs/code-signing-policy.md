# Code signing policy

Code signing by [Azure Artifact Signing](https://learn.microsoft.com/azure/artifact-signing/) (formerly
Trusted Signing), with a Public Trust certificate issued to the maintainer's validated identity.

TaskSmack's Windows release binaries are signed with an Authenticode certificate whose subject is
`CN=Matt Gradwohl, O=Matt Gradwohl, L=REDMOND, S=wa, C=US`. Windows shows **Matt Gradwohl** as the publisher of
a signed TaskSmack binary, for example in the User Account Control prompt and in the file's Properties >
Digital Signatures tab. The certificates are short-lived (about three days) and Microsoft rotates them, so a
signature is identified by that subject, not by a certificate thumbprint, and every signature carries a
timestamp that keeps it valid after its certificate expires.

Releases from v0.9.1 are signed ([#2302](https://github.com/mgradwohl/tasksmack/issues/2302)). Releases up to
v0.9.0 are not Authenticode-signed, and their Windows `.zip`s don't contain the helper, which never ships
unsigned.

## What is signed

- **`TaskSmack.exe`**, the application in the Windows x64 and ARM64 `.zip`s and installers on the
  [releases page](https://github.com/mgradwohl/tasksmack/releases).
- **`TaskSmackHelper.exe`**, the TaskSmack helper ([#2181](https://github.com/mgradwohl/tasksmack/issues/2181)),
  beside it in the same `.zip` and installed beside it. The helper ships only signed: the release workflow
  refuses to publish a `.zip` or an installer whose executables aren't all validly signed and timestamped by
  the expected signer.
- **The installers**, `TaskSmack-<version>-win64.exe` and `TaskSmack-<version>-windows-arm64.exe`, and
  **`Uninstall.exe`**, the uninstaller each one installs, so installing and removing TaskSmack both show the
  same publisher.

## How signing works

The release workflow builds and packages each Windows `.zip` on a GitHub-hosted runner, with a first build of
the installer whose only use is its uninstaller. A separate job, which checks out no code and builds nothing,
then takes both `.zip`s and both uninstallers, signs and timestamps `TaskSmack.exe` and `TaskSmackHelper.exe`
from each `.zip` and each uninstaller through Azure Artifact Signing, and writes the executables back into their
`.zip`s, leaving everything else unchanged. It signs in to Azure with GitHub's OpenID Connect token for the
`release-signing` environment; no password or client secret exists. Another job, without that token, builds
each installer from its signed `.zip`'s files and its signed uninstaller, and a third job like the first signs
and timestamps the two installers. The workflow then checks the signatures in each `.zip`
(`Get-AuthenticodeSignature`: valid, the expected signer, a certificate Microsoft's ID Verified code-signing CA
issued with Artifact Signing's extended key usage, timestamped), and checks each installer the same way: its
own signature, then a silent install whose files must be the `.zip`'s and whose executables, the uninstaller
included, must all pass, a start of the installed TaskSmack, and a silent uninstall that must leave nothing
behind. Only then are the files hashed, signed with Sigstore and published. The details are in
[CONTRIBUTING.md](https://github.com/mgradwohl/tasksmack/blob/main/CONTRIBUTING.md#windows-code-signing).

Only binaries built from this repository's source by the project's GitHub Actions release workflow
([`.github/workflows/release.yml`](https://github.com/mgradwohl/tasksmack/blob/main/.github/workflows/release.yml)),
running on GitHub-hosted runners, are signed. Nothing built on a developer's machine, in a fork, or by a pull
request's CI run is signed. The `release-signing` environment admits only release runs for a `v*` tag and
rehearsal runs (`dry_run`) dispatched from the `main` or `release/*` branches; a rehearsal signs
with the same certificate but publishes nothing.

The third-party libraries TaskSmack uses are compiled from source and linked statically into
`TaskSmack.exe`. TaskSmack does not sign binaries published by other projects; if a release ever ships one, it
ships with that project's own signature or none.

Every signed `TaskSmack.exe`, `TaskSmackHelper.exe` and installer carries the product name **TaskSmack** and the product version of the release
(`project(TaskSmack VERSION ...)` in `CMakeLists.txt`, which the release workflow requires to match the tag).

On top of Authenticode, every release asset (the `.zip`, installer, `.deb`, `.tar.gz` and SBOM) is also signed with
Sigstore; see [Verifying a Release](guide/user-guide.md#verifying-a-release).

## Team roles and members

| Role | Members |
|------|---------|
| Committers and reviewers | [mgradwohl](https://github.com/mgradwohl) |
| Approvers | [mgradwohl](https://github.com/mgradwohl) |

- **Committers and reviewers.** mgradwohl owns the repository and is its only committer and reviewer. AI coding
  agents working for the maintainer open pull requests; they push nothing to `main` directly. Branch protection
  on `main` requires the CI checks to pass, allows no force-pushes, and applies to administrators too. An agent may merge a pull request it opened only
  once CI is green and every review thread on it is resolved. It never merges with administrator override,
  never force-pushes, and needs the maintainer's go-ahead to merge anyone else's pull request or take any
  other shared or hard-to-reverse action. These rules are in
  [AGENTS.md](https://github.com/mgradwohl/tasksmack/blob/main/AGENTS.md). Changes to the GitHub Actions
  workflows, including the release workflow, need the maintainer's explicit confirmation before they are
  pushed.
- **Approvers.** mgradwohl holds the Azure Artifact Signing account and its identity validation, controls which
  refs the `release-signing` environment admits, and alone pushes release tags.

All team members use multi-factor authentication for both GitHub and Azure.

## Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by
the user or the person installing or operating it.

TaskSmack has no telemetry, crash reporting, update check or analytics. The only network requests it makes
are the ones the user asks for:

- **Show public IP** (System tab, Network adapters section). Clicking it sends a DNS query for
  `myip.opendns.com` to OpenDNS's resolvers (208.67.222.222 over IPv4 and 2620:119:35::35 over IPv6), which
  answer with the address the query came from. Every **Refresh** of the System tab after that repeats the
  lookup until TaskSmack closes; nothing is sent before the click. OpenDNS's handling of that query is covered
  by [Cisco's privacy statement](https://www.cisco.com/c/en/us/about/legal/privacy-full.html).
- **Links** in the About dialog and the Help window (the repository, user guide, issue tracker and Font
  Awesome licence) open in the
  default web browser when clicked.

TaskSmack's settings and log file stay on the local machine.
