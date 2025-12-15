# Google Test setup
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    SYSTEM  # Treat as system headers to suppress warnings from gtest
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# Disable GoogleTest's own compile warnings
set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Ensure Google Test headers are treated as SYSTEM to suppress warnings
# Apply SYSTEM include directories to all gtest/gmock targets
foreach(_gtest_target gtest gtest_main gmock gmock_main)
    if(TARGET ${_gtest_target})
        get_target_property(_includes ${_gtest_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(_includes)
            set_target_properties(${_gtest_target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_includes}"
            )
        endif()
        # Suppress warnings when compiling gtest itself
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
            target_compile_options(${_gtest_target} PRIVATE -w)
        elseif(MSVC)
            target_compile_options(${_gtest_target} PRIVATE /w)
        endif()
    endif()
endforeach()

include(GoogleTest)

# Example test executable
add_executable(MyProject_tests
    test_main.cpp
)

target_link_libraries(MyProject_tests PRIVATE
    GTest::gtest_main
)

myproject_apply_default_warnings(MyProject_tests)

gtest_discover_tests(MyProject_tests)
