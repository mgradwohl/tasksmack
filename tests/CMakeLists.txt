# Google Test setup
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 063de7e9578f82b369302001269680b4b1553359  # v1.18.0 - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings from gtest
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/googletest-build"  # per-preset build tree (#1308)
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

if(WIN32)
    find_program(TASKSMACK_PWSH NAMES pwsh)
    if(TASKSMACK_PWSH)
        add_test(NAME ResizeCaptureScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-profile-etw-resize.ps1)
        add_test(NAME EtwCaptureScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-profile-etw.ps1)
        # Runs real stub apps through the app-capture lifecycle (#1186), about 30 s; keep it clear
        # of the 60 s --timeout the CI ctest steps pass under parallel load.
        set_tests_properties(EtwCaptureScript PROPERTIES TIMEOUT 180)
        add_test(NAME EtwAnalysisScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-analyze-etw.ps1)
    else()
        # Say so rather than leave the ETW script tests silently missing from ctest (#1187).
        message(WARNING "pwsh (PowerShell 7) not found: the ResizeCaptureScript, EtwCaptureScript and "
                        "EtwAnalysisScript tests are not registered. Install PowerShell 7 to run them.")
    endif()
endif()

# tools/check-benchmark-regression.py gates CI on benchmark regressions; Python3_EXECUTABLE is
# already required (GLAD generation, cmake/Dependencies.cmake).
add_test(NAME BenchmarkRegressionGateScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_benchmark_regression.py)

# Test sources. Every list is defined on every platform, so the registration check below can see
# all of them; the platform branch then picks its own.
set(COMMON_TEST_SOURCES
    test_main.cpp
    Benchmarks/test_AllocationCounter.cpp
    Core/test_Application.cpp
    Core/test_EnvUtils.cpp
    Core/test_ApplicationEvents.cpp
    Core/test_Event.cpp
    Core/test_Layer.cpp
    Core/test_PathService.cpp
    Core/test_VideoBackend.cpp
    Core/test_Window.cpp
    Core/test_WindowEventRouting.cpp
    Core/test_WindowEvents.cpp
    Core/test_WindowGeometry.cpp
    Domain/test_History.cpp
    Domain/test_SharedHistory.cpp
    Domain/test_Numeric.cpp
    Domain/test_SingleLineText.cpp
    Domain/test_ProcessEnergyAttribution.cpp
    Domain/test_ProcessModel.cpp
    Domain/test_SocketTrafficAccumulator.cpp
    Domain/test_GPUModel.cpp
    Domain/test_ProcessStatus.cpp
    Domain/test_ProcessState.cpp
    Domain/test_SystemModel.cpp
    Domain/test_SystemModelAllocationFailure.cpp
    Domain/test_StorageModel.cpp
    Domain/test_BackgroundSampler.cpp
    Domain/test_PriorityConfig.cpp
    Domain/test_SamplingConfig.cpp
    Domain/test_HistorySeries.cpp
    UI/test_ChartWidgets.cpp
    UI/test_ChartGridLayout.cpp
    UI/test_HistoryPlotHeight.cpp
    UI/test_FillPlotLayout.cpp
    UI/test_ChartGeometryBudget.cpp
    UI/test_EmptyState.cpp
    UI/test_Format.cpp
    UI/test_RenderMetrics.cpp
    UI/test_RateAxis.cpp
    UI/test_InlineText.cpp
    UI/test_BundledThemes.cpp
    UI/test_ColorContrast.cpp
    UI/test_ColorDifference.cpp
    UI/test_StyleScale.cpp
    UI/test_DialogMetrics.cpp
    UI/test_LineLayout.cpp
    UI/test_IconEncoding.cpp
    App/test_DialogGeometry.cpp
    UI/test_ThemeCatalog.cpp
    UI/test_ThemeLoader.cpp
    UI/test_AssetPath.cpp
    UI/test_ThemeHeader.cpp
    UI/test_WidgetsHeader.cpp
    UI/test_IconLoader.cpp
    UI/test_MonospaceFontPath.cpp
    UI/test_DpiScale.cpp
    UI/test_FontFileCache.cpp
    UI/test_TabularDigits.cpp
    App/test_ProcessColumnConfig.cpp
    App/test_ProcessSortUtils.cpp
    App/test_AdaptiveIntervalUtils.cpp
    App/test_ProcessTreeFlatten.cpp
    App/test_ProcessTreeIndent.cpp
    App/test_ProcessTableFlags.cpp
    App/test_ProcessTableLayout.cpp
    App/test_ProcessTableSettings.cpp
    App/test_ProcessTableSettingsRoundTrip.cpp
    App/test_ProcessDetailsLayout.cpp
    App/test_GpuClockReference.cpp
    App/test_GpuSeriesScale.cpp
    App/test_GpuSectionEmptyState.cpp
    App/test_GpuSectionDrawList.cpp
    App/test_ProcessRowFormat.cpp
    App/test_ProcessColumnAvailability.cpp
    App/test_ProcessStateColor.cpp
    App/test_ProcessTypeColor.cpp
    App/test_ProcessCellWidths.cpp
    App/test_ProcessFilterCache.cpp
    App/test_ProcessDisplayFreeze.cpp
    App/test_CpuCoreGridIds.cpp
    App/test_CpuSummaryText.cpp
    Platform/test_CgroupFreezerPath.cpp
    Platform/test_ProcessIdentity.cpp
    Platform/test_ProcessName.cpp
    Platform/test_CpuAffinity.cpp
    Platform/test_ThreadName.cpp
    App/test_FpsCounter.cpp
    App/test_Panel.cpp
    App/test_ProcessesPanel.cpp
    App/test_ShellLayer.cpp
    App/test_TitleBarGeometry.cpp
    App/test_UserConfig.cpp
    App/test_UserConfigHelpers.cpp
    App/test_UserConfigPersistence.cpp
    App/test_InstanceLock.cpp
    App/test_PanelTabs.cpp
    App/test_TabLabel.cpp
    App/test_PriorityHelpers.cpp
    App/test_ProcessGpuHelpers.cpp
    App/test_ProcessResourceHelpers.cpp
    App/test_ProcessHistoryHelpers.cpp
    App/test_ProcessDetailsHistory.cpp
    App/test_ProcessActionHelpers.cpp
    App/test_ProcessActionsView.cpp
    App/test_ProcessActionConfirmPopup.cpp
    App/test_SettingsLayerDetail.cpp
    App/test_NetInterfaceUtils.cpp
    App/test_PowerStatusText.cpp
    App/test_ElevationNoticeText.cpp
    App/test_FontSizeChange.cpp
    App/test_SyntheticScenario.cpp
    Platform/test_SyntheticWorkload.cpp
)
# The Windows probes' pure *Math.h helpers include no Windows header, so their tests are built on
# every platform: Linux CI runs them under its sanitizers and coverage too (#1133). Tests that need
# a probe, Windows types or fakes of Windows APIs stay in WINDOWS_PLATFORM_TEST_SOURCES.
set(WINDOWS_MATH_TEST_SOURCES
    Platform/WindowsMath/test_DXGIGPUProbeMath.cpp
    Platform/WindowsMath/test_WindowsDiskProbeMath.cpp
    Platform/WindowsMath/test_WindowsGPUProbeMath.cpp
    Platform/WindowsMath/test_WindowsPathProviderMath.cpp
    Platform/WindowsMath/test_WindowsPowerProbeMath.cpp
    Platform/WindowsMath/test_WindowsProcessProbeMath.cpp
    Platform/WindowsMath/test_WindowsSystemProbeMath.cpp
)
list(APPEND COMMON_TEST_SOURCES ${WINDOWS_MATH_TEST_SOURCES})
set(WINDOWS_PLATFORM_TEST_SOURCES
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
    Platform/test_WindowsDXGIGPUProbe.cpp
    Platform/test_WindowsGPURescan.cpp
    Platform/test_WindowsProcAddress.cpp
    Platform/test_WinString.cpp
)
set(WINDOWS_INTEGRATION_TEST_SOURCES
    Integration/test_CrossLayer.cpp
    Integration/test_WindowsRealProbes.cpp
)
set(LINUX_PLATFORM_TEST_SOURCES
    Platform/test_ProcessProbeContract.cpp
    Platform/test_SystemProbeContract.cpp
    Platform/test_ProcessActionsContract.cpp
    Platform/test_PathProviderContract.cpp
    Platform/test_PowerProbeContract.cpp
    Platform/test_LinuxProcessProbe.cpp
    Platform/test_LinuxSystemProbe.cpp
    Platform/test_LinuxProcessActions.cpp
    Platform/test_LinuxProcessStatus.cpp
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
set(LINUX_INTEGRATION_TEST_SOURCES
    Integration/test_CrossLayer.cpp
    Integration/test_LinuxRealProbes.cpp
)

# Platform-specific test sources
if(WIN32)
    set(PLATFORM_TEST_SOURCES ${WINDOWS_PLATFORM_TEST_SOURCES})
    set(INTEGRATION_TEST_SOURCES ${WINDOWS_INTEGRATION_TEST_SOURCES})
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
else()
    set(PLATFORM_TEST_SOURCES ${LINUX_PLATFORM_TEST_SOURCES})
    set(INTEGRATION_TEST_SOURCES ${LINUX_INTEGRATION_TEST_SOURCES})
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
endif()

# Every tests/**/test_*.cpp must be in one of the lists above; otherwise it is never built and its
# tests silently never run, as test_Numeric.cpp wasn't for months (#1135). Compared against the
# lists themselves, not the text of this file, so a mention in a comment doesn't count.
set(TASKSMACK_REGISTERED_TEST_SOURCES
    ${COMMON_TEST_SOURCES}
    ${WINDOWS_PLATFORM_TEST_SOURCES}
    ${WINDOWS_INTEGRATION_TEST_SOURCES}
    ${LINUX_PLATFORM_TEST_SOURCES}
    ${LINUX_INTEGRATION_TEST_SOURCES}
)
file(GLOB_RECURSE TASKSMACK_TEST_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
foreach(test_file IN LISTS TASKSMACK_TEST_FILES)
    if(NOT test_file IN_LIST TASKSMACK_REGISTERED_TEST_SOURCES)
        message(FATAL_ERROR "tests/${test_file} is not in any test source list in tests/CMakeLists.txt, so it would never be built or run")
    endif()
endforeach()

# Test executable with all test sources
add_executable(TaskSmackTests
    ${COMMON_TEST_SOURCES}
    ${PLATFORM_TEST_SOURCES}
    ${INTEGRATION_TEST_SOURCES}
    # Source files under test
    ${CMAKE_SOURCE_DIR}/src/Core/Application.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/PathService.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/VideoBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/Window.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/X11WindowManager.cpp
    # App layer
    ${CMAKE_SOURCE_DIR}/src/App/UserConfig.cpp
    ${CMAKE_SOURCE_DIR}/src/App/InstanceLock.cpp
    # Synthetic scenario (#1413): the env-var parser, composition helpers and history preload
    ${CMAKE_SOURCE_DIR}/src/App/SyntheticScenario.cpp
    # The process-action confirm modal and the Actions tab, run headless (test_ProcessActionConfirmPopup.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessActionConfirm.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessActionsView.cpp
    # Domain layer
    ${CMAKE_SOURCE_DIR}/src/Domain/ProcessModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/GPUModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/StorageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/BackgroundSampler.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/ThreadName.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/Synthetic/SyntheticProbes.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/Synthetic/SyntheticWorkload.cpp
    # UI source files under test (Note: Theme.cpp excluded - requires ImGui/ImPlot runtime context)
    ${CMAKE_SOURCE_DIR}/src/UI/ThemeLoader.cpp
    ${CMAKE_SOURCE_DIR}/src/UI/AssetPath.cpp
    # IconLoader.cpp only needs imgui.h for type declarations (no ImGui:: calls) and its GL
    # calls resolve against glad_gl_core_33 (linked below) - unlike Theme.cpp it doesn't need
    # a live ImGui/GL context to link, only to actually load a texture successfully (#771).
    ${CMAKE_SOURCE_DIR}/src/UI/IconLoader.cpp
    ${CMAKE_SOURCE_DIR}/src/UI/MonospaceFontPath.cpp
    # Marker glyphs for the value strip and series, drawn by test_FillPlotLayout.cpp and
    # test_ChartGeometryBudget.cpp under a live ImGui context
    ${CMAKE_SOURCE_DIR}/src/UI/ChartLegend.cpp
    # Measures font files with FreeType (linked below), no ImGui context needed (#1201)
    ${CMAKE_SOURCE_DIR}/src/UI/TabularDigits.cpp
    # Stub: satisfies Theme symbols referenced by UserConfig.cpp without a live ImGui context
    ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/ThemeStub.cpp
    ${PLATFORM_SRC_UNDER_TEST}
)

# The Theme stub defines Theme::get() & co. with the same names (and bodies) as src/UI/Theme.cpp.
# With honest coverage (#1131) the report merges the test binary with the app binary, and LLVM matches
# profile counters by function name and hash, not by binary -- so stub executions were credited to the
# production Theme.cpp, which no test runs. Leave the stub uninstrumented; Theme.cpp keeps its coverage.
if(TASKSMACK_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(TASKSMACK_NO_COVERAGE_FLAGS "/clang:-fno-profile-instr-generate" "/clang:-fno-coverage-mapping")
    else()
        set(TASKSMACK_NO_COVERAGE_FLAGS -fno-profile-instr-generate -fno-coverage-mapping)
    endif()
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Mocks/ThemeStub.cpp PROPERTIES COMPILE_OPTIONS "${TASKSMACK_NO_COVERAGE_FLAGS}")
endif()

target_include_directories(TaskSmackTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}  # For tests/Mocks includes
    ${CMAKE_SOURCE_DIR}/benchmarks  # For AllocationCounter.h (std-only; no Google Benchmark dependency)
    ${CMAKE_BINARY_DIR}/generated  # For generated version.h
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
    ${implot_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/gladsources/include  # For glad/gl.h
)
target_include_directories(TaskSmackTests SYSTEM PRIVATE ${stb_SOURCE_DIR})  # For IconLoader.cpp

target_link_libraries(TaskSmackTests PRIVATE
    gtest
    gmock
    spdlog::spdlog_header_only
    tomlplusplus::tomlplusplus
    SDL3::SDL3
    glad_gl_core_33
    Freetype::Freetype  # For TabularDigits.cpp
    # test_TabLabel.cpp checks labels against ImGui's own ImHashStr() (no ImGui context needed);
    # test_ProcessTableSettingsRoundTrip.cpp runs headless ImGui frames to load saved table layouts.
    imgui_lib
    # test_ChartGeometryBudget.cpp renders the real ImPlot charts headless to count their geometry (#1421).
    implot_lib
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

# test_BundledThemes.cpp checks the shipped theme files themselves, from the source tree.
target_compile_definitions(TaskSmackTests PRIVATE TASKSMACK_SOURCE_THEMES_DIR="${CMAKE_SOURCE_DIR}/assets/themes")

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
        cfgmgr32 # DisplayDevicePower: a GPU's PnP power state (#1265)
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
