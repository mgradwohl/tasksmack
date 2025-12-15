# MyProject - TODO & Improvement Ideas

## Recommended Next Steps

Priority order for maximum impact with minimal effort:

1. **EditorConfig** (5 min) - Immediate value, prevents CRLF/LF diffs
2. **Security hardening flags** (15 min) - Add to release/optimized presets
3. **Pre-commit hooks** (30 min) - Catch formatting issues before CI
4. **GitHub Release workflow** (1 hr) - Auto-create releases with binaries on tag push
5. **Dev container** (2-3 hrs) - Biggest friction reducer for new users
6. **`std::expected` example** (1 hr) - Showcase modern C++23 idioms

---

## Active Backlog

- [ ] **Performance profiling** - Add perf/Instruments support, profile-guided optimization (PGO) builds

---

## Improvement Ideas

### High Impact / Quick Wins

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **EditorConfig** | Developer Experience | 5 min | An `.editorconfig` file ensures consistent editor settings (indentation, line endings, charset) across all editors and IDEs—not just VS Code. This is especially valuable for cross-platform projects where Windows developers might accidentally introduce CRLF line endings. It's a simple file that prevents annoying whitespace-only diffs. | Ready to implement. No blockers. |
| **Hardening flags** | Build & Tooling | 15 min | Security hardening compiler flags like `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-fPIE`, and Control Flow Integrity (CFI) protect against common vulnerability classes including buffer overflows and ROP attacks. These flags have minimal performance impact in release builds and represent security best practices that should be standard in any production-quality C++ project. | Ready to implement. Add to release/optimized presets. |
| **Pre-commit hooks** | Developer Experience | 30 min | Pre-commit hooks via `.pre-commit-config.yaml` automatically run clang-format and other checks before each commit. This catches formatting issues, trailing whitespace, and other problems at the earliest possible moment—before code even reaches CI. Developers get instant feedback and never accidentally push unformatted code. | Ready to implement. Requires Python for pre-commit tool. |
| **GitHub Release workflow** | Documentation & Release | 1 hr | A GitHub Actions workflow that automatically creates releases with pre-built binaries when you push a version tag streamlines the release process. Users get downloadable artifacts without manual intervention, and the release notes can be auto-generated from commit history. | Ready to implement. No blockers. |
| **Dev container** | Developer Experience | 2-3 hrs | A `.devcontainer/` configuration enables instant, reproducible development environments in VS Code and GitHub Codespaces. New contributors can start coding within minutes without manually installing clang, cmake, ninja, or any other dependencies. This dramatically lowers the barrier to entry for contributors and ensures everyone has identical tooling, eliminating "works on my machine" issues. | Ready to implement. No blockers. |

### Code Quality & Safety

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **Fuzzing** | Testing & Quality | 2 hrs | libFuzzer integration provides automated, coverage-guided fuzzing that generates random inputs to find edge cases and crashes. Unlike unit tests that check expected behavior, fuzzing discovers unexpected failures. A single fuzzing target can find more bugs in an hour than months of manual testing, especially for parsing, serialization, or any code handling untrusted input. | Ready to implement. libFuzzer built into Clang. Needs fuzz targets. |
| **Property-based testing** | Testing & Quality | Medium | Property-based testing with rapidcheck generates hundreds of random test cases from declarative properties. Instead of writing `EXPECT_EQ(reverse(reverse(list)), list)` for a few examples, you declare the property and the framework generates diverse inputs. This finds edge cases you wouldn't think to test manually. | Ready to implement. rapidcheck available via FetchContent. |
| **Mutation testing** | Testing & Quality | High | Mutation testing tools like mull systematically introduce bugs into your code and verify that your tests catch them. This measures the actual quality of your test suite—not just coverage, but whether your assertions would detect real bugs. It answers the question: "If this code was wrong, would my tests fail?" | **Blocked.** Mull requires LLVM 14-17. Clang 22 likely needs newer mull support. Defer until mull catches up. |

### Performance Optimization

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **Benchmark framework** | Testing & Quality | 1-2 hrs | Google Benchmark integration provides a standard way to measure and track performance. Microbenchmarks catch performance regressions early, and the framework handles warmup, statistical analysis, and output formatting. Combined with CI, you can detect performance regressions before they reach production. | Ready to implement. Available via FetchContent. |
| **Compile-time metrics** | Build & Tooling | Low | Use `-ftime-trace` to generate Chrome tracing files showing where build time goes. Helps identify slow headers and optimize compilation. | Ready to implement. Built into Clang. |
| **Unity builds** | Build & Tooling | Low | `CMAKE_UNITY_BUILD` combines multiple source files into single translation units, dramatically reducing compile times for full rebuilds by eliminating redundant header parsing and enabling better optimization across file boundaries. While incremental builds don't benefit, clean builds can be 2-5x faster. | Ready to implement. Single CMake variable. Low value for small projects. |

### Modern C++ Adoption

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **`std::expected` examples** | Modern C++23 | 1 hr | `std::expected<T, E>` provides a modern, type-safe alternative to exceptions or error codes for recoverable errors. Including example patterns in the template demonstrates idiomatic C++23 error handling and helps developers adopt this powerful feature correctly. | Ready to implement. Fully supported in Clang 22. |
| **`std::mdspan` examples** | Modern C++23 | Low | Multi-dimensional array views for scientific computing and graphics. Shows modern approach to multi-dimensional data access. | Ready to implement. Fully supported in Clang 22. |
| **`std::ranges` pipelines** | Modern C++23 | Low | Modern iteration patterns with range adaptors and views. Cleaner, more composable code than raw loops. | Ready to implement. Fully supported in Clang 22. |
| **`std::print` adoption** | Modern C++23 | Low | `std::print` and `std::println` provide type-safe, format-string-based output that's cleaner than iostream and safer than printf. For simple console output, this can replace spdlog dependencies entirely, reducing compilation time and external dependencies. | ✅ Already implemented in template. |
| **`std::generator` examples** | Modern C++23 | Low | `std::generator` enables lazy, coroutine-based sequences that can represent infinite or expensive-to-compute collections. Example code demonstrating generator patterns helps developers leverage this C++23 feature for cleaner, more memory-efficient iteration. | **Partial support.** libc++ `std::generator` is experimental. May need `-fexperimental-library`. |
| **C++20/23 module support** | Build & Tooling | Medium | C++ modules (`import std;`) replace header files with compiled module interfaces, offering faster compilation, better encapsulation, and elimination of include-order bugs. CMake 3.28+ has experimental module support. While tooling is still maturing, early adoption positions the project for the future of C++. | **Blocked.** `import std;` requires custom-built libc++ with modules. clangd support incomplete. Revisit late 2026 (LLVM 24+). |

### Tooling Gaps

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **Dependency scanning** | Security | Medium | `osv-scanner` or similar for scanning C++ dependencies for known vulnerabilities. Dependabot only covers GitHub Actions, not C++ deps. | Ready to implement. osv-scanner is standalone binary. |
| **Include-what-you-use** | Code Quality | Medium | IWYU is more reliable than clang-tidy's `misc-include-cleaner`. Ensures headers include exactly what they need. | Ready to implement. Separate tool from LLVM. |

### Platform & Distribution

| Idea | Type | Effort | Description | Notes |
|------|------|--------|-------------|-------|
| **Changelog generation** | Documentation & Release | 1 hr | Conventional commit messages combined with automated changelog generation (via tools like git-cliff or release-please) create professional, consistent release notes. This documents what changed between versions without manual effort, helping users understand upgrade impacts. | Ready to implement. git-cliff is standalone binary. |
| **SBOM generation** | Documentation & Release | Medium | Software Bill of Materials (SBOM) generation documents all dependencies in standard formats like SPDX or CycloneDX. This is increasingly required for supply chain security compliance and helps users understand exactly what's in your software. Some industries and government contracts now mandate SBOMs. | Ready to implement. Tools: syft, cyclonedx-cli. May need CMake integration. |
| **License scanning** | Documentation & Release | Low | REUSE compliance ensures every file has clear license information via SPDX headers. This removes legal ambiguity for users and contributors, making it clear how the code can be used. Automated scanning in CI prevents accidentally adding incompatibly-licensed code. | Ready to implement. REUSE tool via pip. |
| **Cross-compilation presets** | Build & Tooling | Medium | CMake presets for ARM64 and WebAssembly (via Emscripten) targets enable building for additional platforms without leaving the familiar preset workflow. ARM64 support is increasingly important as Apple Silicon and ARM servers become mainstream; WASM enables running C++ in browsers. | ARM64: Ready (needs cross-compiler). WASM: Requires Emscripten SDK install. |

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
