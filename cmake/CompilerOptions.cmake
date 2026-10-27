# Toolchain, warning, hardening, and build-speed configuration.
# Must be included after Options.cmake and before Dependencies.cmake so global
# flags (coverage, hardening, compiler launcher) apply to third-party builds too.

# Windows: Configure MSVC runtime library linkage for Clang.
# Debug keeps the DLL runtime (/MDd). Every other configuration links the CRT and C++ runtime
# statically (/MT): the release .zip ships just TaskSmack.exe, and with the DLL runtime a PC
# without the Visual C++ Redistributable failed to start it ("MSVCP140.dll was not found", #1398).
# Set here, before Dependencies.cmake, so SDL3, FreeType, ImGui, googletest and every other
# FetchContent target are compiled against the same runtime (CMAKE_POLICY_DEFAULT_CMP0091 makes
# that hold for dependencies whose cmake_minimum_required predates the policy).
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")
endif()

# Windows clang Debug builds: global type hashes in the CodeView debug info (-gcodeview-ghash), which
# lld-link then merges in parallel when writing the PDB (/DEBUG:GHASH). Debugging is unchanged; the
# objects get a .debug$H section. Set before Dependencies.cmake so third-party objects carry the
# hashes too. CI's Windows debug build turns it on (#1875).
if(TASKSMACK_ENABLE_GHASH AND WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options("$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C,CXX>>:-gcodeview-ghash>")
    add_link_options("$<$<CONFIG:Debug>:LINKER:/DEBUG:GHASH>")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

# Compiler caching (ccache/sccache) - required for development
if(TASKSMACK_ENABLE_CCACHE)
    # Self-heal stale cache entries (common on Windows package upgrades)
    # so find_program can re-discover valid launcher paths.
    if(DEFINED CCACHE_PROGRAM AND CCACHE_PROGRAM AND NOT EXISTS "${CCACHE_PROGRAM}")
        unset(CCACHE_PROGRAM CACHE)
    endif()
    if(DEFINED SCCACHE_PROGRAM AND SCCACHE_PROGRAM AND NOT EXISTS "${SCCACHE_PROGRAM}")
        unset(SCCACHE_PROGRAM CACHE)
    endif()

    find_program(CCACHE_PROGRAM ccache)
    find_program(SCCACHE_PROGRAM sccache)
    if(SCCACHE_PROGRAM)
        message(STATUS "Using sccache: ${SCCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
    elseif(CCACHE_PROGRAM)
        message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    else()
        message(WARNING "Neither ccache nor sccache found. Install ccache for faster rebuilds:\n"
            "  Linux: sudo apt install ccache\n"
            "  macOS: brew install ccache\n"
            "  Windows: choco install ccache OR scoop install ccache")
    endif()
endif()

# Code coverage support (Clang only)
if(TASKSMACK_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(STATUS "Code coverage enabled")
        add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
        add_link_options(-fprofile-instr-generate -fcoverage-mapping)
        # Continuous mode (#3108): with %c in LLVM_PROFILE_FILE the profile is opened and mapped at
        # startup and the counters are written through the mapping, so tasksmack-helper's counts survive
        # its privilege drop and seccomp filter, which refuse the usual write at exit. On Linux (ELF)
        # that needs the counters relocated at run time.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_compile_options("SHELL:-mllvm -runtime-counter-relocation")
            # Atomic counter updates (#3165): the samplers call the same functions from several threads,
            # and plain increments lose updates under contention. A line whose count is derived by
            # subtracting one counter from another then came out 0, or wrapped negative (24 to 34 such
            # lines a run), so the same source's coverage moved by a few lines between runs and flipped
            # main around the 95% floor.
            add_compile_options(-fprofile-update=atomic)
        endif()
    else()
        message(WARNING "Code coverage is only supported with Clang. Disabling coverage.")
        set(TASKSMACK_ENABLE_COVERAGE OFF)
    endif()
endif()

function(tasksmack_apply_default_warnings target_name)
    if(NOT TASKSMACK_ENABLE_WARNINGS)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
        # Baseline warnings - high signal, low noise for modern C++
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wformat=2
            -Wimplicit-fallthrough
            -Wnull-dereference
            -Wdouble-promotion
            -Wcast-align
            -Wundef
            -Werror=return-type
            $<$<BOOL:${TASKSMACK_WARNINGS_AS_ERRORS}>:-Werror>
        )

        # Platform-specific warning adjustments
        if(WIN32)
            # Suppress noise from MSVC STL and Windows SDK headers
            target_compile_options(${target_name} PRIVATE
                -Wno-unknown-pragmas
                -Wno-nonportable-system-include-path
            )
        else()
            # Linux/macOS-specific warnings
            target_compile_options(${target_name} PRIVATE
                -Wmisleading-indentation
            )
        endif()

        # Apply extra warning flags if specified
        if(TASKSMACK_EXTRA_WARNING_FLAGS)
            target_compile_options(${target_name} PRIVATE ${TASKSMACK_EXTRA_WARNING_FLAGS})
        endif()
    elseif(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /permissive-
            /EHsc
            $<$<BOOL:${TASKSMACK_WARNINGS_AS_ERRORS}>:/WX>
        )

        # Apply extra warning flags if specified
        if(TASKSMACK_EXTRA_WARNING_FLAGS)
            target_compile_options(${target_name} PRIVATE ${TASKSMACK_EXTRA_WARNING_FLAGS})
        endif()
    endif()
endfunction()

# Text encoding (#1648): every TaskSmack-owned target reads its sources as UTF-8 and stores its
# narrow string literals as UTF-8, whatever the build machine's code page; Windows targets also
# define UNICODE/_UNICODE, so an unsuffixed Win32 macro (CreateFile, LoadImage, ...) resolves to
# its W function and passing it a narrow string fails to compile. Per target, never global:
# FetchContent dependencies (SDL, imgui, glad, freetype, ...) keep their own settings.
#
# clang-cl/MSVC: /utf-8 sets both the source and the execution charset. clang/gcc: the source
# charset is -finput-charset; clang accepts -fexec-charset only as UTF-8 (its only execution
# charset), and gcc accepts it too, so it's checked rather than assumed. C and C++ only: the same
# target also compiles the Windows .rc file, whose compiler takes neither flag.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR (MSVC AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT))
    set(TASKSMACK_TEXT_ENCODING_FLAGS /utf-8)
else()
    include(CheckCXXCompilerFlag)
    set(TASKSMACK_TEXT_ENCODING_FLAGS -finput-charset=UTF-8)
    check_cxx_compiler_flag(-fexec-charset=UTF-8 TASKSMACK_HAS_FEXEC_CHARSET_UTF8)
    if(TASKSMACK_HAS_FEXEC_CHARSET_UTF8)
        list(APPEND TASKSMACK_TEXT_ENCODING_FLAGS -fexec-charset=UTF-8)
    endif()
endif()

function(tasksmack_apply_text_encoding target)
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:${TASKSMACK_TEXT_ENCODING_FLAGS}>")
    if(WIN32)
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)
    endif()
endfunction()

if(TASKSMACK_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "Configuring TaskSmack with clang: ${CMAKE_CXX_COMPILER}")
else()
    message(WARNING "TaskSmack is validated primarily with clang; proceeding with ${CMAKE_CXX_COMPILER_ID}.")
endif()

# Linker selection on Unix.
# TASKSMACK_LINKER controls which linker is used: lld (default), mold (faster incremental), default (system).
# On Windows the linker is always lld-link (set via CMAKE_LINKER_TYPE=LLD in CMakePresets.json).
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(NOT TASKSMACK_LINKER STREQUAL "lld"
            AND NOT TASKSMACK_LINKER STREQUAL "mold"
            AND NOT TASKSMACK_LINKER STREQUAL "default")
        message(FATAL_ERROR "Invalid TASKSMACK_LINKER='${TASKSMACK_LINKER}'. Expected one of: lld, mold, default.")
    endif()

    if(TASKSMACK_LINKER STREQUAL "mold")
        set(_ts_linker_flag "-fuse-ld=mold")
        message(STATUS "Linker: mold (opt-in)")
    elseif(TASKSMACK_LINKER STREQUAL "default")
        set(_ts_linker_flag "")
        message(STATUS "Linker: system default")
    else()
        set(_ts_linker_flag "-fuse-ld=lld")
        message(STATUS "Linker: lld (default)")
    endif()
endif()

# Target microarchitecture override. TASKSMACK_MARCH is declared (empty by default) in
# cmake/Options.cmake; presets that need specific tuning (optimized, release-compatible,
# pgo-*) bake -march directly into CMAKE_CXX_FLAGS_RELEASE instead, so this only matters for
# presets that don't already set one -- e.g. `cmake --preset release -DTASKSMACK_MARCH=native`,
# as documented in CONTRIBUTING.md's CPU Compatibility section. Applied globally (like the
# security hardening flags below) so third-party FetchContent targets built in the same
# configuration stay ABI/ISA-consistent with TaskSmack's own code.
if(TASKSMACK_MARCH)
    # CMake treats ';' as a list separator even inside a value substituted into a larger
    # unquoted token (e.g. -march=${TASKSMACK_MARCH}), so a value like "x86-64;-Wall" would
    # silently expand into two separate compiler flags -- bypassing the single-flag
    # check_cxx_compiler_flag() validation below entirely, since check_cxx_compiler_flag
    # itself also treats a ';'-containing value as a list of flags to validate together
    # rather than one candidate flag. Verified locally: TASKSMACK_MARCH=x86-64;-Wall passed
    # the (list-based) check and injected -march=x86-64 and -Wall as two separate flags into
    # the actual build. Reject any embedded ';' up front instead.
    if(TASKSMACK_MARCH MATCHES ";")
        message(FATAL_ERROR "TASKSMACK_MARCH must be a single value with no ';' in it -- got "
            "'${TASKSMACK_MARCH}'. CMake list-separator semantics would otherwise let this "
            "expand into multiple compiler flags instead of one -march argument.")
    endif()
    include(CheckCXXCompilerFlag)
    # Cache variable name must depend on the value: check_cxx_compiler_flag() only evaluates
    # once per distinct result variable, so a fixed name would return a stale answer if
    # TASKSMACK_MARCH changes across reconfigures of the same build directory.
    string(MAKE_C_IDENTIFIER "TASKSMACK_MARCH_SUPPORTED_${TASKSMACK_MARCH}" _ts_march_check_var)
    check_cxx_compiler_flag("-march=${TASKSMACK_MARCH}" ${_ts_march_check_var})
    if(NOT ${_ts_march_check_var})
        message(FATAL_ERROR "TASKSMACK_MARCH='${TASKSMACK_MARCH}' is not a microarchitecture "
            "${CMAKE_CXX_COMPILER_ID} accepts via -march. Try 'native', 'x86-64-v2', or "
            "'x86-64-v3', or leave TASKSMACK_MARCH unset to use the preset's own setting.")
    endif()
    add_compile_options(-march=${TASKSMACK_MARCH})
    message(STATUS "Target microarchitecture: -march=${TASKSMACK_MARCH} (TASKSMACK_MARCH override)")
endif()

# Linux Release builds (the release, release-compatible, optimized, benchmark and pgo-* presets;
# `release` is what the .deb/.tar.gz are built from) link libc++ and libc++abi statically
# (TASKSMACK_STATIC_LIBCXX). The apt.llvm.org clang-23 build imports libc++ symbols that the distro
# libc++1 on Ubuntu 24.04 (LLVM 18) doesn't have, e.g. std::__1::__hash_memory, so a .deb that
# depended on the system libc++1 failed to start on a stock system (#1398). Clang's
# -static-libstdc++ applies to whichever C++ library -stdlib picks, so with -stdlib=libc++ it
# links libc++.a (which pulls in libc++abi.a); `readelf -d` then lists no libc++*.so. Debug,
# RelWithDebInfo and the sanitizer presets keep the shared libc++: MSan needs an instrumented
# libc++, and the other sanitizers' interceptors are only exercised against the shared one.
function(tasksmack_apply_linux_toolchain target)
    if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE -stdlib=libc++ -pthread)
        target_link_options(${target} PRIVATE -stdlib=libc++ -pthread ${_ts_linker_flag})
        if(TASKSMACK_STATIC_LIBCXX)
            target_link_options(${target} PRIVATE -static-libstdc++)
        endif()
    endif()
endfunction()

# Security hardening flags for release builds
# Applied only for Release-type builds to avoid impacting Debug ergonomics (e.g., ASAN stack checks).
# NOTE: this assumes a single-config generator, true for every preset in CMakePresets.json
# (all use Ninja) - CMAKE_BUILD_TYPE is empty at configure time under a multi-config
# generator (Ninja Multi-Config, Visual Studio), so these flags would silently never apply
# to a Release configuration if the project were ever configured directly with one instead
# of a preset.
get_property(_ts_is_multi_config_generator GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_ts_is_multi_config_generator)
    message(WARNING "Security hardening flags are gated on CMAKE_BUILD_TYPE and will not apply "
                     "to any configuration under this multi-config generator (${CMAKE_GENERATOR}). "
                     "Use one of the single-config presets in CMakePresets.json instead.")
endif()
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # -fstack-protector-strong: instrument functions with stack buffers/alloca against overflows
        # -D_FORTIFY_SOURCE=2: enable glibc buffer-overflow checks for common string/memory functions
        # -fPIE/-pie: produce a position-independent executable (enables ASLR)
        add_compile_options(-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE)
        add_link_options(-pie)
        message(STATUS "Security hardening flags enabled (Linux release)")
    elseif(WIN32)
        # -fstack-protector-strong: stack canaries via clang on Windows
        # ASLR (/DYNAMICBASE) is enabled by default by lld-link on Windows
        add_compile_options(-fstack-protector-strong)
        message(STATUS "Security hardening flags enabled (Windows release)")
    endif()
endif()

# Extra hardening for a target that runs as root on Linux: tasksmack-helper (#2640). A small, closed
# target, so it takes flags the UI binary isn't ready for, on top of the release flags above:
#   -Wl,-z,relro,-z,now: full RELRO, every symbol bound at load, so the GOT is read-only (lld's default
#                        is partial RELRO with lazy binding), in every build type
#   -fstack-clash-protection: probe each stack page, so a large allocation can't jump the guard page
#   -fcf-protection=full (x86-64) / -mbranch-protection=standard (AArch64): CET IBT+SHSTK or BTI+PAC
#   -D_FORTIFY_SOURCE=3: also checks sizes known only at run time (needs optimization: release only)
#   _LIBCPP_HARDENING_MODE_FAST: libc++'s cheap bounds and precondition checks
function(tasksmack_apply_privileged_hardening target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    target_link_options(${target} PRIVATE -Wl,-z,relro,-z,now)
    target_compile_options(${target} PRIVATE -fstack-clash-protection)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        target_compile_options(${target} PRIVATE -fcf-protection=full)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        target_compile_options(${target} PRIVATE -mbranch-protection=standard)
    endif()
    target_compile_definitions(${target} PRIVATE _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
    if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        # After the directory-wide -D_FORTIFY_SOURCE=2 above, so it replaces it
        target_compile_options(${target} PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
    endif()
endfunction()
