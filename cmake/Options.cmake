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
# The headless PGO UI training driver, TaskSmackUiTraining (#880). Cheap: it links the app's own
# objects (TaskSmackApp) and compiles only its few sources; ON so its ctest smoke test runs everywhere.
option(TASKSMACK_BUILD_UI_TRAINING "Build the headless PGO UI training driver" ON)
option(TASKSMACK_ENABLE_PCH "Enable precompiled headers" ON)
# Windows clang Debug only: -gcodeview-ghash plus lld-link /DEBUG:GHASH for faster PDB links (#1875).
option(TASKSMACK_ENABLE_GHASH "Windows clang Debug: global type hashes for faster lld-link PDB writes" OFF)
option(TASKSMACK_ENABLE_CCACHE "Enable compiler caching (ccache/sccache)" ON)
option(TASKSMACK_ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(TASKSMACK_ENABLE_FETCHCONTENT_CACHE "Use a shared FetchContent cache directory instead of per-build _deps" ON)
option(TASKSMACK_ENABLE_UNITY_BUILD "Enable unity builds for TaskSmack-owned targets" OFF)
# Windows: whether the .zip and the NSIS installer carry TaskSmackHelper.exe, the elevated helper (#2550).
# Only for Authenticode-signed builds (#2302): packaging then refuses a helper or TaskSmack.exe that
# isn't validly signed by one Artifact Signing signer (cmake/VerifyWindowsSignatures.cmake). Off, the packages are as
# before and TaskSmack offers no "Retry as administrator"; `cmake --install` installs it either way.
option(TASKSMACK_PACKAGE_WINDOWS_HELPER "Windows: package TaskSmackHelper.exe (Authenticode-signed builds only)" OFF)
# The publisher the gate requires: the signer certificate's common name, as UAC shows it (#2302: the Azure
# Artifact Signing certificate profile's subject; docs/code-signing-policy.md).
set(TASKSMACK_EXPECTED_SIGNER "Matt Gradwohl" CACHE STRING "Windows: the common name TaskSmackHelper.exe's signer must have to be packaged")
# And that it's an Azure Artifact Signing certificate, not a test or internal one that also says CN=Matt Gradwohl
# (#2990): the signer certificate's issuer and one of its extended key usages, as PowerShell -like wildcards.
# Artifact Signing's certificates rotate every few days and its issuing CA rolls over now and then, so these
# match what stays the same: today's issuer is "CN=Microsoft ID Verified CS EOC CA 04, O=Microsoft Corporation,
# C=US", and every certificate carries Artifact Signing's 1.3.6.1.4.1.311.97.* usages beside Code Signing.
# Empty means the default; cmake/VerifyWindowsSignatures.cmake has the same defaults.
set(TASKSMACK_EXPECTED_ISSUER "CN=Microsoft ID Verified CS [AE]OC CA [0-9][0-9], O=Microsoft Corporation, C=US" CACHE STRING
    "Windows: a -like pattern the issuer of TaskSmackHelper.exe's signer certificate must match to be packaged")
set(TASKSMACK_EXPECTED_EKU "1.3.6.1.4.1.311.97.*" CACHE STRING
    "Windows: a -like pattern one extended key usage of TaskSmackHelper.exe's signer certificate must match to be packaged")
# A pipeline that signs the packaged .zip instead of the build's executables (#2302: release.yml's sign-windows
# signs the executables inside the .zip): CPack then packages the helper unsigned in the .zip, without the gate,
# and the pipeline must run cmake/VerifyWindowsSignatures.cmake on the signed .zip's contents before publishing
# it. The NSIS installer can't be signed inside afterwards, so it keeps the gate either way.
option(TASKSMACK_SIGN_AFTER_PACKAGING "Windows: the .zip is Authenticode-signed after packaging, so check its signatures then, not before" OFF)
set(TASKSMACK_FETCHCONTENT_CACHE_DIR "" CACHE PATH "Override FetchContent cache directory when caching is enabled")
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

# Linux: link libc++/libc++abi statically into TaskSmack's executables, so a packaged build doesn't
# depend on the distro's (older) libc++1 (#1398). ON by default for Release, the configuration every
# packaging preset uses; see tasksmack_apply_linux_toolchain() in cmake/CompilerOptions.cmake.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_ts_static_libcxx_default ON)
else()
    set(_ts_static_libcxx_default OFF)
endif()
option(TASKSMACK_STATIC_LIBCXX "Linux: link libc++ and libc++abi statically (default ON for Release)" ${_ts_static_libcxx_default})
unset(_ts_static_libcxx_default)

set(TASKSMACK_EXTRA_WARNING_FLAGS "" CACHE STRING "Additional warning flags")
