# The DISCOVERY_TIMEOUT default of tasksmack_gtest_add_suites() (cmake/GTestSuites.cmake), run
# with -P:
#   cmake -DTS_SOURCE_DIR=<checkout> -P GTestSuitesTimeoutCheck.cmake
# It calls the function with no DISCOVERY_TIMEOUT and captures the -DDISCOVERY_TIMEOUT= its
# post-build discovery step carries (#2838): the default must keep headroom for a loaded runner,
# where the old 30 s cap killed <binary> --gtest_list_tests and failed the Build step.

cmake_minimum_required(VERSION 3.29) # a script run with -P starts with no policies; CMP0057, as GTestSuitesList.cmake

if(NOT TS_SOURCE_DIR)
    message(FATAL_ERROR "GTestSuitesTimeoutCheck: pass -DTS_SOURCE_DIR=<checkout>")
endif()

# add_custom_command() has no read-back, so catch the argument the function hands it: in script
# mode a user-defined command shadows the built-in.
function(add_custom_command)
    foreach(_arg IN LISTS ARGN)
        if(_arg MATCHES "^-DDISCOVERY_TIMEOUT=(.+)$")
            set_property(GLOBAL PROPERTY gtest_suites_timeout "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endfunction()

include("${TS_SOURCE_DIR}/cmake/GTestSuites.cmake")
tasksmack_gtest_add_suites(gtest_suites_timeout_dummy)

get_property(default_timeout GLOBAL PROPERTY gtest_suites_timeout)
if(NOT default_timeout)
    message(FATAL_ERROR "tasksmack_gtest_add_suites() emitted no -DDISCOVERY_TIMEOUT= for its discovery step; the capture caught nothing")
endif()
if(NOT default_timeout EQUAL 120)
    message(FATAL_ERROR "tasksmack_gtest_add_suites() discovers under a ${default_timeout} s cap by default, expected 120 s")
endif()
message(STATUS "GTestSuitesTimeoutCheck: discovery defaults to ${default_timeout} s")
