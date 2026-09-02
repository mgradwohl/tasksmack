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

# Pre-detect RC.exe on Windows and set CMAKE_RC_COMPILER before project() declaration.
# If RC.exe is not found, use a no-op fallback to prevent CMake configuration failures.
# NOTE: We set CMAKE_RC_COMPILER BEFORE calling project() so CMake's platform detection
# doesn't fail when looking for the RC compiler.
if(WIN32)
    # Search in common Windows SDK locations, then fall back to PATH
    find_program(RC_COMPILER_FOUND rc.exe
        HINTS
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
            "C:/Program Files (x86)/Windows Kits/11/bin/11.0.1.0/x64"
            "C:/Program Files/Windows Kits/10/bin/10.0.26100.0/x64"
            "C:/Program Files/Windows Kits/11/bin/11.0.1.0/x64"
    )
    
    if(RC_COMPILER_FOUND)
        set(CMAKE_RC_COMPILER ${RC_COMPILER_FOUND})
        message(STATUS "Found RC compiler: ${CMAKE_RC_COMPILER}")
        set(TASKSMACK_HAS_RC_COMPILER TRUE)
    else()
        # Use a no-op fallback to prevent CMake from failing during platform detection.
        # This allows configuration to proceed without rc.exe in PATH.
        message(STATUS "RC compiler (rc.exe) not found; using no-op fallback")
        set(CMAKE_RC_COMPILER "echo")
        set(TASKSMACK_HAS_RC_COMPILER FALSE)
    endif()
endif()

# Initialize with both C and CXX (required by dependencies like SDL3, GLAD)
project(TaskSmack VERSION 0.8.0 LANGUAGES C CXX)

# Focused modules keep this root file declarative. Order matters:
#   Options          - cache options consumed by every later module
#   CompilerOptions  - toolchain, warnings, hardening, ccache, IPO, linker selection
#                      (global flags must be set before dependencies are configured)
#   Dependencies     - FetchContent orchestration and third-party target wiring
include(cmake/Options.cmake)
include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

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
    src/Core/VideoBackend.cpp
    src/Core/Window.cpp
    src/UI/AssetPath.cpp
    src/UI/UILayer.cpp
    src/UI/RenderMetrics.cpp
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
    src/UI/RenderMetrics.h
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
    src/Domain/ISamplable.h
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

# Static analysis targets consume TASKSMACK_SOURCES/TASKSMACK_HEADERS as defined
# above — include before the Windows .rc file is appended so clang-tidy and
# clang-format only see real C++ sources.
include(cmake/StaticAnalysis.cmake)

# Windows resource file (icon, version info, manifest)
# Only compile resources if RC.exe is available
if(WIN32 AND TASKSMACK_HAS_RC_COMPILER)
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
elseif(WIN32)
    message(STATUS "Skipping Windows resource compilation (RC.exe not found)")
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

# Keep libc++, pthread, and the selected linker consistent across all
# TaskSmack-owned executables and configurations.
tasksmack_apply_linux_toolchain(TaskSmack)

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
)

# SYSTEM (not PRIVATE above): stb is third-party, and the FetchContent_Declare(SYSTEM)
# keyword only marks CMake's own view of the dependency, not this consuming include path -
# without this, its headers were reported as project code by warnings-as-errors, papered
# over by manual #pragma diagnostic guards at the one #include site (IconLoader.cpp) that
# would silently stop covering any future stb-touching file or warning class.
target_include_directories(TaskSmack SYSTEM PRIVATE ${stb_SOURCE_DIR})

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

include(cmake/InstallRules.cmake)

if(TASKSMACK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TASKSMACK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(cmake/Packaging.cmake)
