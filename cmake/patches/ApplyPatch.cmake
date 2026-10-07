# Applies a patch to a FetchContent dependency's source tree, idempotently.
#
# Run as a FetchContent PATCH_COMMAND (the working directory is the dependency's source dir):
#   PATCH_COMMAND ${CMAKE_COMMAND} -DGIT_EXECUTABLE=${GIT_EXECUTABLE} -DPATCH_FILE=<file.patch>
#                 -P ${CMAKE_SOURCE_DIR}/cmake/patches/ApplyPatch.cmake
#
# The source tree under FETCHCONTENT_BASE_DIR is shared by every preset, and FetchContent re-runs
# the patch step on a fresh configure, so the patch may already be in place: if it reverses
# cleanly it is treated as applied and left alone. A patch that neither applies nor reverses
# (the pin moved and the patch no longer fits) stops the configure instead of building an
# unpatched dependency.

cmake_minimum_required(VERSION 3.29)

if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "ApplyPatch.cmake: GIT_EXECUTABLE is not set")
endif()
if(NOT PATCH_FILE OR NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "ApplyPatch.cmake: PATCH_FILE '${PATCH_FILE}' does not exist")
endif()

get_filename_component(_patch_name "${PATCH_FILE}" NAME)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check --reverse --whitespace=nowarn "${PATCH_FILE}"
    RESULT_VARIABLE _already_applied
    OUTPUT_QUIET ERROR_QUIET)
if(_already_applied EQUAL 0)
    message(STATUS "Patch ${_patch_name}: already applied")
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH_FILE}"
    RESULT_VARIABLE _apply_result
    ERROR_VARIABLE _apply_error)
if(NOT _apply_result EQUAL 0)
    message(FATAL_ERROR "Patch ${_patch_name}: does not apply to ${CMAKE_CURRENT_SOURCE_DIR} "
                        "(was the dependency's pin changed?):\n${_apply_error}")
endif()
message(STATUS "Patch ${_patch_name}: applied")
