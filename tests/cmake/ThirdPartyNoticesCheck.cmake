# The THIRD_PARTY_NOTICES.txt generator run on its own, as the -P check on main runs it (#2960), with:
#   cmake -DTS_SOURCE_DIR=<checkout> -DTS_WORK_DIR=<scratch folder> -DTS_CASE=<standalone|explicit> -P ThirdPartyNoticesCheck.cmake
# standalone: nothing includes Dependencies.cmake and TASKSMACK_TOMLPLUSPLUS_PATCH is unset, so the generator
#   reads the patch's name from Dependencies.cmake itself. The backport of that fallback once read an
#   undefined _deps_file here and failed (#2960); configure never reached it, as Dependencies.cmake sets the
#   variable first.
# explicit: the patch variable set, as configure sets it.
# Both: one notice for each FetchContent_Declare() in Dependencies.cmake, and the toml++ patch named.
# A fresh -P process each time, writing to its own scratch folder: no configure cache or caller's variable
# can hide a failure.

include("${TS_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake")
set(CMAKE_SOURCE_DIR "${TS_SOURCE_DIR}")
file(REMOVE_RECURSE "${TS_WORK_DIR}")
file(MAKE_DIRECTORY "${TS_WORK_DIR}")

file(READ "${TS_SOURCE_DIR}/cmake/Dependencies.cmake" _deps)
string(REGEX MATCH "\nset\\(TASKSMACK_TOMLPLUSPLUS_PATCH \"\\$\\{CMAKE_CURRENT_LIST_DIR\\}/([^\"]+)\"\\)" _ "${_deps}")
set(_patch "${CMAKE_MATCH_1}")
if(_patch STREQUAL "")
    message(FATAL_ERROR "Dependencies.cmake doesn't set TASKSMACK_TOMLPLUSPLUS_PATCH")
endif()
cmake_path(GET _patch FILENAME _patch_name) # the notice names the file it's installed as

if(TS_CASE STREQUAL "standalone")
    if(DEFINED TASKSMACK_TOMLPLUSPLUS_PATCH)
        message(FATAL_ERROR "TASKSMACK_TOMLPLUSPLUS_PATCH is already set: this case must run without it")
    endif()
elseif(TS_CASE STREQUAL "explicit")
    set(TASKSMACK_TOMLPLUSPLUS_PATCH "${TS_SOURCE_DIR}/cmake/${_patch}")
else()
    message(FATAL_ERROR "TS_CASE must be standalone or explicit, not '${TS_CASE}'")
endif()

ts_write_third_party_notices("${TS_WORK_DIR}/notices.txt")
file(READ "${TS_WORK_DIR}/notices.txt" _notices)

string(REGEX MATCHALL "(^|\n)[ \t]*FetchContent_Declare[ \t]*\\(" _calls "${_deps}")
string(REGEX MATCHALL "\n  Licence:  " _licences "${_notices}")
list(LENGTH _calls _call_count)
list(LENGTH _licences _notice_count)
if(_call_count EQUAL 0 OR NOT _notice_count EQUAL _call_count)
    message(FATAL_ERROR "${_notice_count} notices for ${_call_count} FetchContent_Declare() calls")
endif()
string(FIND "${_notices}" "patched with ${_patch_name} " _named)
if(_named EQUAL -1)
    message(FATAL_ERROR "the toml++ notice doesn't name its patch, ${_patch_name}")
endif()
message(STATUS "ThirdPartyNoticesCheck (${TS_CASE}): ${_notice_count} notices, toml++ patched with ${_patch_name}")
