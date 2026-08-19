# Minimum CMake version rationale:
#   3.25: FetchContent_Declare(SYSTEM) keyword (suppresses warnings from third-party headers)
#   3.25: CMakePresets.json schema version 6 (workflowPresets, include directives)
#   3.24: FETCHCONTENT_TRY_FIND_PACKAGE_MODE variable
#   3.29: Declared as minimum in CMakePresets.json (cmakeMinimumRequired field)
cmake_minimum_required(VERSION 3.29)

# Enable IPO policy explicitly (avoids third-party warnings when targets set IPO properties)
if(POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
endif()


project(TaskSmack VERSION 0.8.0 LANGUAGES C CXX)

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

# Keep clangd fed regardless of config: copy compile_commands.json to source root on build.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    add_custom_target(copy-compile-commands ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            "${CMAKE_SOURCE_DIR}/compile_commands.json"
        BYPRODUCTS "${CMAKE_SOURCE_DIR}/compile_commands.json"
        COMMENT "Copying compile_commands.json to source tree for clangd"
    )
endif()

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

set(TASKSMACK_EXTRA_WARNING_FLAGS "" CACHE STRING "Additional warning flags")

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

find_program(CLANG_TIDY_EXE NAMES clang-tidy
    HINTS
        "/usr/lib/llvm-${TASKSMACK_LLVM_VERSION}/bin"
        "$ENV{LLVM_ROOT}/bin"
        "$ENV{ProgramFiles}/LLVM/bin"
        "C:/Program Files/LLVM/bin"
)

find_program(CLANG_FORMAT_EXE NAMES clang-format
    HINTS
        "/usr/lib/llvm-${TASKSMACK_LLVM_VERSION}/bin"
        "$ENV{LLVM_ROOT}/bin"
        "$ENV{ProgramFiles}/LLVM/bin"
        "C:/Program Files/LLVM/bin"
)

if(TASKSMACK_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

if(TASKSMACK_ENABLE_FETCHCONTENT_CACHE)
    if(TASKSMACK_FETCHCONTENT_CACHE_DIR)
        set(_tasksmack_fetchcontent_cache "${TASKSMACK_FETCHCONTENT_CACHE_DIR}")
    elseif(DEFINED ENV{FETCHCONTENT_BASE_DIR} AND NOT "$ENV{FETCHCONTENT_BASE_DIR}" STREQUAL "")
        set(_tasksmack_fetchcontent_cache "$ENV{FETCHCONTENT_BASE_DIR}")
    else()
        # Keep the shared cache outside build trees but near the workspace
        set(_tasksmack_fetchcontent_cache "${CMAKE_SOURCE_DIR}/.cache/fetchcontent")
    endif()

    if(NOT IS_ABSOLUTE "${_tasksmack_fetchcontent_cache}")
        get_filename_component(_tasksmack_fetchcontent_cache "${_tasksmack_fetchcontent_cache}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    endif()

    if(NOT _tasksmack_fetchcontent_cache STREQUAL "")
        file(MAKE_DIRECTORY "${_tasksmack_fetchcontent_cache}")
        set(FETCHCONTENT_BASE_DIR "${_tasksmack_fetchcontent_cache}" CACHE PATH "Base dir for FetchContent downloads" FORCE)
        if(NOT CPM_SOURCE_CACHE)
            set(CPM_SOURCE_CACHE "${_tasksmack_fetchcontent_cache}" CACHE PATH "CPM.cmake source cache" FORCE)
        endif()
        message(STATUS "FetchContent cache enabled: ${FETCHCONTENT_BASE_DIR}")
    endif()
endif()

include(FetchContent)
include(GNUInstallDirs)
# Keep source-based dependencies deterministic: we rely on <dep>_SOURCE_DIR values below.
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE OPT_IN)

find_package(OpenGL REQUIRED)

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

# Security hardening flags for release builds
# Applied only for Release-type builds to avoid impacting Debug ergonomics (e.g., ASAN stack checks).
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

# Prefer header-only spdlog and std::format to sidestep MSVC fmt deprecation noise.
set(SPDLOG_HEADER_ONLY ON CACHE BOOL "Build spdlog in header-only mode" FORCE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0
    SYSTEM  # Treat as system headers to suppress warnings from spdlog
)

FetchContent_MakeAvailable(spdlog)

if(TARGET spdlog_header_only)
    target_compile_definitions(spdlog_header_only INTERFACE
        SPDLOG_USE_STD_FORMAT
        $<$<PLATFORM_ID:Windows>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>
        $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_WARN>)
endif()

if(TARGET spdlog)
    target_compile_definitions(spdlog PUBLIC
        SPDLOG_USE_STD_FORMAT
        $<$<PLATFORM_ID:Windows>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>
        $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_WARN>)
endif()

# toml++ for configuration files
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    SYSTEM  # Treat as system headers to suppress warnings
)
FetchContent_MakeAvailable(tomlplusplus)

# OpenGL + ImGui stack dependencies

# stb (image loader for runtime textures)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG f1c79c02822848a9bed4315b12c8c8f3761e1296  # Pinned to specific commit for supply-chain security
)
FetchContent_MakeAvailable(stb)

# SDL3 for windowing, input, and custom title bar support
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.4
    SYSTEM  # Treat as system headers to suppress warnings from SDL3
)

# Configure SDL3 build options (disable unnecessary components)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)  # We don't need audio
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)  # We don't need haptic
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)  # We don't need sensor
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)  # We don't need camera
set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)  # We don't need screensaver support
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)  # We don't need XTEST
set(SDL_X11_XDBE OFF CACHE BOOL "" FORCE)  # We don't need double buffering extension

FetchContent_MakeAvailable(SDL3)
set_target_properties(SDL3-static PROPERTIES FOLDER "third_party")

# Prefer the project-local environment created by tools/setup-dev.sh without
# overriding the machine's system Python.
if(NOT Python3_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/.venv/bin/python")
    set(Python3_EXECUTABLE "${CMAKE_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "Python interpreter")
endif()

# Early check for GLAD requirements (Python3 + jinja2)
# GLAD generation uses jinja2 templates and fails with cryptic errors if not installed
find_package(Python3 3.14 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import jinja2"
    RESULT_VARIABLE JINJA2_CHECK
    OUTPUT_QUIET ERROR_QUIET
)
if(NOT JINJA2_CHECK EQUAL 0)
    message(FATAL_ERROR
        "Python jinja2 module not found. Required for GLAD generation.\n"
        "Install with: pip install jinja2")
endif()

# GLAD
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v2.0.8
)
FetchContent_MakeAvailable(glad)
add_subdirectory("${glad_SOURCE_DIR}/cmake" "${glad_BINARY_DIR}/glad_cmake")
glad_add_library(glad_gl_core_33 REPRODUCIBLE EXCLUDE_FROM_ALL LOADER API gl:core=3.3)
set_target_properties(glad_gl_core_33 PROPERTIES FOLDER "third_party")

# Mark GLAD headers as SYSTEM to suppress warnings (e.g., -Wlanguage-extension-token from __int32)
get_target_property(_glad_includes glad_gl_core_33 INTERFACE_INCLUDE_DIRECTORIES)
if(_glad_includes)
    set_target_properties(glad_gl_core_33 PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_glad_includes}"
    )
endif()

# FreeType for improved font rendering (always use FetchContent to avoid noisy Findfreetype warnings)

FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG VER-2-14-3
    SYSTEM  # Treat as system headers to suppress warnings from FreeType
)

# Configure FreeType build options (disable optional dependencies)
set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
set(FT_REQUIRE_ZLIB FALSE CACHE BOOL "" FORCE)
set(FT_REQUIRE_BZIP2 FALSE CACHE BOOL "" FORCE)
set(FT_REQUIRE_PNG FALSE CACHE BOOL "" FORCE)
set(FT_REQUIRE_HARFBUZZ FALSE CACHE BOOL "" FORCE)
set(FT_REQUIRE_BROTLI FALSE CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(freetype)

set_target_properties(freetype PROPERTIES FOLDER "third_party")
# Create an alias target to match find_package(Freetype) behavior
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.7-docking
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
)
target_include_directories(imgui_lib
    SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${imgui_SOURCE_DIR}/misc/cpp
        ${imgui_SOURCE_DIR}/misc/freetype
)
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3-static glad_gl_core_33 Freetype::Freetype)
target_compile_definitions(imgui_lib PUBLIC
    IMGUI_IMPL_OPENGL_LOADER_GLAD2
    IMGUI_ENABLE_FREETYPE
)
set_target_properties(imgui_lib PROPERTIES FOLDER "third_party")

# ImPlot v1.0 (requires ImGui docking branch)
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG v1.0
)
FetchContent_MakeAvailable(implot)

add_library(implot_lib STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp
)
target_include_directories(implot_lib
    SYSTEM PUBLIC
        ${implot_SOURCE_DIR}
        ${imgui_SOURCE_DIR}
)
target_link_libraries(implot_lib PUBLIC imgui_lib)
set_target_properties(implot_lib PROPERTIES FOLDER "third_party")

# Generate version header from template
string(TOUPPER ${PROJECT_NAME} PROJECT_NAME_UPPER)
string(TOLOWER ${PROJECT_NAME} PROJECT_NAME_LOWER)

# Capture git commit (short). Fallback to "unknown" if git is unavailable.
set(GIT_COMMIT "unknown")
execute_process(
    COMMAND git rev-parse --short=8 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIT_COMMIT_RESULT
)
if(GIT_COMMIT_RESULT EQUAL 0 AND NOT GIT_COMMIT_OUTPUT STREQUAL "")
    set(GIT_COMMIT ${GIT_COMMIT_OUTPUT})
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
    @ONLY
)

set(TASKSMACK_SOURCES
    src/main.cpp
    src/Core/Application.cpp
    src/Core/PathService.cpp
    src/Core/Window.cpp
    src/UI/AssetPath.cpp
    src/UI/UILayer.cpp
    src/UI/Theme.cpp
    src/UI/ThemeLoader.cpp
    src/UI/IconLoader.cpp
    src/App/AboutLayer.cpp
    src/App/ElevationNoticeLayer.cpp
    src/App/PlatformOpen.cpp
    src/App/SettingsLayer.cpp
    src/App/ShellLayer.cpp
    src/App/TitleBarLayer.cpp
    src/App/UserConfig.cpp
    src/App/Panels/ProcessesPanel.cpp
    src/App/Panels/ProcessDetailsPanel.cpp
    src/App/Panels/SystemMetricsPanel.cpp
    src/App/Panels/CpuCoresSection.cpp
    src/App/Panels/MemorySection.cpp
    src/App/Panels/NetworkSection.cpp
    src/App/Panels/StorageSection.cpp
    src/App/Panels/GpuSection.cpp
    src/Domain/ProcessModel.cpp
    src/Domain/BackgroundSampler.cpp
    src/Domain/SystemModel.cpp
    src/Domain/StorageModel.cpp
    src/Domain/GPUModel.cpp
)

# Platform-specific sources
set(PLATFORM_SOURCES_LINUX
    src/Platform/Linux/LinuxProcessProbe.cpp
    src/Platform/Linux/LinuxProcessActions.cpp
    src/Platform/Linux/LinuxSystemProbe.cpp
    src/Platform/Linux/LinuxDiskProbe.cpp
    src/Platform/Linux/LinuxPathProvider.cpp
    src/Platform/Linux/LinuxPowerProbe.cpp
    src/Platform/Linux/LinuxGPUProbe.cpp
    src/Platform/Linux/NVMLGPUProbe.cpp
    src/Platform/Linux/DRMGPUProbe.cpp
    src/Platform/Linux/ROCmGPUProbe.cpp
    src/Platform/Linux/NetlinkSocketStats.cpp
    src/Platform/Linux/Factory.cpp
)

set(PLATFORM_SOURCES_WINDOWS
    src/Platform/Windows/WindowsProcessProbe.cpp
    src/Platform/Windows/WindowsProcessActions.cpp
    src/Platform/Windows/WindowsSystemProbe.cpp
    src/Platform/Windows/WindowsDiskProbe.cpp
    src/Platform/Windows/WindowsPathProvider.cpp
    src/Platform/Windows/WindowsPowerProbe.cpp
    src/Platform/Windows/WindowsGPUProbe.cpp
    src/Platform/Windows/DXGIGPUProbe.cpp
    src/Platform/Windows/NVMLGPUProbe.cpp
    src/Platform/Windows/PDHGPUProbe.cpp
    src/Platform/Windows/WinString.cpp
    src/Platform/Windows/Factory.cpp
)

if(UNIX AND NOT APPLE)
    list(APPEND TASKSMACK_SOURCES ${PLATFORM_SOURCES_LINUX})
elseif(WIN32)
    list(APPEND TASKSMACK_SOURCES ${PLATFORM_SOURCES_WINDOWS})
else()
    message(WARNING "Platform sources not defined for ${CMAKE_SYSTEM_NAME}")
endif()

set(TASKSMACK_HEADERS
    src/Core/Application.h
    src/Core/PathService.h
    src/Core/Window.h
    src/Core/Layer.h
    src/UI/AssetPath.h
    src/UI/UILayer.h
    src/App/ShellLayer.h
    src/App/Panels/ProcessesPanel.h
    src/Platform/Factory.h
    src/Platform/IDiskProbe.h
    src/Platform/IGPUProbe.h
    src/Platform/IPowerProbe.h
    src/Platform/IProcessProbe.h
    src/Platform/IProcessActions.h
    src/Platform/ISystemProbe.h
    src/Platform/GPUTypes.h
    src/Platform/PowerTypes.h
    src/Platform/ProcessTypes.h
    src/Domain/ProcessSnapshot.h
    src/Domain/ProcessModel.h
    src/Domain/GPUSnapshot.h
    src/Domain/GPUModel.h
)

# Platform-specific headers
if(UNIX AND NOT APPLE)
    list(APPEND TASKSMACK_HEADERS
        src/Platform/Linux/LinuxProcessProbe.h
        src/Platform/Linux/LinuxDiskProbe.h
        src/Platform/Linux/LinuxPowerProbe.h
        src/Platform/Linux/LinuxGPUProbe.h
        src/Platform/Linux/NVMLGPUProbe.h
        src/Platform/Linux/DRMGPUProbe.h
    )
elseif(WIN32)
    list(APPEND TASKSMACK_HEADERS
        src/Platform/Windows/WindowsProcessProbe.h
        src/Platform/Windows/WindowsProcessActions.h
        src/Platform/Windows/WindowsSystemProbe.h
        src/Platform/Windows/WindowsDiskProbe.h
        src/Platform/Windows/WindowsPowerProbe.h
        src/Platform/Windows/WindowsGPUProbe.h
        src/Platform/Windows/DXGIGPUProbe.h
        src/Platform/Windows/NVMLGPUProbe.h
        src/Platform/Windows/WinString.h
    )
endif()

if(CLANG_TIDY_EXE)
    # Generate a clean compile_commands.json for clang-tidy in a dedicated directory, stripping flags
    # that clang-tidy cannot handle (C++20 module @*.modmap dependency files, PCH flags).
    # Writing to a *separate* directory preserves the live compile_commands.json that clangd watches;
    # clang-tidy tools point -p at the dedicated directory so clangd is never disturbed.
    set(TASKSMACK_CLANG_TIDY_COMPDB_DIR "${CMAKE_BINARY_DIR}/clang-tidy-compdb")
    if(WIN32)
        add_custom_target(generate-clang-tidy-compile-commands
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}
            COMMAND powershell -NoProfile -Command
                "(Get-Content '${CMAKE_BINARY_DIR}/compile_commands.json' -Raw) -replace '@[^ ]*\\.modmap', '' -replace '-fmodule-output=[^ ]*', '' -replace '-Xclang -include-pch -Xclang [^ ]*', '' -replace '-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*', '' -replace '-Xclang -fno-pch-timestamp', '' | Set-Content '${TASKSMACK_CLANG_TIDY_COMPDB_DIR}/compile_commands.json' -NoNewline"
            DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
            COMMENT "Writing clang-tidy compile_commands.json (module/PCH flags stripped)"
        )
    else()
        add_custom_target(generate-clang-tidy-compile-commands
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_BINARY_DIR}/compile_commands.json
                ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}/compile_commands.json
            COMMAND sed -i
                -e "s/@[^ ]*\\.modmap//g"
                -e "s/-fmodule-output=[^ ]*//g"
                -e "s/-Xclang -include-pch -Xclang [^ ]*//g"
                -e "s/-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*//g"
                -e "s/-Xclang -fno-pch-timestamp//g"
                ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}/compile_commands.json
            DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
            COMMENT "Writing clang-tidy compile_commands.json (module/PCH flags stripped)"
        )
    endif()

    add_custom_target(run-clang-tidy
        COMMAND ${CLANG_TIDY_EXE}
            --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
            -p ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}
            --extra-arg=-std=c++23
            --extra-arg=-Wno-unknown-warning-option
            ${TASKSMACK_SOURCES}
            ${TASKSMACK_HEADERS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy on project sources"
    )
    # Dependencies:
    # - generate-clang-tidy-compile-commands: strips C++20 module flags from compile_commands.json
    # - copy-compile-commands: ensures compile_commands.json is in source root
    # - glad_gl_core_33: ensures GLAD headers are generated before clang-tidy runs
    add_dependencies(run-clang-tidy generate-clang-tidy-compile-commands copy-compile-commands glad_gl_core_33)
endif()

if(CLANG_FORMAT_EXE)
    add_custom_target(run-clang-format
        COMMAND ${CLANG_FORMAT_EXE} -i
            ${TASKSMACK_SOURCES}
            ${TASKSMACK_HEADERS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Applying clang-format to project sources"
    )
endif()

# Windows resource file (icon, version info, manifest)
if(WIN32)
    set(TASKSMACK_RC_IN ${CMAKE_SOURCE_DIR}/assets/tasksmack.rc.in)
    set(TASKSMACK_RC_OUT ${CMAKE_BINARY_DIR}/tasksmack.rc)
    set(TASKSMACK_ICO_FILE ${CMAKE_SOURCE_DIR}/assets/icons/tasksmack.ico)

    if(NOT EXISTS ${TASKSMACK_RC_IN})
        message(FATAL_ERROR "Windows resource template not found: ${TASKSMACK_RC_IN}\n"
                           "This is required for Windows builds.")
    endif()

    # Check if ICO file exists and set the define for configure_file
    if(EXISTS ${TASKSMACK_ICO_FILE})
        set(TASKSMACK_HAS_ICON TRUE)
        message(STATUS "Including application icon: ${TASKSMACK_ICO_FILE}")
    else()
        set(TASKSMACK_HAS_ICON FALSE)
        message(STATUS "Icon file not found (run tools/generate-icons.ps1 to create): ${TASKSMACK_ICO_FILE}")
    endif()

    # Configure the RC file with version info
    configure_file(${TASKSMACK_RC_IN} ${TASKSMACK_RC_OUT} @ONLY)
    list(APPEND TASKSMACK_SOURCES ${TASKSMACK_RC_OUT})
    message(STATUS "Configured Windows resource file with version ${PROJECT_VERSION}")

    # Configure and use our custom manifest for DPI awareness and Windows compatibility
    set(TASKSMACK_MANIFEST_IN ${CMAKE_SOURCE_DIR}/assets/app.manifest.in)
    set(TASKSMACK_MANIFEST_OUT ${CMAKE_BINARY_DIR}/app.manifest)
    if(NOT EXISTS ${TASKSMACK_MANIFEST_IN})
        message(FATAL_ERROR "Application manifest template not found: ${TASKSMACK_MANIFEST_IN}\n"
                           "This is required for Windows builds.")
    endif()

    configure_file(${TASKSMACK_MANIFEST_IN} ${TASKSMACK_MANIFEST_OUT} @ONLY)
    message(STATUS "Configured application manifest with version ${PROJECT_VERSION}")
endif()

# Enable unity compilation only after third-party targets have been declared. Setting
# CMAKE_UNITY_BUILD in a preset would also combine dependency sources and can trigger
# collisions between their private macros and identifiers.
if(TASKSMACK_ENABLE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 16)
endif()

add_executable(TaskSmack WIN32
    ${TASKSMACK_SOURCES}
    ${TASKSMACK_HEADERS}
)

# Compile-time flamegraph tracing is limited to TaskSmack sources so FetchContent
# dependencies do not emit unrelated traces or incur tracing overhead.
if(TASKSMACK_ENABLE_TIME_TRACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(TaskSmack PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-ftime-trace>")
        message(STATUS "Compile-time tracing enabled (-ftime-trace). Run tools/time-trace.sh after build.")
    else()
        message(WARNING "TASKSMACK_ENABLE_TIME_TRACE requires Clang. Ignoring on ${CMAKE_CXX_COMPILER_ID}.")
    endif()
endif()
# Embed custom manifest on Windows (for DPI awareness and OS compatibility)
if(WIN32 AND EXISTS ${CMAKE_BINARY_DIR}/app.manifest)
    target_link_options(TaskSmack PRIVATE
        "LINKER:/MANIFESTINPUT:${CMAKE_BINARY_DIR}/app.manifest"
    )
endif()

target_compile_features(TaskSmack PRIVATE cxx_std_23)

# Ensure libc++/pthread/lld are applied across all configs on Linux when using Clang,
# so optimized and RelWithDebInfo builds keep the same link toolchain as Debug.
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(TaskSmack PRIVATE -stdlib=libc++ -pthread)
    if(_ts_linker_flag)
        target_link_options(TaskSmack PRIVATE -stdlib=libc++ -pthread ${_ts_linker_flag})
    else()
        target_link_options(TaskSmack PRIVATE -stdlib=libc++ -pthread)
    endif()
endif()

# Precompiled headers
if(TASKSMACK_ENABLE_PCH)
    include(cmake/PrecompiledHeaders.cmake)
    target_precompile_headers(TaskSmack PRIVATE ${TASKSMACK_PCH_HEADERS})
endif()

tasksmack_apply_default_warnings(TaskSmack)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(TaskSmack PRIVATE
        $<$<CONFIG:Debug>:-glldb>
        $<$<CONFIG:RelWithDebInfo>:-glldb>
    )
endif()

if(WIN32)
    # Windows SDK version: GetIfTable2 and MIB_IF_TABLE2 are available on Vista+,
    # but we intentionally target Windows 10+ via _WIN32_WINNT=0x0A00 / WINVER=0x0A00.
    # NTDDI_VERSION is required for iphlpapi.h to include netioapi.h (GetIfTable2 APIs).
    target_compile_definitions(TaskSmack PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NTDDI_VERSION=0x0A000000
    )
endif()

target_include_directories(TaskSmack PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${stb_SOURCE_DIR}
)

target_link_libraries(TaskSmack PRIVATE
    spdlog::spdlog_header_only
    imgui_lib
    implot_lib
    tomlplusplus::tomlplusplus
    OpenGL::GL
)

if(WIN32)
    set(TASKSMACK_WINDOWS_SYSTEM_LIBS
        iphlpapi
        ws2_32
        dxgi
        gdi32
        version
    )

    target_link_libraries(TaskSmack PRIVATE ${TASKSMACK_WINDOWS_SYSTEM_LIBS})
elseif(UNIX AND NOT APPLE)
    target_link_libraries(TaskSmack PRIVATE dl)
endif()

# For multi-config generators (e.g., Visual Studio, Ninja Multi-Config), include
# $<CONFIG> in the output path to prevent Debug/Release binary collisions.
# For single-config generators (Ninja), emit directly to bin/ so script and doc
# paths (e.g., build/pgo-use/bin/TaskSmack) remain correct.
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_is_multi_config)
    set_target_properties(TaskSmack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
else()
    set_target_properties(TaskSmack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()
unset(_is_multi_config)

# Copy assets (fonts and themes) to build directory
set(TASKSMACK_ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets")
add_custom_command(TARGET TaskSmack POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TaskSmack>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${TASKSMACK_ASSETS_DIR}/fonts"
        "$<TARGET_FILE_DIR:TaskSmack>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TaskSmack>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${TASKSMACK_ASSETS_DIR}/icons"
        "$<TARGET_FILE_DIR:TaskSmack>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TaskSmack>/assets/themes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${TASKSMACK_ASSETS_DIR}/themes"
        "$<TARGET_FILE_DIR:TaskSmack>/assets/themes"
    COMMENT "Copying assets (fonts, themes) to build directory"
)

# Copy FreeType runtime (if shared/imported) next to the executable for portable layout
if(TARGET Freetype::Freetype)
    get_target_property(_ft_alias Freetype::Freetype ALIASED_TARGET)
    if(_ft_alias)
        set(_ft_target ${_ft_alias})
    else()
        set(_ft_target Freetype::Freetype)
    endif()

    get_target_property(_ft_imported ${_ft_target} IMPORTED)
    if(_ft_imported)
        set(_ft_runtime_file $<TARGET_FILE:${_ft_target}>)
        set(_ft_needs_copy ON)
    else()
        get_target_property(_ft_type ${_ft_target} TYPE)
        if(_ft_type STREQUAL "SHARED_LIBRARY" OR _ft_type STREQUAL "MODULE_LIBRARY")
            set(_ft_runtime_file $<TARGET_FILE:${_ft_target}>)
            set(_ft_needs_copy ON)
        endif()
    endif()

    if(_ft_needs_copy)
        add_custom_command(TARGET TaskSmack POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_ft_runtime_file}
                $<TARGET_FILE_DIR:TaskSmack>
            COMMENT "Copying FreeType runtime to build directory")

        install(TARGETS ${_ft_target}
            RUNTIME DESTINATION .
            LIBRARY DESTINATION .
        )
    endif()
endif()

# Note: SDL3 handles X11/Wayland internally, no explicit X11 linking needed

install(TARGETS TaskSmack
    BUNDLE DESTINATION .
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
    ARCHIVE DESTINATION .
)

# Install static libs alongside the binary for local/portable layout
install(TARGETS imgui_lib implot_lib glad_gl_core_33
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
    ARCHIVE DESTINATION .
)

# Install runtime assets (fonts/themes/icons) using GNUInstallDirs for FHS compliance.
# On Linux this resolves to share/<project_name_lower>/assets/; on Windows the prefix root is typically
# the install directory so share/<project_name_lower>/assets/ lives alongside the binary.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/fonts
        ${CMAKE_SOURCE_DIR}/assets/themes
        ${CMAKE_SOURCE_DIR}/assets/icons
    DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME_LOWER}/assets
)

if(TASKSMACK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TASKSMACK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# CPack configuration for packaging
set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_NAME} - TaskSmack")
set(CPACK_PACKAGE_DESCRIPTION "TaskSmack - Modern cross-platform system monitor")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/mgradwohl/tasksmack")
set(CPACK_PACKAGE_VENDOR "TaskSmack Contributors")
set(CPACK_PACKAGE_CONTACT "mgradwohl@users.noreply.github.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/assets/icons/tasksmack.ico")

# Output directory for packages
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")

# Archive generators (cross-platform)
set(CPACK_GENERATOR "ZIP;TGZ")

# Platform-specific generators
if(WIN32)
    list(APPEND CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH ON)
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/assets/icons/tasksmack.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\${PROJECT_NAME}.exe")
    set(CPACK_NSIS_DISPLAY_NAME "${PROJECT_NAME}")
    set(CPACK_NSIS_HELP_LINK "https://github.com/mgradwohl/tasksmack")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/mgradwohl/tasksmack")
elseif(APPLE)
    list(APPEND CPACK_GENERATOR "DragNDrop")
elseif(UNIX)
    # Check for dpkg (Debian/Ubuntu)
    find_program(DPKG_PROGRAM dpkg)
    if(DPKG_PROGRAM)
        list(APPEND CPACK_GENERATOR "DEB")
        set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
        set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
        set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsdl3-0, libx11-6, libfreetype6")
    endif()
    # Check for rpmbuild (Red Hat/Fedora)
    find_program(RPMBUILD_PROGRAM rpmbuild)
    if(RPMBUILD_PROGRAM)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()
endif()

include(CPack)
