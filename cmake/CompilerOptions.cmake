# Toolchain, warning, hardening, and build-speed configuration.
# Must be included after Options.cmake and before Dependencies.cmake so global
# flags (coverage, hardening, compiler launcher) apply to third-party builds too.

# Windows: Configure MSVC runtime library linkage for Clang
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Use dynamic MSVC runtime (/MD for Release, /MDd for Debug)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
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

function(tasksmack_apply_linux_toolchain target)
    if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE -stdlib=libc++ -pthread)
        if(_ts_linker_flag)
            target_link_options(${target} PRIVATE -stdlib=libc++ -pthread ${_ts_linker_flag})
        else()
            target_link_options(${target} PRIVATE -stdlib=libc++ -pthread)
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
