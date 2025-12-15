# Contributing to MyProject

Thank you for your interest in contributing!

## Development Setup

### Prerequisites

**Linux:**
```bash
# Ubuntu/Debian
sudo apt install clang-22 lld-22 cmake ninja-build

# Or use a recent clang version available on your system
```

**Windows:**
1. Install LLVM from https://releases.llvm.org/
2. Set `LLVM_ROOT` environment variable to the LLVM installation path
3. Install CMake and Ninja
4. (Optional) Install vcpkg for package management

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

### Security Issues

For security vulnerabilities, please see [SECURITY.md](SECURITY.md) for responsible disclosure guidelines. **Do not** open public issues for security vulnerabilities.
