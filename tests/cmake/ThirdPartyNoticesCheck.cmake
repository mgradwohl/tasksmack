# The THIRD_PARTY_NOTICES.txt generator against Dependencies.cmake (#2753), run with -P:
#   cmake -DTS_SOURCE_DIR=<checkout> -DTS_WORK_DIR=<scratch folder> -DTS_CASE=<real|unparsed> -P ThirdPartyNoticesCheck.cmake
# real: the checkout's own file gives one notice for each FetchContent_Declare() call in it.
# unparsed: the same file with a FetchContent_Declare() in another shape (a URL source) fails, saying so.

include("${TS_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake")
set(CMAKE_SOURCE_DIR "${TS_SOURCE_DIR}")
file(MAKE_DIRECTORY "${TS_WORK_DIR}")

if(TS_CASE STREQUAL "real")
    ts_write_third_party_notices("${TS_WORK_DIR}/notices.txt")
    file(READ "${TS_WORK_DIR}/notices.txt" _notices)
    file(READ "${TS_SOURCE_DIR}/cmake/Dependencies.cmake" _deps)
    string(REGEX MATCHALL "(^|\n)[ \t]*FetchContent_Declare[ \t]*\\(" _calls "${_deps}")
    string(REGEX MATCHALL "\n  Licence:  " _licences "${_notices}")
    list(LENGTH _calls _call_count)
    list(LENGTH _licences _notice_count)
    if(_call_count EQUAL 0 OR NOT _notice_count EQUAL _call_count)
        message(FATAL_ERROR "${_notice_count} notices for ${_call_count} FetchContent_Declare() calls")
    endif()
    message(STATUS "ThirdPartyNoticesCheck: every dependency has a notice")
elseif(TS_CASE STREQUAL "unparsed")
    file(READ "${TS_SOURCE_DIR}/cmake/Dependencies.cmake" _deps)
    string(APPEND _deps "\nFetchContent_Declare(\n    newdep\n    URL https://example.invalid/newdep-1.0.tar.gz\n"
                        "    URL_HASH SHA256=0000000000000000000000000000000000000000000000000000000000000000)\n")
    file(WRITE "${TS_WORK_DIR}/Dependencies.cmake" "${_deps}")
    # Expected to stop here with FATAL_ERROR; the test passes on its message.
    ts_write_third_party_notices("${TS_WORK_DIR}/notices.txt" "${TS_WORK_DIR}/Dependencies.cmake")
    message(STATUS "ThirdPartyNoticesCheck: the unparsed declare was let through")
else()
    message(FATAL_ERROR "TS_CASE must be real or unparsed, not '${TS_CASE}'")
endif()
