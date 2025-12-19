# Completed Features

This file lists features that are already implemented in TaskSmack.

## Application Features

(From [tasksmack.md](tasksmack.md).)

- MEM% column (memory as a percentage of total RAM)
- TIME+ column (CPU time formatted as H:MM:SS.cc)
- Command column (full command line)
- Task summary header (e.g., “N processes, M running”)
- VIRT column (virtual memory size)
- NI column (nice value / priority)
- Thread count column
- PPID column
- SHR column (shared memory size)
- Process state color coding
- Column visibility toggles (persisted)

## Developer Tooling / Infrastructure

- CMake presets implementation
- Deprecation of legacy scripts
- VS Code integration updates (tasks / launch configs)
- Precompiled headers (PCH)
- Compiler caching support (ccache/sccache)
- clang-tidy warning fixes and workflow
- CI on Linux + Windows (build, tests, formatting, clang-tidy, coverage)
- Code coverage reporting (llvm-cov)
- CPack packaging support
- Version header generation (`version.h`)
- Sanitizer presets (ASan+UBSan, TSan on Linux)
- Comprehensive compiler warning configuration (platform-tuned)

## Completed Improvement Ideas

- Version header generation (build embeds version/build/compiler metadata)
