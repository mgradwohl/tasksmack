# Applies a patch to a FetchContent dependency's source tree, idempotently.
#
# Run as a FetchContent PATCH_COMMAND (the working directory is the dependency's source dir):
#   PATCH_COMMAND ${CMAKE_COMMAND} -DGIT_EXECUTABLE=${GIT_EXECUTABLE} -DPATCH_FILE=<file.patch>
#                 -P ${CMAKE_SOURCE_DIR}/cmake/patches/ApplyPatch.cmake
#
# The source tree under FETCHCONTENT_BASE_DIR is shared by every preset, and FetchContent re-runs
# the patch step on a fresh configure, so the patch may already be in place: if it reverses
# cleanly it is treated as applied and left alone.
#
# A patch that neither applies nor reverses usually means the tree carries an older version of the
# patch: a FetchContent cache restored from before the patch was edited (CI restores the newest
# entry by key prefix), or a developer's long-lived cache. When the source dir is the root of its
# own git checkout (FetchContent clones with git), it is reset to the checked-out commit's pristine
# content (git checkout -- . and git clean -fdq; HEAD is untouched) and the patch is applied again.
# Only that checkout is touched, never an enclosing repository. If the patch still doesn't apply
# (the pin moved and the patch no longer fits), the configure stops instead of building an
# unpatched dependency. The reset discards every other local change to the tree, so a dependency
# gets at most one patch file.

cmake_minimum_required(VERSION 3.29)

if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "ApplyPatch.cmake: GIT_EXECUTABLE is not set")
endif()
if(NOT PATCH_FILE OR NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "ApplyPatch.cmake: PATCH_FILE '${PATCH_FILE}' does not exist")
endif()

get_filename_component(_patch_name "${PATCH_FILE}" NAME)
set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check --reverse --whitespace=nowarn "${PATCH_FILE}"
    WORKING_DIRECTORY "${_src_dir}"
    RESULT_VARIABLE _already_applied
    OUTPUT_QUIET ERROR_QUIET)
if(_already_applied EQUAL 0)
    message(STATUS "Patch ${_patch_name}: already applied")
    return()
endif()

function(_tasksmack_apply_patch out_result out_error)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH_FILE}"
        WORKING_DIRECTORY "${_src_dir}"
        RESULT_VARIABLE _result
        ERROR_VARIABLE _error)
    set(${out_result} "${_result}" PARENT_SCOPE)
    set(${out_error} "${_error}" PARENT_SCOPE)
endfunction()

_tasksmack_apply_patch(_apply_result _apply_error)
if(_apply_result EQUAL 0)
    message(STATUS "Patch ${_patch_name}: applied")
    return()
endif()

# Reset only when the source dir is the top of its own checkout. A source tree without its own .git
# (under the project's .cache/, say) sits inside the project repository, and git would act on that.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel
    WORKING_DIRECTORY "${_src_dir}"
    RESULT_VARIABLE _toplevel_result
    OUTPUT_VARIABLE _toplevel
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
set(_is_own_checkout FALSE)
if(_toplevel_result EQUAL 0)
    file(REAL_PATH "${_toplevel}" _toplevel_real)
    file(REAL_PATH "${_src_dir}" _src_real)
    if(_toplevel_real STREQUAL _src_real)
        set(_is_own_checkout TRUE)
    endif()
endif()
if(NOT _is_own_checkout)
    message(FATAL_ERROR "Patch ${_patch_name}: does not apply to ${_src_dir}, which is not the root of a git "
                        "checkout this script may reset (was the dependency's pin changed?):\n${_apply_error}")
endif()

get_filename_component(_dep_name "${_src_dir}" NAME)
message(STATUS "Patch ${_patch_name}: neither applies nor is already applied; "
               "resetting ${_dep_name} to pristine before applying ${_patch_name}")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" checkout -- .
    WORKING_DIRECTORY "${_src_dir}"
    RESULT_VARIABLE _checkout_result
    ERROR_VARIABLE _checkout_error)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" clean -fdq
    WORKING_DIRECTORY "${_src_dir}"
    RESULT_VARIABLE _clean_result
    ERROR_VARIABLE _clean_error)
if(NOT _checkout_result EQUAL 0 OR NOT _clean_result EQUAL 0)
    message(FATAL_ERROR "Patch ${_patch_name}: could not reset ${_src_dir}:\n${_checkout_error}${_clean_error}")
endif()

_tasksmack_apply_patch(_apply_result _apply_error)
if(NOT _apply_result EQUAL 0)
    message(FATAL_ERROR "Patch ${_patch_name}: does not apply to ${_src_dir} even after resetting it to "
                        "pristine (was the dependency's pin changed?):\n${_apply_error}")
endif()
message(STATUS "Patch ${_patch_name}: applied")
