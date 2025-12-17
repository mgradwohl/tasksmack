# Contributing to TaskSmack

Thank you for your interest in contributing!

> **Using GitHub Copilot?** See [.github/copilot-coding-agent-tips.md](.github/copilot-coding-agent-tips.md) for tips on writing effective issues and working with Copilot on this repository.

## Development Setup

### Prerequisites

See [README.md](README.md#requirements) for detailed prerequisites and minimum versions.

**Quick summary:**
- Clang 21+ (C++23 compiler)
- CMake 3.28+, Ninja
- Python 3 with jinja2 (for GLAD OpenGL loader)
- lld, ccache (recommended)
- clang-tidy, clang-format

### Verify Prerequisites

Run the prerequisite checker to verify all required tools:

```bash
./tools/check-prereqs.sh    # Linux
.\tools\check-prereqs.ps1   # Windows
```

See [README.md](README.md#requirements) for the complete list of tools, minimum versions, and common fixes.

### Building

See [README.md](README.md#step-5-build-and-run) for detailed build instructions and all available presets.

```bash
# Quick start
cmake --preset debug        # or win-debug on Windows
cmake --build --preset debug
```

### Running Tests

```bash
ctest --test-dir build/debug --output-on-failure
```

## Code Style

We follow strict C++23 coding standards. See [.github/copilot-instructions.md](.github/copilot-instructions.md#coding-standards) for comprehensive guidelines including:
- Naming conventions
- Include order requirements  
- Modern C++ feature usage
- Rule of 5 compliance
- RAII and exception safety
- Common clang-tidy warning avoidance

### Formatting

This project uses clang-format with the configuration in `.clang-format`. Run before committing:

```bash
./tools/clang-format.sh    # Linux
.\tools\clang-format.ps1   # Windows
```

To check without modifying:
```bash
./tools/check-format.sh    # Linux
.\tools\check-format.ps1   # Windows
```

### Static Analysis

Run clang-tidy regularly:

```bash
./tools/clang-tidy.sh debug    # Linux
.\tools\clang-tidy.ps1 debug   # Windows
```

See [.github/copilot-instructions.md](.github/copilot-instructions.md#avoiding-common-clang-tidy-warnings) for guidance on avoiding common warnings.

## Pull Request Process

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make your changes
4. Run formatting: `./tools/clang-format.sh`
5. Run static analysis: `./tools/clang-tidy.sh`
6. Run tests: `ctest --test-dir build/debug`
7. Commit your changes (`git commit -m 'Add amazing feature'`)
8. Push to the branch (`git push origin feature/amazing-feature`)
9. Open a Pull Request

The **PR template** will guide you through the checklist. Please complete all applicable items before requesting review.

## Reporting Issues

Please use the appropriate **issue template**:
- **Bug Report** - For bugs and unexpected behavior
- **Feature Request** - For new features and improvements

The templates will guide you to include all necessary information.

## CI Workflow Reports

The CI pipeline generates several reports that can help with debugging and code quality. Here's how to access them:

### Using GitHub UI

1. Go to the **Actions** tab in the repository
2. Click on the workflow run you want to inspect
3. Scroll to the bottom to find the **Artifacts** section
4. Download any of these artifacts:
   - `coverage-html-report` - Full HTML coverage report
   - `asan-ubsan-report` - AddressSanitizer + UndefinedBehaviorSanitizer results
   - `tsan-report` - ThreadSanitizer results
   - `linux-test-results` / `windows-test-results` - Test results
   - `clang-tidy-results` - Static analysis output

### Using GitHub CLI

```bash
# Download specific artifacts by name
gh run download <run-id> -n coverage-html-report
gh run download <run-id> -n asan-ubsan-report
gh run download <run-id> -n tsan-report
```

## Project Structure

```
.
├── CMakeLists.txt          # Main build configuration
├── CMakePresets.json       # CMake presets for all platforms/configs
├── Devshell-Updated.ps1    # Windows dev environment setup
├── setup.sh / setup.ps1    # Project renaming scripts (deleted after use)
├── src/
│   ├── main.cpp            # Application entry point
│   └── version.h.in        # Version header template (generates version.h)
├── tests/
│   ├── CMakeLists.txt      # Test configuration
│   └── test_main.cpp       # Example tests
├── tools/
│   ├── build.sh/ps1        # Build helper scripts
│   ├── configure.sh/ps1    # Configure helper scripts
│   ├── check-prereqs.sh/ps1 # Check prerequisite tools and versions
│   ├── clang-tidy.sh/ps1   # Static analysis
│   ├── clang-format.sh/ps1 # Code formatting
│   ├── check-format.sh/ps1 # Format checking
│   └── coverage.sh/ps1     # Code coverage reports
├── dist/                   # CPack output (generated, gitignored)
│   └── *.zip, *.tar.gz     # Distribution packages
├── coverage/               # Coverage reports (generated, gitignored)
│   └── index.html          # HTML coverage report
├── .clang-format           # Formatting rules
├── .clang-tidy             # Static analysis rules
├── .clangd                 # clangd LSP configuration
├── .github/
│   ├── workflows/ci.yml    # CI/CD pipeline
│   ├── ISSUE_TEMPLATE/     # Bug report and feature request templates
│   ├── pull_request_template.md  # PR checklist
│   └── dependabot.yml      # Automated dependency updates
├── SECURITY.md             # Security policy and vulnerability reporting
└── .vscode/
    ├── tasks.json          # Build tasks (platform-aware)
    ├── launch.json         # Debug configurations (platform-aware)
    └── settings.json       # Editor settings
```

### Security Issues

For security vulnerabilities, please see [SECURITY.md](SECURITY.md) for responsible disclosure guidelines. **Do not** open public issues for security vulnerabilities.
