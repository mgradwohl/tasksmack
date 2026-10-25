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
        message(STATUS "FetchContent cache enabled: ${FETCHCONTENT_BASE_DIR}")
    endif()
endif()

include(FetchContent)

# Every install() rule a dependency added below declares lands in a "third_party" component, which
# cmake/Packaging.cmake leaves out of the packages: TaskSmack links all of them statically, so their
# headers, libraries and CMake/pkg-config files never belong in one (#1398). Generic, so a new
# dependency can't leak; reset to the default at the end of this file for TaskSmack's own rules.
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME third_party)

# TASKSMACK_ENABLE_PCH=OFF means no precompiled headers anywhere, the dependencies' own included.
# CI builds with it off so ccache can cache every compile (#1406); SDL3 adds a PCH to its own target
# regardless, which left its ~260 C files uncacheable, about 30% of every CI build's compiler
# calls (#1875).
if(NOT TASKSMACK_ENABLE_PCH)
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
endif()

# Keep source-based dependencies deterministic: we rely on <dep>_SOURCE_DIR values below.
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE OPT_IN)

# Every FetchContent_Declare() below passes BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/<dep>-build"
# and SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/<dep>-subbuild".
#
# FETCHCONTENT_BASE_DIR above points at a cache shared by every preset, and by default
# FetchContent puts <dep>-src, <dep>-subbuild AND <dep>-build under it. Sharing -src (the
# download) is the point of the cache, but sharing -build meant every preset compiled SDL3,
# FreeType, googletest, ... into the same object directory,
# so Ninja in one preset could treat objects compiled with another preset's flags (sanitizers,
# coverage, LTO, -O level) as up to date (#1308). Pinning BINARY_DIR to this build tree keeps
# the download shared while every preset builds its own objects. The calls stay literal
# FetchContent_Declare() (no wrapper) because osv-scanner.yml's Syft cmake cataloger and
# Renovate's regex managers read them from the source text.
set(TASKSMACK_DEPS_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps")

# The -subbuild is the small CMake project FetchContent configures and builds to clone a
# dependency into its -src. Its CMakeCache.txt records the absolute path and generator it was
# configured with, and its download stamps record this host's cmake and paths, so one sub-build
# shared by WSL (/mnt/c/...) and Windows (C:/...) failed the second host's configure (#2253).
# Each host gets its own sub-builds under the cache, keyed by host OS, the cache path as this
# host spells it, and the generator. They stay in the cache (not in this build tree) so every
# preset on a host, and a CI cache restore, still finds its stamps and doesn't re-clone. A host
# whose stamps aren't there yet clones once (ExternalProject replaces -src); configure it with
# -DFETCHCONTENT_FULLY_DISCONNECTED=ON to use the -src another host already fetched instead.
# With the cache off, FetchContent's default (<build>/_deps/<dep>-subbuild) is already per-preset.
if(TASKSMACK_ENABLE_FETCHCONTENT_CACHE)
    string(SHA256 _tasksmack_subbuild_key "${CMAKE_HOST_SYSTEM_NAME}|${FETCHCONTENT_BASE_DIR}|${CMAKE_GENERATOR}")
    string(SUBSTRING "${_tasksmack_subbuild_key}" 0 8 _tasksmack_subbuild_key)
    set(TASKSMACK_DEPS_SUBBUILD_DIR "${FETCHCONTENT_BASE_DIR}/subbuild/${CMAKE_HOST_SYSTEM_NAME}-${_tasksmack_subbuild_key}")
else()
    set(TASKSMACK_DEPS_SUBBUILD_DIR "${TASKSMACK_DEPS_BINARY_DIR}")
endif()

# Configure-time guard for #1308: every directory added to the build (including every
# FetchContent dependency, however it was declared) must have its binary dir inside this
# preset's build tree, never in the shared FetchContent cache.
function(_tasksmack_check_dependency_binary_dirs dir)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        get_property(_bin DIRECTORY "${_subdir}" PROPERTY BINARY_DIR)
        cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${_bin}" NORMALIZE _inside_build)
        if(NOT _inside_build)
            message(FATAL_ERROR
                "Directory '${_subdir}' builds into '${_bin}', outside this build tree "
                "'${CMAKE_BINARY_DIR}'. Dependency build trees must be per-preset (#1308): "
                "pass BINARY_DIR \"\${TASKSMACK_DEPS_BINARY_DIR}/<dep>-build\" to its FetchContent_Declare().")
        endif()
        _tasksmack_check_dependency_binary_dirs("${_subdir}")
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _tasksmack_check_dependency_binary_dirs "${CMAKE_SOURCE_DIR}")

# Configure-time guard for #2253: a FetchContent_Declare() without SUBBUILD_DIR configures its
# sub-build at the shared default, <cache>/<dep>-subbuild. Any such CMakeCache.txt written after
# this marker was written by this configure.
if(TASKSMACK_ENABLE_FETCHCONTENT_CACHE)
    set(_tasksmack_subbuild_marker "${CMAKE_BINARY_DIR}/CMakeFiles/tasksmack-deps-configure.marker")
    file(TOUCH "${_tasksmack_subbuild_marker}")
    function(_tasksmack_check_dependency_subbuild_dirs marker)
        file(GLOB _caches "${FETCHCONTENT_BASE_DIR}/*-subbuild/CMakeCache.txt")
        foreach(_cache IN LISTS _caches)
            if(NOT "${marker}" IS_NEWER_THAN "${_cache}")
                cmake_path(GET _cache PARENT_PATH _subbuild)
                message(FATAL_ERROR
                    "FetchContent configured the sub-build '${_subbuild}' in the cache shared by every "
                    "host. Sub-builds must be per-host (#2253): pass SUBBUILD_DIR "
                    "\"\${TASKSMACK_DEPS_SUBBUILD_DIR}/<dep>-subbuild\" to its FetchContent_Declare().")
            endif()
        endforeach()
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _tasksmack_check_dependency_subbuild_dirs "${_tasksmack_subbuild_marker}")
endif()

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
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/spdlog-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/spdlog-subbuild"  # per-host sub-build (#2253)
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
#
# Pinned to an untagged master commit (2026-07-21, 50 commits past v3.4.0) rather than v3.4.0:
# v3.4.0 hits a TOML_ASSERT_ASSUME in parse_key() on '[' then a newline (#1387, fixed upstream in
# e4bcc15) and TOML_UNREACHABLE in is_non_ascii_horizontal_whitespace() on U+FB26..U+FEFE (#1389,
# fixed upstream in 1e8829b). Both are UB in release builds. Move back to a release tag once one
# includes them (v3.5.0+).
#
# The patch fixes TOML_ASSERT_ASSUMEs that malformed input reaches, none fixed upstream yet: two in
# parse_value(), 'm=[}' (#1388) and a date, a space, a digit, then a terminator, as in
# 'h=2000-02-1a 1 1' (#1659); and the digit asserts that open parse_time(), parse_date() and
# parse_date_time(), which a non-digit after a date-time's 'T'/'t' reaches, as in
# 'h=2000-02-10Tx:' (#2427). A source tree that still has an older version of the patch applied
# (a restored CI cache, a long-lived local one) is reset to pristine and re-patched by
# ApplyPatch.cmake. ApplyPatch.cmake skips it when already applied, since the source tree under
# FETCHCONTENT_BASE_DIR is shared by every preset. Re-check the patch whenever GIT_TAG moves, and
# give an edited patch a new file name: FetchContent only re-runs the patch step when this
# declaration changes, and CI's FetchContent cache is keyed on this file, not on the patch.
# The fuzz build (.clusterfuzzlite/fetch-deps.sh, #1390) reads the GIT_TAG on the line after
# GIT_REPOSITORY, so keep them adjacent. Renovate only tracks this pin while its comment reads
# "# vX.Y.Z" (see .github/renovate.json5), so it leaves this untagged pin alone.
#
# The patch's path, defined once (#2779): PATCH_COMMAND below applies it, cmake/InstallRules.cmake
# ships it beside the licences, and cmake/ThirdPartyNotices.cmake names it in THIRD_PARTY_NOTICES.txt.
# install(FILES) only looks for its sources at install time, so a renamed patch that isn't here would
# otherwise first fail the release's cpack.
set(TASKSMACK_TOMLPLUSPLUS_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/tomlplusplus-parse-assertions.patch")
if(NOT EXISTS "${TASKSMACK_TOMLPLUSPLUS_PATCH}")
    message(FATAL_ERROR "toml++ patch file not found: ${TASKSMACK_TOMLPLUSPLUS_PATCH}")
endif()
find_package(Git REQUIRED)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG 1e8829b793b66ad17011732a146b8077d379b011  # master after v3.4.0 - pinned to SHA for supply chain security
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
                  "-DPATCH_FILE=${TASKSMACK_TOMLPLUSPLUS_PATCH}"
                  -P "${CMAKE_CURRENT_LIST_DIR}/patches/ApplyPatch.cmake"
    SYSTEM  # Treat as system headers to suppress warnings
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/tomlplusplus-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/tomlplusplus-subbuild"  # per-host sub-build (#2253)
)
FetchContent_MakeAvailable(tomlplusplus)

# OpenGL + ImGui stack dependencies

# stb (image loader for runtime textures)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20  # Pinned to specific commit for supply-chain security
    SYSTEM  # Treat as system headers to suppress third-party warning noise, matching every other dependency here
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/stb-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/stb-subbuild"  # per-host sub-build (#2253)
)
FetchContent_MakeAvailable(stb)

# SDL3 for windowing, input, and custom title bar support
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG 829a65d769d935c4852f8159e964312c0957260a  # release-3.4.18 - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings from SDL3
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/sdl3-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/sdl3-subbuild"  # per-host sub-build (#2253)
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

set(_tasksmack_restore_cmp0219_default FALSE)
if(DEFINED CMAKE_POLICY_DEFAULT_CMP0219)
    set(_tasksmack_saved_cmp0219_default "${CMAKE_POLICY_DEFAULT_CMP0219}")
    set(_tasksmack_restore_cmp0219_default TRUE)
endif()
if(POLICY CMP0219)
    set(CMAKE_POLICY_DEFAULT_CMP0219 NEW)
endif()
FetchContent_MakeAvailable(SDL3)
if(_tasksmack_restore_cmp0219_default)
    set(CMAKE_POLICY_DEFAULT_CMP0219 "${_tasksmack_saved_cmp0219_default}")
else()
    unset(CMAKE_POLICY_DEFAULT_CMP0219)
endif()
unset(_tasksmack_restore_cmp0219_default)
unset(_tasksmack_saved_cmp0219_default)
set_target_properties(SDL3-static PROPERTIES FOLDER "third_party")

# Prefer the project-local environment created by tools/setup-dev.sh / tools/setup-dev.ps1
# without overriding the machine's system Python. Windows venvs keep the interpreter in
# Scripts\python.exe, Linux ones in bin/python; a checkout shared with WSL can hold the other
# OS's layout, so look only for the host's.
if(CMAKE_HOST_WIN32)
    set(_tasksmack_venv_python "${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe")
else()
    set(_tasksmack_venv_python "${CMAKE_SOURCE_DIR}/.venv/bin/python")
endif()
if(NOT Python3_EXECUTABLE AND EXISTS "${_tasksmack_venv_python}")
    set(Python3_EXECUTABLE "${_tasksmack_venv_python}" CACHE FILEPATH "Python interpreter")
endif()
unset(_tasksmack_venv_python)

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
# GLAD's glad_add_library() runs its own find_package(Python) and generates with Python_EXECUTABLE,
# not Python3_EXECUTABLE. Left alone, FindPython takes the newest interpreter on the machine -- a
# runner image's Python 3.15 without jinja2, while CI passed -DPython3_EXECUTABLE for 3.14 (#2672) --
# so hand GLAD the interpreter the check above verified, whether it came from the command line, the
# venv or find_package(), unless the caller chose GLAD's explicitly.
if(NOT Python_EXECUTABLE)
    set(Python_EXECUTABLE "${Python3_EXECUTABLE}" CACHE FILEPATH "Python interpreter (GLAD)")
endif()

# GLAD
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG 73db193f853e2ee079bf3ca8a64aa2eaf6459043  # v2.0.8 - pinned to SHA for supply chain security
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/glad-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/glad-subbuild"  # per-host sub-build (#2253)
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
    GIT_TAG 37515cf56ba3349656049edbed89ffc6f6466f2d  # VER-2-15-0 - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings from FreeType
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/freetype-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/freetype-subbuild"  # per-host sub-build (#2253)
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

# FreeType is linked statically into TaskSmack; without this its own install() rules put
# include/freetype2, lib/libfreetype.a, lib/cmake and lib/pkgconfig into every package (#1398).
# (FetchContent's EXCLUDE_FROM_ALL wouldn't help: it leaves a subdirectory's install rules in place.)
set(SKIP_INSTALL_ALL ON)
FetchContent_MakeAvailable(freetype)
unset(SKIP_INSTALL_ALL)

set_target_properties(freetype PROPERTIES FOLDER "third_party")
# Create an alias target to match find_package(Freetype) behavior
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG b916186e66aa3c02b72c6ca17455516512ed8049  # v1.93.0-docking - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings from ImGui
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/imgui-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/imgui-subbuild"  # per-host sub-build (#2253)
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

# ImPlot master after v1.0, "1.1 WIP" (requires ImGui docking branch). v1.0 reads ImGui 1.93's
# TexUvLines in the 1.92 layout, so its anti-aliased lines sampled the wrong atlas texels (#2744);
# 7eeb9168 ("fix: use ImDrawList::_SelectLineTexture()") fixes that, and the seven commits between
# v1.0 and it are ImGui 1.93 API updates (AddRect/AddPolyline argument order, AddLineH/AddLineV)
# plus two marker types and two legend/scatter colour fixes. tests/UI/test_PlotLineTexture.cpp
# holds the line UVs to ImGui's own. Renovate only tracks this pin while its comment reads "# vX.Y"
# (see .github/renovate.json5), so it leaves this untagged pin alone; move back to a tag (v1.1)
# once one includes the fix.
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG 7eeb9168d2e5e6b14e266d8782ecf7e649dfc3a4  # master after v1.0 - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings from ImPlot
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/implot-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/implot-subbuild"  # per-host sub-build (#2253)
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

# Back to the default component ("Unspecified") for TaskSmack's own install rules.
unset(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME)
