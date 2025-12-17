# Contributing to TaskSmack

Thank you for your interest in contributing!

> **Using GitHub Copilot?** See [.github/copilot-coding-agent-tips.md](.github/copilot-coding-agent-tips.md) for tips on writing effective issues and working with Copilot on this repository.

## Development Setup

### Prerequisites

**Linux:**
```bash
# Ubuntu/Debian (with LLVM APT repository)
sudo apt install clang-21 lld-21 cmake ninja-build python3 python3-jinja2

# Or use a recent clang version (21+) available on your system
```

**Windows:**
1. Install LLVM from https://releases.llvm.org/
2. Set `LLVM_ROOT` environment variable to the LLVM installation path
3. Install CMake and Ninja
4. Install Python 3 (`winget install Python.Python.3.12`) and jinja2 (`pip install jinja2`)
5. (Optional) Install vcpkg for package management

### Verify Prerequisites

Run the prerequisite checker to verify all required tools are installed with correct versions:

```bash
./tools/check-prereqs.sh    # Linux
.\tools\check-prereqs.ps1   # Windows
```

The script checks for:
| Tool | Minimum Version | Purpose |
|------|-----------------|---------|
| clang/clang++ | 21+ | C++23 compiler |
| cmake | 3.28+ | Build system |
| ninja | any | Build backend |
| lld | any | Fast linker |
| ccache | 4.9.1+ | Compiler cache |
| clang-tidy | any | Static analysis |
| clang-format | any | Code formatting |
| llvm-profdata | any | Coverage (optional) |
| llvm-cov | any | Coverage (optional) |
| python3 | 3.0+ | GLAD OpenGL loader generation |
| jinja2 | any | GLAD template engine |

**Common fixes:**
- **jinja2 not found:** Run `pip install jinja2` (or `pip install --user jinja2` if no admin rights)
- **LLVM_ROOT not set (Windows):** Add to environment variables or run `Devshell-Updated.ps1`
- **Wrong clang version:** Ensure LLVM 21+ is in PATH before older versions

### Building

```bash
# Linux
./tools/configure.sh debug
./tools/build.sh debug

# Windows (PowerShell)
.\tools\configure.ps1 debug
.\tools\build.ps1 debug
```

### Running Tests

```bash
ctest --test-dir build/debug --output-on-failure
```

## Code Style

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

### Include Guidelines

**Include order:**
1. Matching header (for `.cpp` files)
2. Project headers (alphabetical)
3. Third-party headers (alphabetical)
4. Standard library headers (alphabetical)

Separate each group with a blank line.

**Example:**
```cpp
#include "MyClass.h"

#include "src/utils/Helper.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <vector>
```

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

### Security Issues

For security vulnerabilities, please see [SECURITY.md](SECURITY.md) for responsible disclosure guidelines. **Do not** open public issues for security vulnerabilities.
