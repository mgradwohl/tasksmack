# Build Instructions

This page is a focused reference for configuring, building, and running TaskSmack across all supported presets. For toolchain setup and code-quality workflows, see the [Developer Guide](developer-guide.md).

---

## CMake Presets

### Linux Presets

| Preset | Description |
|--------|-------------|
| `debug` | Debug symbols, no optimisation, security hardening flags |
| `relwithdebinfo` | Debug symbols + optimisation (RelWithDebInfo) |
| `release` | Optimised, no debug symbols |
| `release-compatible` | Release build targeting x86-64-v2 (2009+ CPUs) |
| `optimized` | LTO + `-march=x86-64-v3` + stripped (Haswell 2013+) |
| `coverage` | Debug + LLVM code-coverage instrumentation |
| `asan-ubsan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |
| `benchmark` | Optimised build with Google Benchmark enabled |
| `profile` | Optimised + frame pointers for perf / flame graphs |
| `pgo-generate` | Instrumented build for PGO profile collection |
| `pgo-use` | PGO-optimised build (reads `profiles/tasksmack.profdata`) |

### Windows Presets

| Preset | Description |
|--------|-------------|
| `win-debug` | Debug symbols, no optimisation |
| `win-relwithdebinfo` | Debug symbols + optimisation |
| `win-release` | Optimised, no debug symbols |
| `win-release-compatible` | Release build targeting x86-64-v2 (2009+ CPUs) |
| `win-optimized` | LTO + `-march=x86-64-v3` + stripped (Haswell 2013+) |
| `win-coverage` | Debug + LLVM code-coverage instrumentation |
| `win-benchmark` | Optimised build with Google Benchmark enabled |
| `win-profile` | Optimised + frame pointers |
| `win-pgo-generate` | Instrumented build for PGO profile collection |
| `win-pgo-use` | PGO-optimised build |

List all available presets:

```bash
cmake --list-presets
```

---

## Configure + Build

### Linux

```bash
# Configure
cmake --preset debug

# Build
cmake --build --preset debug

# Test
ctest --preset debug
```

### Windows

```powershell
# Configure
cmake --preset win-debug

# Build
cmake --build --preset win-debug

# Test
ctest --preset win-debug
```

---

## Running the Binary

| Platform | Preset | Binary path |
|----------|--------|-------------|
| Linux | `debug` | `build/debug/bin/TaskSmack` |
| Linux | `release` | `build/release/bin/TaskSmack` |
| Windows | `win-debug` | `build\win-debug\bin\TaskSmack.exe` |
| Windows | `win-release` | `build\win-release\bin\TaskSmack.exe` |

The pattern is always `build/<preset>/bin/TaskSmack[.exe]`.

---

## CPU Compatibility

The `optimized` / `win-optimized` presets target x86-64-v3 (AVX2, Haswell 2013+ or Excavator 2015+). Use `release-compatible` / `win-release-compatible` for older CPUs.

Customise the target architecture with the `TASKSMACK_MARCH` CMake variable:

```bash
cmake --preset release -DTASKSMACK_MARCH=native       # Optimise for this exact CPU
cmake --preset release -DTASKSMACK_MARCH=x86-64-v2    # 2009+ CPUs
cmake --preset release -DTASKSMACK_MARCH=x86-64       # Broadest compatibility
```

---

## Sanitizer Builds (Linux Only)

### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

`ctest --preset asan-ubsan` sets `LSAN_OPTIONS` to the project suppression file
followed by any inherited `LSAN_OPTIONS` from the environment:

```
suppressions=${sourceDir}/tests/sanitizer-suppressions/lsan.supp:$penv{LSAN_OPTIONS}
```

`${sourceDir}` is the CMake Presets variable for the repository root.
Caller-provided options (verbosity, `halt_on_error`, etc.) are preserved. Because
`suppressions=` is a scalar key where the last occurrence wins, if the caller
provides their own `suppressions=` entry it will override the project's suppression
file — add entries directly to `lsan.supp` in that case.

### ThreadSanitizer

```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

`ctest --preset tsan` sets `TSAN_OPTIONS` to the project suppression file followed
by any inherited `TSAN_OPTIONS` from the environment:

```
suppressions=${sourceDir}/tests/sanitizer-suppressions/tsan.supp:$penv{TSAN_OPTIONS}
```

Caller-provided options are preserved. If the caller provides their own
`suppressions=` entry, it overrides the project's — add entries to `tsan.supp`
in that case.

---

## Benchmarks

### Build and Run

```bash
# Linux
cmake --preset benchmark
cmake --build --preset benchmark
./build/benchmark/bin/TaskSmackBenchmarks

# Windows
cmake --preset win-benchmark
cmake --build --preset win-benchmark
.\build\win-benchmark\bin\TaskSmackBenchmarks.exe
```

### JSON Output and Comparison

```bash
# Capture baseline
./build/benchmark/bin/TaskSmackBenchmarks --benchmark_format=json > baseline.json

# After changes, capture current
./build/benchmark/bin/TaskSmackBenchmarks --benchmark_format=json > current.json

# Compare (requires: pip install google-benchmark)
python -m google_benchmark.compare baseline.json current.json
```

---

## Profile-Guided Optimisation (PGO)

PGO uses real runtime behaviour to guide compiler optimisations (inlining, branch hints, layout), typically yielding 5–15 % throughput gains on hot paths.

### Automated Workflow (Recommended)

```bash
# Linux — full three-phase workflow
./tools/pgo.sh

# Run individual phases
./tools/pgo.sh generate   # Phase 1: instrumented build + profile collection
./tools/pgo.sh merge      # Phase 2: merge *.profraw → profiles/tasksmack.profdata
./tools/pgo.sh use        # Phase 3: PGO-optimised build

# Optimised binary: build/pgo-use/bin/TaskSmack
```

```powershell
# Windows — requires LLVM_ROOT set to your LLVM 22 install dir
pwsh tools/pgo.ps1

# Optimised binary: build\win-pgo-use\bin\TaskSmack.exe
```

### Manual Workflow

```bash
# Phase 1 — instrumented build
cmake --preset pgo-generate
cmake --build --preset pgo-generate

# Collect profile data (run benchmarks and/or the app itself)
mkdir -p profiles
LLVM_PROFILE_FILE="profiles/tasksmack-%p.profraw" \
    ./build/pgo-generate/bin/TaskSmackBenchmarks --benchmark_min_time=0.5

# Phase 2 — merge profiles
llvm-profdata-22 merge -sparse profiles/*.profraw -o profiles/tasksmack.profdata

# Phase 3 — PGO-optimised build
cmake --preset pgo-use
cmake --build --preset pgo-use
```

---

## Coverage Build

```bash
# Linux — generate HTML report
./tools/coverage.sh
./tools/coverage.sh --open      # Open in browser after generating

# Windows
pwsh tools/coverage.ps1
pwsh tools/coverage.ps1 -OpenReport
```

Coverage reports are written to `coverage/` (gitignored). CI heavy-checks also upload a summary to Codecov.
