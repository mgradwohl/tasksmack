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

Reports about TaskSmack itself are in scope, and in particular about its privileged helpers: on Linux,
`tasksmack-helper` (started through `pkexec` for a polkit action, installed by the `.deb`), and on Windows,
`TaskSmackHelper.exe` (started elevated through UAC). Each helper serves only the TaskSmack that started it
and answers a fixed set of requests.

## Security Practices

- **Sanitizers:** ASan, UBSan and TSan run in CI on Linux; an MSan preset is also available.
- **Static analysis:** clang-tidy (warnings fail the build), CodeQL, OSV-Scanner, dependency review and
  OpenSSF Scorecard.
- **Fuzzing:** ClusterFuzzLite fuzzes the parsers and the helper's request protocol.
- **Hardening:** release builds use `-fstack-protector-strong` everywhere, plus `_FORTIFY_SOURCE=2` and PIE on
  Linux. The Linux helper, which `pkexec` starts as root (it then serves as `nobody` with four capabilities), adds full RELRO with immediate binding, stack-clash protection,
  CET on x86-64, PAC return-address signing on AArch64 (BTI isn't enforced there: Ubuntu 24.04's glibc startup
  objects have no BTI landing pads, so no executable linked on it can carry the BTI note;
  [#2786](https://github.com/mgradwohl/tasksmack/issues/2786)), `_FORTIFY_SOURCE=3`
  and libc++'s fast hardening mode, and at run time keeps only the four capabilities its requests need, with
  `no_new_privs` set; the Windows helper links with Control Flow Guard and CET shadow-stack compatibility. Debug
  builds use `-ftrivial-auto-var-init=pattern` to expose uninitialized variables.
- **Dependencies:** fetched with CMake FetchContent at pinned versions, and kept current by Renovate.
- **CI:** builds and tests on every pull request and push to `main`, plus nightly checks.
- **Releases:** every release asset is signed with Sigstore; see
  [Verifying a Release](docs/guide/user-guide.md#verifying-a-release).

## Dependencies

See [CONTRIBUTING.md](CONTRIBUTING.md#keeping-dependencies-current) for how dependencies are pinned and updated.
