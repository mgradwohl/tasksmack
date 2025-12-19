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

# Platform-specific test sources
if(WIN32)
    set(PLATFORM_TEST_SOURCES
        Platform/test_ProcessProbeContract.cpp
        Platform/test_SystemProbeContract.cpp
        Platform/test_ProcessActionsContract.cpp
        Platform/test_WindowsProcessProbe.cpp
        Platform/test_WindowsSystemProbe.cpp
        Platform/test_WindowsProcessActions.cpp
    )
    set(PLATFORM_SRC_UNDER_TEST
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/Factory.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsProcessProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsSystemProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsProcessActions.cpp
    )
    set(INTEGRATION_TEST_SOURCES
        Integration/test_CrossLayer.cpp
        # Windows-specific integration tests would go here
    )
else()
    set(PLATFORM_TEST_SOURCES
        Platform/test_ProcessProbeContract.cpp
        Platform/test_SystemProbeContract.cpp
        Platform/test_ProcessActionsContract.cpp
        Platform/test_LinuxProcessProbe.cpp
        Platform/test_LinuxSystemProbe.cpp
        Platform/test_LinuxProcessActions.cpp
    )
    set(PLATFORM_SRC_UNDER_TEST
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/Factory.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxProcessProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxSystemProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxProcessActions.cpp
    )
    set(INTEGRATION_TEST_SOURCES
        Integration/test_CrossLayer.cpp
        Integration/test_LinuxRealProbes.cpp
    )
endif()

# Test executable with all test sources
add_executable(TaskSmackTests
    test_main.cpp
    Domain/test_History.cpp
    Domain/test_ProcessModel.cpp
    Domain/test_SystemModel.cpp
    Domain/test_BackgroundSampler.cpp
    ${PLATFORM_TEST_SOURCES}
    ${INTEGRATION_TEST_SOURCES}
    # Source files under test (Domain layer is header + implementation)
    ${CMAKE_SOURCE_DIR}/src/Domain/ProcessModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/BackgroundSampler.cpp
    ${PLATFORM_SRC_UNDER_TEST}
)

target_include_directories(TaskSmackTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}  # For tests/Mocks includes
)

target_link_libraries(TaskSmackTests PRIVATE
    gtest_main
    spdlog::spdlog_header_only
)

# Platform-specific library linking
if(UNIX AND NOT APPLE)
    # Linux needs X11 for platform tests
    find_package(X11 REQUIRED)
    target_link_libraries(TaskSmackTests PRIVATE X11::X11)
endif()

if(WIN32)
    # Windows needs additional system libraries
    target_link_libraries(TaskSmackTests PRIVATE 
        pdh
        psapi
        userenv
    )
endif()

tasksmack_apply_default_warnings(TaskSmackTests)

# Precompiled headers for tests (speeds up compilation)
if(TASKSMACK_ENABLE_PCH)
    target_precompile_headers(TaskSmackTests PRIVATE
        <gtest/gtest.h>
        <memory>
        <vector>
        <string>
        <chrono>
    )
endif()

gtest_discover_tests(TaskSmackTests)
