# Project-wide options and cache variables.
# Declarations only — behavior driven by these lives in CompilerOptions.cmake,
# Dependencies.cmake, and StaticAnalysis.cmake.

# Keep clangd fed during local development: optionally copy compile_commands.json to the
# source root on build. Opt-in (ON in the debug/win-debug dev presets) so default configures
# keep the source tree clean (#599).
option(TASKSMACK_COPY_COMPILE_COMMANDS "Copy compile_commands.json to the source root for clangd" OFF)

set(TASKSMACK_LLVM_VERSION "22" CACHE STRING "Canonical LLVM toolchain version used for tool discovery hints")
option(TASKSMACK_ENABLE_IPO "Enable interprocedural optimization" ON)
option(TASKSMACK_ENABLE_WARNINGS "Enable extra compiler warnings" ON)
option(TASKSMACK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(TASKSMACK_BUILD_TESTS "Build tests" ON)
option(TASKSMACK_BUILD_BENCHMARKS "Build performance benchmarks" OFF)
option(TASKSMACK_ENABLE_PCH "Enable precompiled headers" ON)
option(TASKSMACK_ENABLE_CCACHE "Enable compiler caching (ccache/sccache)" ON)
option(TASKSMACK_ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(TASKSMACK_ENABLE_FETCHCONTENT_CACHE "Use a shared FetchContent/CPM cache directory instead of per-build _deps" ON)
option(TASKSMACK_ENABLE_UNITY_BUILD "Enable unity builds for TaskSmack-owned targets" OFF)
set(TASKSMACK_FETCHCONTENT_CACHE_DIR "" CACHE PATH "Override FetchContent/CPM cache directory when caching is enabled")
# TASKSMACK_MARCH is available for manual override via command line (e.g., -DTASKSMACK_MARCH=native)
# Presets specify -march directly in CMAKE_CXX_FLAGS_RELEASE for self-contained configurations
set(TASKSMACK_MARCH "" CACHE STRING "Target microarchitecture (e.g., native, x86-64-v3, x86-64-v2). Empty string means no -march flag.")
# Clang -ftime-trace: emits per-TU compile-time flamegraphs (<source>.json) for chrome://tracing.
# Enable with -DTASKSMACK_ENABLE_TIME_TRACE=ON to diagnose slow compilation hot spots.
option(TASKSMACK_ENABLE_TIME_TRACE "Enable -ftime-trace compile-time flamegraph output (Clang only)" OFF)
# Linker selection on Unix (ignored on Windows, which always uses lld-link via CMAKE_LINKER_TYPE=LLD).
# Supported values: lld (default), mold (faster incremental debug links), default (system linker).
set(TASKSMACK_LINKER "lld" CACHE STRING "Linker to use on Unix: lld, mold, or default")
set_property(CACHE TASKSMACK_LINKER PROPERTY STRINGS lld mold default)

set(TASKSMACK_EXTRA_WARNING_FLAGS "" CACHE STRING "Additional warning flags")
