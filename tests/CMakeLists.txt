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
        Platform/test_PathProviderContract.cpp
        Platform/test_PowerProbeContract.cpp
        Platform/test_WindowsProcessProbe.cpp
        Platform/test_WindowsSystemProbe.cpp
        Platform/test_WindowsProcessActions.cpp
        Platform/test_WindowsDiskProbe.cpp
        Platform/test_WindowsPathProvider.cpp
        Platform/test_WindowsPowerProbe.cpp
        Platform/test_WinString.cpp
    )
    set(PLATFORM_SRC_UNDER_TEST
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/Factory.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsProcessProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsSystemProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsProcessActions.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsDiskProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsPathProvider.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsPowerProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsGPUProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/DXGIGPUProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/NVMLGPUProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/D3DKMTGPUProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WinString.cpp
    )
    set(INTEGRATION_TEST_SOURCES
        Integration/test_CrossLayer.cpp
        Integration/test_WindowsRealProbes.cpp
    )
else()
    set(PLATFORM_TEST_SOURCES
        Platform/test_ProcessProbeContract.cpp
        Platform/test_SystemProbeContract.cpp
        Platform/test_ProcessActionsContract.cpp
        Platform/test_PathProviderContract.cpp
        Platform/test_PowerProbeContract.cpp
        Platform/test_LinuxProcessProbe.cpp
        Platform/test_LinuxSystemProbe.cpp
        Platform/test_LinuxProcessActions.cpp
        Platform/test_LinuxDiskProbe.cpp
        Platform/test_LinuxPathProvider.cpp
        Platform/test_LinuxPowerProbe.cpp
        Platform/test_NetlinkSocketStats.cpp
    )
    set(PLATFORM_SRC_UNDER_TEST
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/Factory.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxProcessProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxSystemProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxProcessActions.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxDiskProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxPathProvider.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxPowerProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxGPUProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/NVMLGPUProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/DRMGPUProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/ROCmGPUProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/NetlinkSocketStats.cpp
    )
    set(INTEGRATION_TEST_SOURCES
        Integration/test_CrossLayer.cpp
        Integration/test_LinuxRealProbes.cpp
    )
endif()

# Test executable with all test sources
add_executable(TaskSmackTests
    test_main.cpp
    Core/test_Application.cpp
    Core/test_Layer.cpp
    Domain/test_History.cpp
    Domain/test_ProcessModel.cpp
    Domain/test_GPUModel.cpp
    Domain/test_ProcessStatus.cpp
    Domain/test_SystemModel.cpp
    Domain/test_StorageModel.cpp
    Domain/test_BackgroundSampler.cpp
    Domain/test_PriorityConfig.cpp
    Domain/test_SamplingConfig.cpp
    UI/test_ChartWidgets.cpp
    UI/test_Format.cpp
    UI/test_ThemeLoader.cpp
    App/test_ProcessColumnConfig.cpp
    App/test_ProcessesPanel.cpp
    App/test_UserConfig.cpp
    App/test_UserConfigPersistence.cpp
    App/test_ActiveTab.cpp
    App/test_PriorityHelpers.cpp
    App/test_SettingsLayerDetail.cpp
    App/test_NetInterfaceUtils.cpp
    ${PLATFORM_TEST_SOURCES}
    ${INTEGRATION_TEST_SOURCES}
    # Source files under test
    ${CMAKE_SOURCE_DIR}/src/Core/Application.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/Window.cpp
    # Domain layer
    ${CMAKE_SOURCE_DIR}/src/Domain/ProcessModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/GPUModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/StorageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/BackgroundSampler.cpp
    # UI source files under test (Note: Theme.cpp excluded - requires ImGui/ImPlot)
    ${CMAKE_SOURCE_DIR}/src/UI/ThemeLoader.cpp
    ${PLATFORM_SRC_UNDER_TEST}
)

target_include_directories(TaskSmackTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}  # For tests/Mocks includes
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
    ${implot_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/gladsources/include  # For glad/gl.h
)

target_link_libraries(TaskSmackTests PRIVATE
    gtest_main
    spdlog::spdlog_header_only
    tomlplusplus::tomlplusplus
    glfw
    glad_gl_core_33
)

# Platform-specific library linking
if(UNIX AND NOT APPLE)
    # Linux needs X11 for platform tests
    find_package(X11 REQUIRED)
    target_link_libraries(TaskSmackTests PRIVATE X11::X11 ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    # Windows SDK version: GetIfTable2 and MIB_IF_TABLE2 are available on Vista+,
    # but we intentionally target Windows 10+ via _WIN32_WINNT=0x0A00 / WINVER=0x0A00.
    # NTDDI_VERSION is required for iphlpapi.h to include netioapi.h (GetIfTable2 APIs).
    target_compile_definitions(TaskSmackTests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NTDDI_VERSION=0x0A000000
    )
    # Windows needs additional system libraries
    target_link_libraries(TaskSmackTests PRIVATE
        pdh
        psapi
        iphlpapi
        ws2_32
        userenv
        dxgi
        gdi32
    )
endif()

tasksmack_apply_default_warnings(TaskSmackTests)

# Precompiled headers for tests (speeds up compilation)
if(TASKSMACK_ENABLE_PCH)
    include(../cmake/TestPrecompiledHeaders.cmake)
    target_precompile_headers(TaskSmackTests PRIVATE ${TASKSMACK_TEST_PCH_HEADERS})
endif()

gtest_discover_tests(TaskSmackTests)
