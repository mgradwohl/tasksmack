# cmake/SdlLibdecorCheck.cmake against SDL settings files it writes (#4661), run with -P:
#   cmake -DTS_SOURCE_DIR=<checkout> -DTS_WORK_DIR=<scratch folder> -DTS_CASE=<case> -P SdlLibdecorCheck.cmake
# loaded:   libdecor loaded at run time, required: passes.
# none:     no libdecor, not required: passes; the "/* #undef */" lines don't count.
# missing:  no settings file at all (a fresh tree before the fix read nothing): fails, saying so.
# linked:   libdecor linked, not loaded: fails (#4254).
# required: no libdecor, required: fails (#4440).

include("${TS_SOURCE_DIR}/cmake/SdlLibdecorCheck.cmake")
file(REMOVE_RECURSE "${TS_WORK_DIR}")
file(MAKE_DIRECTORY "${TS_WORK_DIR}/CMakeFiles")
set(_file "${TS_WORK_DIR}/CMakeFiles/SDL_build_config.h.intermediate")

if(TS_CASE STREQUAL "loaded")
    file(WRITE "${_file}" "#define HAVE_LIBDECOR_H 1\n#define SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR \"libdecor-0.so.0\"\n")
    ts_check_sdl_libdecor("${TS_WORK_DIR}" ON)
elseif(TS_CASE STREQUAL "none")
    file(WRITE "${_file}" "/* #undef HAVE_LIBDECOR_H */\n/* #undef SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR */\n")
    ts_check_sdl_libdecor("${TS_WORK_DIR}" OFF)
elseif(TS_CASE STREQUAL "missing")
    ts_check_sdl_libdecor("${TS_WORK_DIR}" OFF)
elseif(TS_CASE STREQUAL "linked")
    file(WRITE "${_file}" "#define HAVE_LIBDECOR_H 1\n/* #undef SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR */\n")
    ts_check_sdl_libdecor("${TS_WORK_DIR}" OFF)
elseif(TS_CASE STREQUAL "required")
    file(WRITE "${_file}" "/* #undef HAVE_LIBDECOR_H */\n")
    ts_check_sdl_libdecor("${TS_WORK_DIR}" ON)
else()
    message(FATAL_ERROR "unknown TS_CASE '${TS_CASE}'")
endif()
message(STATUS "SdlLibdecorCheck: ${TS_CASE} passed the check")
