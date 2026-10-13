# Script mode, run after a test binary links (cmake/GTestSuites.cmake): lists the binary's
# Google Test suites into SUITES_FILE as `set(_suites ...)`, for the ctest include file.
#   -DTEST_EXECUTABLE=... -DTEST_WORKING_DIR=... -DDISCOVERY_TIMEOUT=... -DSUITES_FILE=...

execute_process(
    COMMAND "${TEST_EXECUTABLE}" --gtest_list_tests
    WORKING_DIRECTORY "${TEST_WORKING_DIR}"
    TIMEOUT ${DISCOVERY_TIMEOUT}
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    file(REMOVE "${SUITES_FILE}")
    message(FATAL_ERROR "Listing the tests of ${TEST_EXECUTABLE} failed (${result}):\n${output}")
endif()

# A suite line is the suite's name and a '.', unindented, perhaps followed by "  # TypeParam = ...";
# its cases follow, indented. Other unindented lines (gtest_main's "Running main() from ...") are
# skipped. A suite whose cases are all DISABLED_ is left out too: its entry would run nothing.
string(REPLACE "\r" "" output "${output}")
string(REPLACE ";" "\\;" output "${output}")
string(REPLACE "\n" ";" lines "${output}")
# Each suite's enabled cases are recorded too, as _cases_<suite as a C identifier>, for the suites
# registered one entry per case (ISOLATE).
set(suites "")
set(suite "")
set(content "")
foreach(line IN LISTS lines)
    if(line MATCHES "^([A-Za-z_][A-Za-z0-9_/]*)\\.(  #.*)?$")
        set(suite "${CMAKE_MATCH_1}")
    elseif(NOT suite STREQUAL "" AND line MATCHES "^  ([A-Za-z0-9_/]+)")
        set(case "${CMAKE_MATCH_1}") # before the next MATCHES clears CMAKE_MATCH_1
        if(case MATCHES "^DISABLED_" OR suite MATCHES "^DISABLED_|/DISABLED_")
            continue()
        endif()
        if(NOT suite IN_LIST suites)
            list(APPEND suites "${suite}")
        endif()
        string(MAKE_C_IDENTIFIER "${suite}" id)
        list(APPEND cases_${id} "${case}")
    endif()
endforeach()
if(NOT suites)
    file(REMOVE "${SUITES_FILE}")
    message(FATAL_ERROR "${TEST_EXECUTABLE} --gtest_list_tests listed no test suites:\n${output}")
endif()
foreach(suite IN LISTS suites)
    string(MAKE_C_IDENTIFIER "${suite}" id)
    string(APPEND content "set(_cases_${id} [==[${cases_${id}}]==])\n")
endforeach()
file(WRITE "${SUITES_FILE}" "set(_suites [==[${suites}]==])\n${content}")
