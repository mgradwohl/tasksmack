# Completed Features

This file lists features that are already implemented in TaskSmack.

## Application Features

| Feature | Source | Notes |
|---------|--------|-------|
| **MEM% Column** | htop | Memory as percentage of total system RAM |
| **TIME+ Column** | htop | CPU time formatted as H:MM:SS.cc |
| **Command Column** | htop | Full command line from `/proc/[pid]/cmdline` |
| **Task Summary** | htop | "N processes, M running" in panel header |
| **VIRT Column** | htop | Virtual memory size |
| **NI (Nice) Column** | htop | Process nice value (-20 to 19) |
| **Thread Count Column** | htop | Number of threads per process |
| **PPID Column** | htop | Parent process ID |
| **SHR Column** | htop | Shared memory size |
| **State Color Coding** | htop | Color-code process states based on theme |
| **Column Visibility Toggles** | btop++ | Right-click table header to show/hide columns; persisted to config |
| **Process Tree View** | btop++, htop | Hierarchical view showing parent-child relationships with collapsible nodes |
| **PRI (Base Priority) Column** | Task Manager | Windows-style base priority (1-31); shows scheduling priority |

## Developer Tooling / Infrastructure

| Item | Notes |
|------|-------|
| **CMake presets** | Presets for Debug/Release and platform variants |
| **Legacy scripts deprecated** | Consolidated tooling scripts |
| **VS Code integration** | Tasks and launch configs |
| **Precompiled headers (PCH)** | Faster builds |
| **Compiler caching support** | ccache/sccache |
| **clang-tidy workflow** | Helper scripts and curated config |
| **CI on Linux + Windows** | Build, tests, format check, clang-tidy, coverage |
| **Coverage reporting** | llvm-cov HTML reports |
| **CPack packaging** | ZIP/installer generation via CPack |
| **Version header generation** | Auto-generate `version.h` during configure |
| **Sanitizer presets** | ASan+UBSan, TSan on Linux |
| **Compiler warning configuration** | Tuned warnings and warnings-as-errors |
| **`std::print` adoption** | Type-safe, format-string-based output |
