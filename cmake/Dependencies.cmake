# Third-party dependency orchestration (FetchContent) and third-party target wiring.
# Must be included after CompilerOptions.cmake so global flags (coverage, hardening,
# compiler launcher) apply to third-party builds identically to the previous layout.

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
# Keep source-based dependencies deterministic: we rely on <dep>_SOURCE_DIR values below.
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE OPT_IN)

# CMP0072: FindOpenGL prefers the legacy GL library unless told otherwise, which
# emits a policy warning on Linux. Prefer GLVND explicitly (the modern dispatch
# library) before find_package(OpenGL).
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

# Prefer header-only spdlog and std::format to sidestep MSVC fmt deprecation noise.
set(SPDLOG_HEADER_ONLY ON CACHE BOOL "Build spdlog in header-only mode" FORCE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG 79524ddd08a4ec981b7fea76afd08ee05f83755d  # v1.17.0 - pinned to SHA for supply chain security
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
    GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de  # v3.4.0 - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings
)
FetchContent_MakeAvailable(tomlplusplus)

# OpenGL + ImGui stack dependencies

# stb (image loader for runtime textures)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20  # Pinned to specific commit for supply-chain security
)
FetchContent_MakeAvailable(stb)

# SDL3 for windowing, input, and custom title bar support
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG 147a8ee32dbf9ac02f3794964490687b6bbda1bc  # release-3.4.14 - pinned to SHA for supply chain security
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

if(POLICY CMP0219)
    set(CMAKE_POLICY_DEFAULT_CMP0219 NEW)
endif()
FetchContent_MakeAvailable(SDL3)
unset(CMAKE_POLICY_DEFAULT_CMP0219)
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
    GIT_TAG 73db193f853e2ee079bf3ca8a64aa2eaf6459043  # v2.0.8 - pinned to SHA for supply chain security
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
    GIT_TAG 0a0221a1347e2f1e07c395263540026e9a0aa7c7  # VER-2-14-3 - pinned to SHA for supply chain security
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
    GIT_TAG b48d1afbe8ee8b238e2961dc363a949dd7304e23  # v1.92.9b-docking - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings from ImGui
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
    GIT_TAG 524f9fcd48d76c13fdf94c5ffbba8787a1ff7e39  # v1.0 - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings from ImPlot
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
