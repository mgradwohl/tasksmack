# Google Test setup
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 063de7e9578f82b369302001269680b4b1553359  # v1.18.0 - pinned to SHA for supply chain security
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
        Platform/test_WindowsNVMLGPUProbe.cpp
        Platform/test_WindowsPDHGPUProbe.cpp
        Platform/test_WindowsGPUProbe.cpp
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
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/PDHGPUProbe.cpp
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
        Platform/test_LinuxGPUProbe.cpp
        Platform/test_LinuxNVMLGPUProbe.cpp
        Platform/test_LinuxROCmGPUProbe.cpp
        Platform/test_NetlinkSocketStats.cpp
        Platform/test_DRMGPUProbe.cpp
        Platform/test_Factory.cpp
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
    Core/test_EnvUtils.cpp
    Core/test_Event.cpp
    Core/test_Layer.cpp
    Core/test_PathService.cpp
    Core/test_VideoBackend.cpp
    Core/test_Window.cpp
    Core/test_WindowEvents.cpp
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
    UI/test_RenderMetrics.cpp
    UI/test_ThemeLoader.cpp
    UI/test_AssetPath.cpp
    UI/test_ThemeHeader.cpp
    UI/test_WidgetsHeader.cpp
    UI/test_MonospaceFontPath.cpp
    UI/test_DpiScale.cpp
    App/test_ProcessColumnConfig.cpp
    App/test_ProcessSortUtils.cpp
    App/test_AdaptiveIntervalUtils.cpp
    App/test_FpsCounter.cpp
    App/test_Panel.cpp
    App/test_ProcessesPanel.cpp
    App/test_ShellLayer.cpp
    App/test_TitleBarGeometry.cpp
    App/test_UserConfig.cpp
    App/test_UserConfigHelpers.cpp
    App/test_UserConfigPersistence.cpp
    App/test_ActiveTab.cpp
    App/test_PriorityHelpers.cpp
    App/test_SettingsLayerDetail.cpp
    App/test_NetInterfaceUtils.cpp
    App/test_SingletonLayers.cpp
    ${PLATFORM_TEST_SOURCES}
    ${INTEGRATION_TEST_SOURCES}
    # Source files under test
    ${CMAKE_SOURCE_DIR}/src/Core/Application.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/PathService.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/VideoBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/Window.cpp
    # App layer
    ${CMAKE_SOURCE_DIR}/src/App/UserConfig.cpp
    # Domain layer
    ${CMAKE_SOURCE_DIR}/src/Domain/ProcessModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/GPUModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/StorageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/BackgroundSampler.cpp
    # UI source files under test (Note: Theme.cpp excluded - requires ImGui/ImPlot runtime context)
    ${CMAKE_SOURCE_DIR}/src/UI/ThemeLoader.cpp
    ${CMAKE_SOURCE_DIR}/src/UI/AssetPath.cpp
    ${CMAKE_SOURCE_DIR}/src/UI/MonospaceFontPath.cpp
    # Stub: satisfies Theme symbols referenced by UserConfig.cpp without a live ImGui context
    ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/ThemeStub.cpp
    ${PLATFORM_SRC_UNDER_TEST}
)

target_include_directories(TaskSmackTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}  # For tests/Mocks includes
    ${CMAKE_BINARY_DIR}/generated  # For generated version.h
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
    ${implot_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/gladsources/include  # For glad/gl.h
)

target_link_libraries(TaskSmackTests PRIVATE
    gtest
    gmock
    spdlog::spdlog_header_only
    tomlplusplus::tomlplusplus
    SDL3::SDL3
    glad_gl_core_33
)
tasksmack_apply_linux_toolchain(TaskSmackTests)

# Platform-specific library linking
if(UNIX AND NOT APPLE)
    # Linux needs X11 for platform tests
    find_package(X11 REQUIRED)
    target_link_libraries(TaskSmackTests PRIVATE X11::X11 ${CMAKE_DL_LIBS})

    add_library(TaskSmackNVMLMock SHARED
        Mocks/NVMLMock.cpp
    )
    target_include_directories(TaskSmackNVMLMock PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    set_target_properties(TaskSmackNVMLMock PROPERTIES
        PREFIX ""
        SUFFIX ""
        OUTPUT_NAME "libnvidia-ml.so.1"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mocks"
    )
    tasksmack_apply_default_warnings(TaskSmackNVMLMock)

    add_library(TaskSmackROCmMock SHARED
        Mocks/ROCmMock.cpp
    )
    target_include_directories(TaskSmackROCmMock PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    set_target_properties(TaskSmackROCmMock PROPERTIES
        PREFIX ""
        SUFFIX ""
        OUTPUT_NAME "librocm_smi64.so.6"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mocks"
    )
    tasksmack_apply_default_warnings(TaskSmackROCmMock)

    add_dependencies(TaskSmackTests TaskSmackNVMLMock TaskSmackROCmMock)
    target_compile_definitions(TaskSmackTests PRIVATE
        TASKSMACK_TEST_GPU_MOCK_DIR="${CMAKE_CURRENT_BINARY_DIR}/mocks"
    )
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
        version
    )
endif()

tasksmack_apply_default_warnings(TaskSmackTests)

# Precompiled headers for tests (speeds up compilation)
if(TASKSMACK_ENABLE_PCH)
    include(../cmake/TestPrecompiledHeaders.cmake)
    target_precompile_headers(TaskSmackTests PRIVATE ${TASKSMACK_TEST_PCH_HEADERS})
endif()

# Increase test discovery timeout on Windows runners to avoid spurious timeouts
# when the test executable links many platform libraries and starts slowly.
if(WIN32)
    gtest_discover_tests(TaskSmackTests
        DISCOVERY_TIMEOUT 30
    )
else()
    # Prepend the mocks directory to LD_LIBRARY_PATH before the test process starts so
    # dlopen("libnvidia-ml.so.1") and dlopen("librocm_smi64.so.6") find the mock shared
    # libraries built in the mocks/ directory. The dynamic linker caches LD_LIBRARY_PATH at
    # process startup; runtime setenv() does not affect subsequent dlopen() calls, so the
    # path must be injected via CTest's ENVIRONMENT_MODIFICATION test property.
    # ENVIRONMENT_MODIFICATION prepends rather than replacing, preserving any SDL/driver/system
    # library paths already set by the developer or CI runner.
    gtest_discover_tests(TaskSmackTests
        PROPERTIES
            ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:${CMAKE_CURRENT_BINARY_DIR}/mocks"
    )
endif()
