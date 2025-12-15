# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| Latest  | :white_check_mark: |

## Reporting a Vulnerability

If you discover a security vulnerability in this project, please report it responsibly:

1. **Do not** open a public GitHub issue for security vulnerabilities
2. Instead, please email the maintainer directly or use GitHub's private vulnerability reporting feature

### What to Include

- Description of the vulnerability
- Steps to reproduce
- Potential impact
- Suggested fix (if any)

### Response Timeline

- **Acknowledgment:** Within 48 hours
- **Initial assessment:** Within 1 week
- **Resolution:** Depends on severity and complexity

## Security Best Practices

This project follows security best practices:

- **Sanitizers:** ASan, UBSan, and TSan presets for catching memory and threading bugs
- **Static Analysis:** clang-tidy integration for catching potential issues at compile time
- **Hardening:** Debug builds use `-ftrivial-auto-var-init=pattern` to catch uninitialized variable bugs
- **Dependencies:** Managed via CMake FetchContent with pinned versions
- **CI/CD:** Automated testing on every push and pull request

## Dependencies

Third-party dependencies are fetched at build time:

- **spdlog** - Logging library (MIT License)
- **Google Test** - Testing framework (BSD-3-Clause License)

We recommend regularly updating dependencies and running security scans on production builds.
