# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| Latest  | :white_check_mark: |

## Reporting a Vulnerability

If you discover a security vulnerability in this project, please report it responsibly using [GitHub's private vulnerability reporting](https://github.com/mgradwohl/tasksmack/security/advisories/new):

1. **Do not** open a public GitHub issue for security vulnerabilities
2. Use GitHub's private vulnerability reporting: https://github.com/mgradwohl/tasksmack/security/advisories/new

### What to Include

- Description of the vulnerability
- Steps to reproduce
- Potential impact
- Suggested fix (if any)

### Response Timeline

- **Acknowledgment:** Within 48 hours
- **Initial assessment:** Within 1 week
- **Resolution:** Depends on severity and complexity

## Scope

Reports about TaskSmack itself are in scope, and in particular about its privileged parts: on Linux, TaskSmack
started as root ("TaskSmack (Administrator)", through `pkexec` for its polkit action, or `sudo`), and on Windows,
`TaskSmackHelper.exe` (started elevated through UAC), which serves only the TaskSmack that started it and
answers a fixed set of requests.

## Security Practices

- **Sanitizers:** ASan, UBSan and TSan run in CI on Linux; an MSan preset is also available.
- **Static analysis:** clang-tidy (warnings fail the build), CodeQL, OSV-Scanner, dependency review and
  OpenSSF Scorecard.
- **Fuzzing:** ClusterFuzzLite fuzzes the parsers and the Windows helper's request protocol.
- **Hardening:** release builds use `-fstack-protector-strong` everywhere, plus `_FORTIFY_SOURCE=2` and PIE on
  Linux. On Windows, TaskSmack.exe and the helper, the two images that can run elevated, link with Control Flow
  Guard and CET shadow-stack compatibility, and load the DLLs they import from System32 only. The helper looks
  for a DLL loaded by name in System32 only. TaskSmack does too when it is elevated through UAC or started by
  Restart as administrator, whichever account approved it; otherwise it also looks in its own folder, where a
  software OpenGL can go on a PC with no GPU driver. One case is left: an account with no UAC split token (the
  built-in Administrator, SYSTEM, or an administrator with UAC turned off) starting a copy itself, not through
  Restart as administrator, from a folder another account can write to. Run TaskSmack that way only from a folder
  just administrators can change, such as its installed one. On Linux there is no separate helper (#3580):
  TaskSmack is either started as root for its whole session or runs as you, and never elevates after it starts.
  Debug
  builds use `-ftrivial-auto-var-init=pattern` to expose uninitialized variables.
- **Dependencies:** fetched with CMake FetchContent at pinned versions, and kept current by Renovate.
- **CI:** builds and tests on every pull request and push to `main`, plus nightly checks.
- **Releases:** every release asset is listed in a Sigstore-signed `SHA256SUMS`; see
  [Verifying a Release](docs/guide/user-guide.md#verifying-a-release).

## Dependencies

See [CONTRIBUTING.md](CONTRIBUTING.md#keeping-dependencies-current) for how dependencies are pinned and updated.
