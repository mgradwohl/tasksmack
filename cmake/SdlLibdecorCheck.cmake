# SDL's libdecor build settings, checked at configure time (#4254, #4440, #4661).
#
# ts_check_sdl_libdecor(<SDL binary dir> <require>) reads SDL's CMakeFiles/SDL_build_config.h.intermediate: SDL's
# configure_file() writes it while SDL is configured, with every #cmakedefine resolved. The
# include-config-<config>/build_config/SDL_build_config.h the compiler reads is made from it by file(GENERATE), at
# generate time, after the configure, so on a fresh build tree it isn't there yet, and a check that globbed for it
# passed with nothing to read (#4661). A missing intermediate fails, so the check can never pass by reading nothing.
#
# It fails the configure when:
#   - SDL links libdecor instead of loading it at run time: SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR would then be ignored, and
#     a setcap session would load GTK modules through it (#4254);
#   - <require> is set and SDL was built without libdecor: a native Wayland window would have no frame on GNOME (#4440).

function(ts_check_sdl_libdecor sdl_binary_dir require)
    set(_config "${sdl_binary_dir}/CMakeFiles/SDL_build_config.h.intermediate")
    if(NOT EXISTS "${_config}")
        message(FATAL_ERROR "SDL's configured build settings aren't at ${_config}, so whether it loads libdecor can't "
                            "be checked (#4661). An SDL update may have moved the file: update cmake/SdlLibdecorCheck.cmake.")
    endif()
    # Only a #define counts: an option SDL left off is still in the file, as "/* #undef NAME */" (#4440).
    file(STRINGS "${_config}" _lines REGEX "^#define (HAVE_LIBDECOR_H|SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR)")
    set(_loaded FALSE)
    if(_lines MATCHES "#define SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR")
        set(_loaded TRUE)
    endif()
    if(_lines MATCHES "#define HAVE_LIBDECOR_H 1" AND NOT _loaded)
        message(FATAL_ERROR "SDL links libdecor instead of loading it (${_config}): SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR "
                            "would be ignored (#4254). Keep SDL_WAYLAND_LIBDECOR_SHARED ON.")
    endif()
    if(require AND NOT _loaded)
        message(FATAL_ERROR "SDL was built without libdecor (${_config}), so native Wayland decorations would leave a "
                            "window with no frame on GNOME (#4440). Install libdecor-0-dev, or configure with "
                            "-DTASKSMACK_REQUIRE_LIBDECOR=OFF.")
    endif()
endfunction()
