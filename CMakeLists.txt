cmake_minimum_required(VERSION 3.28)

# Enable IPO policy explicitly (avoids third-party warnings when targets set IPO properties)
if(POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
endif()

# Allow FetchContent_Populate for dependencies without CMakeLists.txt (imgui, implot)
cmake_policy(SET CMP0169 OLD)

project(TaskSmack VERSION 0.2.0 LANGUAGES C CXX)

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

option(TASKSMACK_ENABLE_IPO "Enable interprocedural optimization" ON)
option(TASKSMACK_ENABLE_WARNINGS "Enable extra compiler warnings" ON)
option(TASKSMACK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(TASKSMACK_BUILD_TESTS "Build tests" ON)
option(TASKSMACK_ENABLE_PCH "Enable precompiled headers" ON)
option(TASKSMACK_ENABLE_CCACHE "Enable compiler caching (ccache/sccache)" ON)
option(TASKSMACK_ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(TASKSMACK_ENABLE_FETCHCONTENT_CACHE "Use a shared FetchContent/CPM cache directory instead of per-build _deps" ON)
set(TASKSMACK_FETCHCONTENT_CACHE_DIR "" CACHE PATH "Override FetchContent/CPM cache directory when caching is enabled")
# TASKSMACK_MARCH is available for manual override via command line (e.g., -DTASKSMACK_MARCH=native)
# Presets specify -march directly in CMAKE_CXX_FLAGS_RELEASE for self-contained configurations
set(TASKSMACK_MARCH "" CACHE STRING "Target microarchitecture (e.g., native, x86-64-v3, x86-64-v2). Empty string means no -march flag.")

# Compiler caching (ccache/sccache) - required for development
if(TASKSMACK_ENABLE_CCACHE)
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
        "/usr/lib/llvm-21/bin"
        "$ENV{LLVM_ROOT}/bin"
        "$ENV{ProgramFiles}/LLVM/bin"
        "C:/Program Files/LLVM/bin"
)

find_program(CLANG_FORMAT_EXE NAMES clang-format
    HINTS
        "/usr/lib/llvm-21/bin"
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
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE ALWAYS)

find_package(OpenGL REQUIRED)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "Configuring TaskSmack with clang: ${CMAKE_CXX_COMPILER}")
else()
    message(WARNING "TaskSmack is validated primarily with clang; proceeding with ${CMAKE_CXX_COMPILER_ID}.")
endif()

# Prefer header-only spdlog and std::format to sidestep MSVC fmt deprecation noise.
set(SPDLOG_HEADER_ONLY ON CACHE BOOL "Build spdlog in header-only mode" FORCE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
    SYSTEM  # Treat as system headers to suppress warnings from spdlog
)

FetchContent_MakeAvailable(spdlog)

if(TARGET spdlog_header_only)
    target_compile_definitions(spdlog_header_only INTERFACE
        SPDLOG_USE_STD_FORMAT
        $<$<PLATFORM_ID:Windows>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>)
endif()

if(TARGET spdlog)
    target_compile_definitions(spdlog PUBLIC
        SPDLOG_USE_STD_FORMAT
        $<$<PLATFORM_ID:Windows>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>)
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
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG f1c79c02822848a9bed4315b12c8c8f3761e1296  # Pinned to specific commit for supply-chain security
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

# GLFW
find_package(glfw3 3.4 QUIET)
if(NOT glfw3_FOUND)
    FetchContent_Declare(
        glfw3
        DOWNLOAD_EXTRACT_TIMESTAMP OFF
        URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    )
    FetchContent_GetProperties(glfw3)
    if(NOT glfw3_POPULATED)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Populate(glfw3)
        add_subdirectory(${glfw3_SOURCE_DIR} ${glfw3_BINARY_DIR})
    endif()
endif()
set_target_properties(glfw PROPERTIES FOLDER "third_party")

# OpenGL
find_package(OpenGL REQUIRED)

# Early check for GLAD requirements (Python3 + jinja2)
# GLAD generation uses jinja2 templates and fails with cryptic errors if not installed
find_package(Python3 REQUIRED COMPONENTS Interpreter)
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
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    URL https://github.com/Dav1dde/glad/archive/refs/tags/v2.0.8.zip
)
FetchContent_GetProperties(glad)
if(NOT glad_POPULATED)
    FetchContent_MakeAvailable(glad)
    add_subdirectory("${glad_SOURCE_DIR}/cmake" glad_cmake)
    glad_add_library(glad_gl_core_33 REPRODUCIBLE EXCLUDE_FROM_ALL LOADER API gl:core=3.3)
endif()
set_target_properties(glad_gl_core_33 PROPERTIES FOLDER "third_party")

# FreeType for improved font rendering (always use FetchContent to avoid noisy Findfreetype warnings)

# Avoid FetchContent's auto-find_package for freetype (prevents Findfreetype casing warning)
set(FETCHCONTENT_TRY_FIND_PACKAGE_freetype FALSE)

# CMake 4.2 dropped compatibility for projects declaring cmake_minimum_required(<3.5).
# FreeType 2.13.2 still requests 3.0. Temporarily raise the policy minimum while
# configuring the FreeType subproject, then restore the previous value.
set(_TS_OLD_POLICY_VERSION_MINIMUM ${CMAKE_POLICY_VERSION_MINIMUM})
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(
    freetype
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    URL https://downloads.sourceforge.net/freetype/freetype-2.13.2.tar.gz
    URL_HASH SHA256=1ac27e16c134a7f2ccea177faba19801131116fd682efc1f5737037c5db224b5
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

# Prevent pkg-config warnings emitted by FreeType's optional detection; restore after
set(_TS_OLD_PKGCONFIG_EXEC ${PKG_CONFIG_EXECUTABLE})
set(PKG_CONFIG_EXECUTABLE ${CMAKE_COMMAND} CACHE FILEPATH "" FORCE)

# Temporarily disable deprecation noise from FreeType's legacy CMake minimum; restore afterwards
set(_TS_OLD_WARN_DEPRECATED ${CMAKE_WARN_DEPRECATED})
set(CMAKE_WARN_DEPRECATED FALSE CACHE BOOL "" FORCE)

# Ensure IPO policy defaults to NEW for third-party subdirs that touch IPO
if(POLICY CMP0069)
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
endif()

FetchContent_GetProperties(freetype)
if(NOT freetype_POPULATED)
    FetchContent_Populate(freetype)
    add_subdirectory(${freetype_SOURCE_DIR} ${freetype_BINARY_DIR})
endif()

# Restore the user's policy minimum to avoid global side effects
if(DEFINED _TS_OLD_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM ${_TS_OLD_POLICY_VERSION_MINIMUM})
else()
    unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()

set_target_properties(freetype PROPERTIES FOLDER "third_party")
# Create an alias target to match find_package(Freetype) behavior
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# Restore user-facing settings
if(DEFINED _TS_OLD_WARN_DEPRECATED)
    set(CMAKE_WARN_DEPRECATED ${_TS_OLD_WARN_DEPRECATED} CACHE BOOL "" FORCE)
endif()
if(DEFINED _TS_OLD_PKGCONFIG_EXEC)
    set(PKG_CONFIG_EXECUTABLE ${_TS_OLD_PKGCONFIG_EXEC} CACHE FILEPATH "" FORCE)
endif()

# ImGui
FetchContent_Declare(
    imgui
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    URL https://github.com/ocornut/imgui/archive/refs/heads/docking.zip
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
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
target_link_libraries(imgui_lib PUBLIC glfw glad_gl_core_33 Freetype::Freetype)
target_compile_definitions(imgui_lib PUBLIC
    IMGUI_IMPL_OPENGL_LOADER_GLAD2
    IMGUI_ENABLE_FREETYPE
)
set_target_properties(imgui_lib PROPERTIES FOLDER "third_party")

# ImPlot (master branch for compatibility with latest ImGui)
FetchContent_Declare(
    implot
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    URL https://github.com/epezent/implot/archive/refs/heads/master.zip
)
FetchContent_GetProperties(implot)
if(NOT implot_POPULATED)
    FetchContent_Populate(implot)
endif()

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
    src/Core/Window.cpp
    src/UI/UILayer.cpp
    src/UI/Theme.cpp
    src/UI/ThemeLoader.cpp
    src/UI/IconLoader.cpp
    src/App/AboutLayer.cpp
    src/App/ShellLayer.cpp
    src/App/UserConfig.cpp
    src/App/Panels/ProcessesPanel.cpp
    src/App/Panels/ProcessDetailsPanel.cpp
    src/App/Panels/SystemMetricsPanel.cpp
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
    src/Core/Window.h
    src/Core/Layer.h
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
        src/Platform/Windows/WinString.h
    )
endif()

if(CLANG_TIDY_EXE)
    # Generate a PCH-free and module-free compile_commands.json for clang-tidy
    # This avoids version mismatch issues between compiler and clang-tidy
    # Use platform-appropriate tools: sed on Unix, PowerShell on Windows
    if(WIN32)
        add_custom_target(generate-clang-tidy-compile-commands
            COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_BINARY_DIR}/compile_commands.json
                ${CMAKE_BINARY_DIR}/compile_commands.json.bak
            COMMAND powershell -NoProfile -Command
                "(Get-Content '${CMAKE_BINARY_DIR}/compile_commands.json' -Raw) -replace '-Xclang -include-pch -Xclang [^ ]*\\.pch', '' -replace '-Xclang -emit-pch', '' -replace '-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*', '' -replace '-Winvalid-pch', '' -replace '-fpch-instantiate-templates', '' -replace '@[^ ]*\\.modmap', '' -replace '-fmodule-output=[^ ]*', '' | Set-Content '${CMAKE_BINARY_DIR}/compile_commands.json' -NoNewline"
            DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
            COMMENT "Stripping PCH flags from compile_commands.json for clang-tidy"
        )
    else()
        add_custom_target(generate-clang-tidy-compile-commands
            COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_BINARY_DIR}/compile_commands.json
                ${CMAKE_BINARY_DIR}/compile_commands.json.bak
            COMMAND sed -i
                -e "s/-Xclang -include-pch -Xclang [^ ]*\\.pch//g"
                -e "s/-Xclang -emit-pch//g"
                -e "s/-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*//g"
                -e "s/-Winvalid-pch//g"
                -e "s/-fpch-instantiate-templates//g"
                -e "s/@[^ ]*\\.modmap//g"
                -e "s/-fmodule-output=[^ ]*//g"
                ${CMAKE_BINARY_DIR}/compile_commands.json
            DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
            COMMENT "Stripping PCH flags from compile_commands.json for clang-tidy"
        )
    endif()

    add_custom_target(run-clang-tidy
        COMMAND ${CLANG_TIDY_EXE}
            --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
            -p ${CMAKE_BINARY_DIR}
            --extra-arg=-std=c++23
            --extra-arg=-Wno-unknown-warning-option
            ${TASKSMACK_SOURCES}
            ${TASKSMACK_HEADERS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy on project sources"
    )
    # Dependencies:
    # - generate-clang-tidy-compile-commands: strips PCH flags from compile_commands.json
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

add_executable(TaskSmack WIN32
    ${TASKSMACK_SOURCES}
    ${TASKSMACK_HEADERS}
)

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
    target_link_options(TaskSmack PRIVATE -stdlib=libc++ -pthread -fuse-ld=lld)
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
    target_compile_definitions(TaskSmack PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
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
    target_link_libraries(TaskSmack PRIVATE iphlpapi ws2_32 dxgi)
endif()

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

# Link X11 on Linux (required by ImGui GLFW backend)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(TaskSmack PRIVATE X11::X11)
endif()

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

# Install runtime assets (fonts/themes/icons) only
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/fonts
        ${CMAKE_SOURCE_DIR}/assets/themes
        ${CMAKE_SOURCE_DIR}/assets/icons
    DESTINATION assets
)

if(TASKSMACK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# CPack configuration for packaging
set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_NAME} - TaskSmack")
set(CPACK_PACKAGE_DESCRIPTION "TaskSmack - Modern cross-platform system monitor")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/mgradwohl/tasksmack")
set(CPACK_PACKAGE_VENDOR "")
set(CPACK_PACKAGE_CONTACT "")
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
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libglfw3, libx11-6, libfreetype6")
    endif()
    # Check for rpmbuild (Red Hat/Fedora)
    find_program(RPMBUILD_PROGRAM rpmbuild)
    if(RPMBUILD_PROGRAM)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()
endif()

include(CPack)
