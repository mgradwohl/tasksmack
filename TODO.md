# TaskSmack - TODO & Improvement Ideas

## Recommended Next Steps

Priority order for maximum impact with minimal effort:

1. **EditorConfig** (5 min) - Immediate value, prevents CRLF/LF diffs (Tracked in [#129](https://github.com/mgradwohl/tasksmack/issues/129))
2. **GitHub Release workflow** (1 hr) - Auto-create releases with binaries on tag push (Tracked in [#54](https://github.com/mgradwohl/tasksmack/issues/54))

---

## Improvement Ideas

### High Impact / Quick Wins

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **EditorConfig** | Developer Experience | 5 min | An `.editorconfig` file ensures consistent editor settings (indentation, line endings, charset) across all editors and IDEs—not just VS Code. This is especially valuable for cross-platform projects where Windows developers might accidentally introduce CRLF/LF diffs. It's a simple file that prevents annoying whitespace-only diffs. | Tracked in [#129](https://github.com/mgradwohl/tasksmack/issues/129). |
| **GitHub Release workflow** | Documentation & Release | 1 hr | A GitHub Actions workflow that automatically creates releases with pre-built binaries when you push a version tag streamlines the release process. Users get downloadable artifacts without manual intervention, and the release notes can be auto-generated from commit history. | Tracked in [#54](https://github.com/mgradwohl/tasksmack/issues/54). |

### Code Quality & Safety

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **Fuzzing** | Testing & Quality | 2 hrs | libFuzzer integration provides automated, coverage-guided fuzzing that generates random inputs to find edge cases and crashes. Unlike unit tests that check expected behavior, fuzzing discovers unexpected failures. A single fuzzing target can find more bugs in an hour than months of manual testing, especially for parsing, serialization, or any code handling untrusted input. | Tracked in [#102](https://github.com/mgradwohl/tasksmack/issues/102). |

### Performance Optimization

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **Benchmark framework** | Testing & Quality | 1-2 hrs | Google Benchmark integration provides a standard way to measure and track performance. Microbenchmarks catch performance regressions early, and the framework handles warmup, statistical analysis, and output formatting. Combined with CI, you can detect performance regressions before they reach production. | Tracked in [#104](https://github.com/mgradwohl/tasksmack/issues/104). |

### Modern C++ Adoption

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **`std::print` adoption** | Modern C++23 | Low | `std::print` and `std::println` provide type-safe, format-string-based output that's cleaner than iostream and safer than printf. For simple console output, this can replace spdlog dependencies entirely, reducing compilation time and external dependencies. | ✅ Already implemented in template. |

### Platform & Distribution

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **Changelog generation** | Documentation & Release | 1 hr | Conventional commit messages combined with automated changelog generation (via tools like git-cliff or release-please) create professional, consistent release notes. This documents what changed between versions without manual effort, helping users understand upgrade impacts. | Tracked in [#98](https://github.com/mgradwohl/tasksmack/issues/98). |

---

## Completed

- [x] CMake Presets Implementation
- [x] Deprecate legacy scripts
- [x] Update VS Code integration
- [x] Clean up tasks.json
- [x] Update utility scripts
- [x] Update README
- [x] Update setup scripts
- [x] Add Precompiled Headers (PCH)
- [x] Add compiler caching (ccache/sccache)
- [x] Fix clang-tidy warnings
- [x] Add GitHub Actions CI (Linux/Windows builds, format-check, static-analysis, coverage jobs)
- [x] Add code coverage (llvm-cov)
- [x] Add GNUInstallDirs
- [x] Add CPack support
- [x] Version header generation (version.h from CMake)
- [x] Sanitizer support (ASan+UBSan, TSan presets for Linux)
- [x] Comprehensive compiler warnings (platform-tuned for Clang on Windows/Linux)
- [x] Document design decisions (copilot-instructions.md, README.md)

### Completed Improvement Ideas

| Idea | Type | Effort | Description |
|------|------|--------|-------------|
| **Version header generation** | Build & Tooling | Low | Auto-generating a `version.h` header from CMake embeds version information directly in the binary. This enables runtime version queries, proper `--version` output, and ensures the compiled binary always knows its own version. It's a small feature that makes releases and debugging significantly easier. |

---

## Disabled Clang-Tidy Checks

The following checks are disabled in `.clang-tidy`. Many are too noisy for ImGui-based UI applications or conflict with common C++ idioms. Consider re-enabling selectively as the codebase matures.

| Check | Reason |
|-------|--------|
| `bugprone-easily-swappable-parameters` | Too opinionated; common in UI callbacks |
| `bugprone-exception-escape` | Traces through spdlog/STL exception paths, creates wall of noise |
| `cppcoreguidelines-avoid-c-arrays` | Required for ImGui API interop (char buffers for snprintf) |
| `cppcoreguidelines-avoid-magic-numbers` | UI code has many layout constants (colors, sizes, percentages) |
| `cppcoreguidelines-non-private-member-variables-in-classes` | Conflicts with protected members in Layer base class |
| `cppcoreguidelines-pro-bounds-array-to-pointer-decay` | Required for C API interop (ImGui, snprintf) |
| `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` | Too verbose; bounds are checked by logic |
| `cppcoreguidelines-pro-bounds-constant-array-index` | Too restrictive for loop-based array access |
| `cppcoreguidelines-pro-bounds-pointer-arithmetic` | Required for std::from_chars and similar APIs |
| `cppcoreguidelines-pro-type-reinterpret-cast` | Required for OpenGL string queries (glGetString) |
| `cppcoreguidelines-pro-type-vararg` | Required for ImGui::Text and snprintf |
| `misc-const-correctness` | Too noisy; many false positives with locks and iterators |
| `misc-include-cleaner` | Doesn't work correctly with Windows headers |
| `misc-non-private-member-variables-in-classes` | Duplicate of cppcoreguidelines variant |
| `modernize-avoid-c-arrays` | Duplicate of cppcoreguidelines variant |
| `modernize-deprecated-headers` | `<signal.h>` needed for POSIX signals on Linux |
| `modernize-use-auto` | Explicit types preferred for clarity in this codebase |
| `modernize-use-scoped-lock` | `std::lock_guard` is sufficient for single-mutex locks |
| `modernize-use-trailing-return-type` | Style preference, not a bug |
| `performance-unnecessary-value-param` | False positives with std::stop_token and similar |
| `portability-avoid-pragma-once` | `#pragma once` is the project standard |
| `readability-convert-member-functions-to-static` | Methods may need `this` in future; premature optimization |
| `readability-function-cognitive-complexity` | UI menu rendering inherently has nested conditionals |
| `readability-identifier-length` | Too restrictive (e.g., `i`, `it` are fine) |
| `readability-identifier-naming` | Conflicts with project naming conventions |
| `readability-implicit-bool-conversion` | Common C++ idiom (`if (ptr)`) |
| `readability-magic-numbers` | Duplicate of cppcoreguidelines variant |
| `readability-make-member-function-const` | Methods may mutate in future |
| `readability-use-concise-preprocessor-directives` | `#elifdef` not universally supported |

**TODO:** Review and re-enable some checks after codebase stabilizes, particularly:
- `misc-const-correctness` - Worth enabling with more targeted NOLINT comments
- `readability-convert-member-functions-to-static` - Good for finding dead code
- `performance-unnecessary-value-param` - Worth fixing with explicit moves
