cmake_minimum_required(VERSION 3.28)

# Allow FetchContent_Populate for dependencies without CMakeLists.txt (imgui, implot)
cmake_policy(SET CMP0169 OLD)

project(TaskSmack VERSION 0.1.0 LANGUAGES C CXX)

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
option(TASKSMACK_ENABLE_FETCHCONTENT_CACHE "Use a shared FetchContent/CPM cache directory instead of per-build _deps" OFF)
set(TASKSMACK_FETCHCONTENT_CACHE_DIR "" CACHE PATH "Override FetchContent/CPM cache directory when caching is enabled")

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
    elseif(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /permissive-
            /EHsc
            $<$<BOOL:${TASKSMACK_WARNINGS_AS_ERRORS}>:/WX>
        )
    endif()
endfunction()

find_program(CLANG_TIDY_EXE NAMES clang-tidy
    HINTS
        "/usr/lib/llvm-22/bin"
        "$ENV{LLVM_ROOT}/bin"
        "$ENV{ProgramFiles}/LLVM/bin"
        "C:/Program Files/LLVM/bin"
)

find_program(CLANG_FORMAT_EXE NAMES clang-format
    HINTS
        "/usr/lib/llvm-22/bin"
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
    target_compile_definitions(spdlog_header_only INTERFACE SPDLOG_USE_STD_FORMAT $<$<PLATFORM_ID:Windows>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>)
endif()

if(TARGET spdlog)
    target_compile_definitions(spdlog PUBLIC SPDLOG_USE_STD_FORMAT $<$<PLATFORM_ID:Windows>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>)
endif()

# OpenGL + ImGui stack dependencies

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
)
target_include_directories(imgui_lib
    SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${imgui_SOURCE_DIR}/misc/cpp
)
target_link_libraries(imgui_lib PUBLIC glfw glad_gl_core_33)
target_compile_definitions(imgui_lib PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_IMPL_OPENGL_LOADER_GLAD2
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
    src/App/ShellLayer.cpp
    src/App/Panels/ProcessesPanel.cpp
    src/App/Panels/ProcessDetailsPanel.cpp
    src/App/Panels/SystemMetricsPanel.cpp
    src/Domain/ProcessModel.cpp
    src/Domain/BackgroundSampler.cpp
    src/Domain/SystemModel.cpp
)

# Platform-specific sources
if(UNIX AND NOT APPLE)
    list(APPEND TASKSMACK_SOURCES
        src/Platform/Linux/LinuxProcessProbe.cpp
        src/Platform/Linux/LinuxProcessActions.cpp
        src/Platform/Linux/LinuxSystemProbe.cpp
        src/Platform/Linux/Factory.cpp
    )
elseif(WIN32)
    list(APPEND TASKSMACK_SOURCES
        # src/Platform/Windows/WindowsProcessProbe.cpp
        # src/Platform/Windows/Factory.cpp
    )
endif()

set(TASKSMACK_HEADERS
    src/Core/Application.h
    src/Core/Window.h
    src/Core/Layer.h
    src/UI/UILayer.h
    src/App/ShellLayer.h
    src/App/Panels/ProcessesPanel.h
    src/Platform/ProcessTypes.h
    src/Platform/IProcessProbe.h
    src/Platform/Factory.h
    src/Domain/ProcessSnapshot.h
    src/Domain/ProcessModel.h
)

# Platform-specific headers
if(UNIX AND NOT APPLE)
    list(APPEND TASKSMACK_HEADERS
        src/Platform/Linux/LinuxProcessProbe.h
    )
elseif(WIN32)
    list(APPEND TASKSMACK_HEADERS
        # src/Platform/Windows/WindowsProcessProbe.h
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
                "(Get-Content '${CMAKE_BINARY_DIR}/compile_commands.json' -Raw) -replace '-Xclang -include-pch -Xclang [^ ]*\\.pch', '' -replace '-Xclang -emit-pch', '' -replace '-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*', '' -replace '-Winvalid-pch', '' -replace '-fpch-instantiate-templates', '' -replace '@CMakeFiles[^ ]*\\.modmap', '' -replace '-fmodule-output=[^ ]*', '' | Set-Content '${CMAKE_BINARY_DIR}/compile_commands.json' -NoNewline"
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
                -e "s/@CMakeFiles[^ ]*\\.modmap//g"
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
    add_dependencies(run-clang-tidy generate-clang-tidy-compile-commands copy-compile-commands)
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

add_executable(TaskSmack
    ${TASKSMACK_SOURCES}
    ${TASKSMACK_HEADERS}
)

target_compile_features(TaskSmack PRIVATE cxx_std_23)

# Precompiled headers
if(TASKSMACK_ENABLE_PCH)
    target_precompile_headers(TaskSmack PRIVATE
        # GL headers first to prevent conflicts with system headers
        <glad/gl.h>
        # Standard library headers
        <string>
        <vector>
        <memory>
        <algorithm>
        <functional>
        <optional>
        <variant>
        <span>
        <format>
        # Third-party headers
        <spdlog/spdlog.h>
    )
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
)

target_link_libraries(TaskSmack PRIVATE
    spdlog::spdlog_header_only
    imgui_lib
    implot_lib
    OpenGL::GL
)

# Copy assets (fonts) to build directory
set(TASKSMACK_ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets")
add_custom_command(TARGET TaskSmack POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TaskSmack>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${TASKSMACK_ASSETS_DIR}/fonts"
        "$<TARGET_FILE_DIR:TaskSmack>/assets/fonts"
    COMMENT "Copying font assets to build directory"
)

# Link X11 on Linux (required by ImGui GLFW backend)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(TaskSmack PRIVATE X11::X11)
endif()

install(TARGETS TaskSmack
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(TASKSMACK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# CPack configuration for packaging
set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_NAME} - A modern C++23 application")
set(CPACK_PACKAGE_VENDOR "")
set(CPACK_PACKAGE_CONTACT "")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")

# Output directory for packages
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")

# Archive generators (cross-platform)
set(CPACK_GENERATOR "ZIP;TGZ")

# Platform-specific generators
if(WIN32)
    list(APPEND CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH ON)
elseif(APPLE)
    list(APPEND CPACK_GENERATOR "DragNDrop")
elseif(UNIX)
    # Check for dpkg (Debian/Ubuntu)
    find_program(DPKG_PROGRAM dpkg)
    if(DPKG_PROGRAM)
        list(APPEND CPACK_GENERATOR "DEB")
        set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
        set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    endif()
    # Check for rpmbuild (Red Hat/Fedora)
    find_program(RPMBUILD_PROGRAM rpmbuild)
    if(RPMBUILD_PROGRAM)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()
endif()

include(CPack)
