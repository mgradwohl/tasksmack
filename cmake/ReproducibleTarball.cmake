# CPack post-build script for the Linux .tar.gz (CPACK_POST_BUILD_SCRIPTS, set for TGZ in
# cmake/CPackProjectConfig.cmake.in): rewrites each archive in CPACK_PACKAGE_FILES so that two builds
# of the same commit produce identical bytes (#2636).
#
# CPack's archive generators add entries in the order a recursive glob of the staging tree returns
# them, which is readdir order: on ext4 it follows the filesystem's directory hash, so two runners
# can list the same tree differently. Each entry also takes its owner from the staging tree (the
# build user's uid and name) and its mode from the umask. CPackDeb sorts and normalizes its own
# entries, which is why the .deb never differed. This extracts each archive and packs it again with
# GNU tar: entries sorted by name, owned by root (0:0), modes 0755 or 0644, every mtime set to
# SOURCE_DATE_EPOCH when it is set, and gzip without a name or timestamp.
#
# Run by CPack before it copies the package to CPACK_PACKAGE_DIRECTORY, or directly for the test:
#   cmake -DCPACK_PACKAGE_FILES=<a.tar.gz;...> -P cmake/ReproducibleTarball.cmake

cmake_minimum_required(VERSION 3.29)

if(NOT CPACK_PACKAGE_FILES)
    message(FATAL_ERROR "ReproducibleTarball.cmake: CPACK_PACKAGE_FILES is empty")
endif()

# --sort=name needs GNU tar 1.28 or later; bsdtar has no equivalent.
find_program(TASKSMACK_GNU_TAR NAMES gtar tar REQUIRED)
find_program(TASKSMACK_GZIP NAMES gzip REQUIRED)
execute_process(COMMAND "${TASKSMACK_GNU_TAR}" --version OUTPUT_VARIABLE _tar_version ERROR_QUIET)
if(NOT _tar_version MATCHES "GNU tar")
    message(FATAL_ERROR "ReproducibleTarball.cmake: ${TASKSMACK_GNU_TAR} isn't GNU tar, which is needed "
                        "to write a reproducible .tar.gz (#2636)")
endif()

set(_mtime_args)
if(DEFINED ENV{SOURCE_DATE_EPOCH})
    set(_mtime_args "--mtime=@$ENV{SOURCE_DATE_EPOCH}")
endif()

foreach(_package IN LISTS CPACK_PACKAGE_FILES)
    if(NOT _package MATCHES "\\.tar\\.gz$")
        continue()
    endif()
    message(STATUS "Repacking ${_package} reproducibly")

    get_filename_component(_dir "${_package}" DIRECTORY)
    set(_tree "${_dir}/_tasksmack_repack")
    file(REMOVE_RECURSE "${_tree}")
    file(MAKE_DIRECTORY "${_tree}")
    file(ARCHIVE_EXTRACT INPUT "${_package}" DESTINATION "${_tree}")

    # The top-level names, sorted; --sort=name orders everything below them.
    file(GLOB _members RELATIVE "${_tree}" "${_tree}/*")
    list(SORT _members)
    if(NOT _members)
        message(FATAL_ERROR "ReproducibleTarball.cmake: ${_package} is empty")
    endif()

    set(_repacked "${_package}.repack")
    execute_process(
        COMMAND "${TASKSMACK_GNU_TAR}" --create --file=- --directory=${_tree} --format=gnu --sort=name
                --owner=0 --group=0 --numeric-owner --mode=u=rwX,go=rX ${_mtime_args} -- ${_members}
        COMMAND "${TASKSMACK_GZIP}" -9 --no-name
        OUTPUT_FILE "${_repacked}"
        RESULTS_VARIABLE _results)
    foreach(_result IN LISTS _results)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "ReproducibleTarball.cmake: repacking ${_package} failed (${_results})")
        endif()
    endforeach()

    file(RENAME "${_repacked}" "${_package}")
    file(REMOVE_RECURSE "${_tree}")
endforeach()
