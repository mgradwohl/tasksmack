# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

TaskSmack's Windows release binaries are signed with an Authenticode certificate issued to SignPath Foundation
through its free program for open-source projects. Windows shows **SignPath Foundation** as the publisher of a
signed TaskSmack binary, for example in the User Account Control prompt and in the file's Properties >
Digital Signatures tab.

Signing is being set up ([#2302](https://github.com/mgradwohl/tasksmack/issues/2302)). Until the SignPath
Foundation approves the project, the release workflow signs with a self-signed SignPath **test** certificate,
"TaskSmack Test Signing (untrusted)", which Windows doesn't trust; it proves the signing pipeline end to end.
Releases published before Foundation signing is in place are not signed with a trusted certificate.

## What is signed

- **`TaskSmack.exe`**, the application in the Windows x64 `.zip` on the
  [releases page](https://github.com/mgradwohl/tasksmack/releases).
- **`TaskSmackHelper.exe`**, the TaskSmack helper ([#2181](https://github.com/mgradwohl/tasksmack/issues/2181)),
  beside it in the same `.zip`. The helper ships only signed: the release workflow refuses to publish a `.zip`
  whose two executables aren't both validly signed and timestamped by the expected signer with one certificate.
- Not yet: the Windows ARM64 `.zip`, which ships without the helper and unsigned for now.

## How signing works

The release workflow builds and packages the Windows x64 `.zip` on a GitHub-hosted runner, then a separate job
that checks out no code and builds nothing submits that `.zip` to SignPath. SignPath signs and timestamps
`TaskSmack.exe` and `TaskSmackHelper.exe` inside it and returns it. The workflow then checks both signatures
(`Get-AuthenticodeSignature`: valid, the expected signer, timestamped, one certificate) before the `.zip` is
hashed, signed with Sigstore and published. The details are in
[CONTRIBUTING.md](https://github.com/mgradwohl/tasksmack/blob/main/CONTRIBUTING.md#windows-code-signing).

Only binaries built from this repository's source by the project's GitHub Actions release workflow
([`.github/workflows/release.yml`](https://github.com/mgradwohl/tasksmack/blob/main/.github/workflows/release.yml)),
running on GitHub-hosted runners, are submitted for signing. Nothing built on a developer's machine, in a fork,
or by a pull request's CI run is signed. Rehearsal runs of the release workflow (`dry_run`) are signed only
with the untrusted test certificate, never with the SignPath Foundation certificate; once that certificate is
in use, it signs only real release runs for a `v*.*.*` tag.

The third-party libraries TaskSmack uses are compiled from source and linked statically into
`TaskSmack.exe`. TaskSmack does not sign binaries published by other projects; if a release ever ships one, it
ships with that project's own signature or none.

Every signed binary carries the product name **TaskSmack** and the product version of the release
(`project(TaskSmack VERSION ...)` in `CMakeLists.txt`, which the release workflow requires to match the tag).

On top of Authenticode, every release asset (the `.zip`, `.deb`, `.tar.gz` and SBOM) is also signed with
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
- **Approvers.** mgradwohl approves every signing request for the SignPath Foundation certificate in SignPath
  by hand; the release workflow waits for that approval. Requests for the untrusted test certificate need no
  approval.

All team members use multi-factor authentication for both GitHub and SignPath.

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
