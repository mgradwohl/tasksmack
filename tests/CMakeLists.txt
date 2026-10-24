# Anything installed from here (rapidcheck's install() rules are unconditional: headers, library,
# CMake and pkg-config files) goes into a "tests" component, which cmake/Packaging.cmake leaves out
# of the packages (#1398). Directory-scoped, so it doesn't reach the app's own install rules.
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME tests)

# Google Test setup
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 063de7e9578f82b369302001269680b4b1553359  # v1.18.0 - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings from gtest
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/googletest-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/googletest-subbuild"  # per-host sub-build (#2253)
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# Disable GoogleTest's own compile warnings
set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Property-based testing (#139): rapidcheck and its Google Test integration (RC_GTEST_PROP). It has no
# releases, so a master commit is pinned; its ext/ submodules are only for its own tests and examples.
FetchContent_Declare(
    rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG 6e8dadfdafa3a74eabb52ead87f8787f72eccd0b  # master 2026-08-06 - pinned to SHA for supply chain security
    GIT_SUBMODULES ""
    SYSTEM  # Treat as system headers to suppress warnings from rapidcheck
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/rapidcheck-build"  # per-preset build tree (#1308)
    SUBBUILD_DIR "${TASKSMACK_DEPS_SUBBUILD_DIR}/rapidcheck-subbuild"  # per-host sub-build (#2253)
)
set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)
set(RC_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(RC_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rapidcheck)

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

# tasksmack_gtest_add_suites(): one ctest entry per gtest suite, not per case (#1871)
include(../cmake/GTestSuites.cmake)

if(WIN32)
    find_program(TASKSMACK_PWSH NAMES pwsh)
    if(TASKSMACK_PWSH)
        add_test(NAME ResizeCaptureScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-profile-etw-resize.ps1)
        # Runs real stub apps through the app-capture lifecycle (#1186).
        add_test(NAME EtwCaptureScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-profile-etw.ps1)
        add_test(NAME EtwAnalysisScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-analyze-etw.ps1)
        # tools/cmake-cache.ps1, the CMakeCache.txt reader bench.ps1 and the ETW scripts share (#1479).
        add_test(NAME CMakeCacheScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-cmake-cache.ps1)
        # tools/bench.ps1 against a stub benchmark binary: exit-code propagation (#1423) and the
        # provenance manifest (#1424). The stub benchmark is a Python script when CMake's interpreter
        # is passed in (much cheaper to start than PowerShell).
        add_test(NAME BenchScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-bench.ps1)
        set_tests_properties(BenchScript PROPERTIES ENVIRONMENT "TASKSMACK_TEST_PYTHON=${Python3_EXECUTABLE}")
        # tools/check-prereqs.ps1 reads a two-component ccache version (#2350); one full checker run.
        add_test(NAME CheckPrereqsScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-check-prereqs.ps1)
        # tools/setup-dev.ps1 installs winget's newest version of a package, with a warning, when the
        # catalog no longer carries the pinned one (#2844). Stub winget; nothing is installed.
        add_test(NAME SetupDevScript
            COMMAND ${TASKSMACK_PWSH} -NoProfile -File ${CMAKE_SOURCE_DIR}/tools/test-setup-dev.ps1)
        # The PowerShell script tests take turns (#2054). Each starts pwsh, several compile C# with
        # Add-Type, and some start further pwsh or stub processes; run together under `ctest -j` on a
        # 4-core CI runner they slowed each other down many times over (CMakeCacheScript, 1.5-4.4 s
        # alone, passed 60 s beside ResizeCaptureScript, BenchScript and BenchShScript). Taking turns
        # costs about a minute in all (EtwCaptureScript 33-52 s, BenchScript 14-17 s, the rest under
        # 5 s each on CI), which the gtest suites fill on the other slots.
        #
        # Their own TIMEOUTs, rather than the CI steps' 60 s --timeout: about twice the slowest CI
        # time seen, measured with the scripts all running at once (EtwCaptureScript 74 s, BenchScript
        # 44 s, ResizeCaptureScript 26 s), and 120 s for the short ones.
        set_tests_properties(ResizeCaptureScript EtwCaptureScript EtwAnalysisScript CMakeCacheScript BenchScript
            CheckPrereqsScript SetupDevScript PROPERTIES RESOURCE_LOCK pwsh-script-tests)
        set_tests_properties(ResizeCaptureScript EtwAnalysisScript CMakeCacheScript CheckPrereqsScript SetupDevScript
            PROPERTIES TIMEOUT 120)
        set_tests_properties(EtwCaptureScript BenchScript PROPERTIES TIMEOUT 180)
    else()
        # Say so rather than leave the ETW script tests silently missing from ctest (#1187).
        message(WARNING "pwsh (PowerShell 7) not found: the ResizeCaptureScript, EtwCaptureScript, "
                        "EtwAnalysisScript, CMakeCacheScript, BenchScript, CheckPrereqsScript and SetupDevScript tests are not registered. Install PowerShell 7 to run them.")
    endif()
    # The Authenticode gate on packaging TaskSmackHelper.exe (#2550): the build's own, unsigned
    # executables are refused, by name, before anything could be packaged.
    if(TARGET TaskSmackHelper)
        add_test(NAME WindowsSignatureGateScript
            COMMAND ${CMAKE_COMMAND} -DTASKSMACK_VERIFY_DIR=$<TARGET_FILE_DIR:TaskSmack>
                    -P ${CMAKE_SOURCE_DIR}/cmake/VerifyWindowsSignatures.cmake)
        set_tests_properties(WindowsSignatureGateScript PROPERTIES
            # CMake wraps the message, so any run of spaces and line breaks may separate the words.
            PASS_REGULAR_EXPRESSION "TaskSmack(Helper)?\\.exe[ \r\n]+isn't[ \r\n]+Authenticode-signed[ \r\n]+\\(NotSigned\\)"
            # It starts PowerShell too, so it takes its turn with the script tests above (#2054).
            RESOURCE_LOCK pwsh-script-tests
            TIMEOUT 120)
    endif()
endif()

# THIRD_PARTY_NOTICES.txt's generator (#2753): a notice for each dependency in Dependencies.cmake, and a
# FetchContent_Declare() in a shape it can't read fails the configure rather than being left out.
add_test(NAME ThirdPartyNoticesScript
    COMMAND ${CMAKE_COMMAND} -DTS_SOURCE_DIR=${CMAKE_SOURCE_DIR} -DTS_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/third-party-notices-real
            -DTS_CASE=real -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdPartyNoticesCheck.cmake)
add_test(NAME ThirdPartyNoticesUnparsedDeclareScript
    COMMAND ${CMAKE_COMMAND} -DTS_SOURCE_DIR=${CMAKE_SOURCE_DIR} -DTS_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/third-party-notices-unparsed
            -DTS_CASE=unparsed -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdPartyNoticesCheck.cmake)
# CMake wraps the message, so any run of spaces and line breaks may separate the words.
set_tests_properties(ThirdPartyNoticesUnparsedDeclareScript PROPERTIES
    PASS_REGULAR_EXPRESSION "FetchContent_Declare\\(\\)[ \r\n]+calls,[ \r\n]+but[ \r\n]+only")

# tools/bench.sh against a stub benchmark binary (#1423, #1424), wherever bash exists. On Windows
# only Git for Windows' bash: a bare `bash` there can be WSL's, which cannot run this checkout.
if(WIN32)
    find_program(TASKSMACK_BASH NAMES bash
        HINTS "$ENV{ProgramFiles}/Git/bin" "$ENV{ProgramW6432}/Git/bin" NO_DEFAULT_PATH)
else()
    find_program(TASKSMACK_BASH NAMES bash)
endif()
if(TASKSMACK_BASH)
    add_test(NAME BenchShScript
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_bench_sh.py)
    # About 25 s idle, but over twenty bench.sh runs, each starting bash, Python and git: on a
    # Windows machine busy with other builds it passed 180 s (#1797). 600 s is headroom for that,
    # not the expected time.
    set_tests_properties(BenchShScript PROPERTIES ENVIRONMENT "TASKSMACK_TEST_BASH=${TASKSMACK_BASH}" TIMEOUT 600)
endif()

# tools/install-local.sh writes its binary path as one escaped desktop-entry Exec argument (#2342).
# The installer is Linux-only, so the test is too.
if(TASKSMACK_BASH AND NOT WIN32)
    add_test(NAME InstallLocalShScript
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_install_local_sh.py)
    set_tests_properties(InstallLocalShScript PROPERTIES ENVIRONMENT "TASKSMACK_TEST_BASH=${TASKSMACK_BASH}" TIMEOUT 120)
endif()

# tools/setup-dev.sh stops before rewriting a Windows .venv in a checkout shared with WSL (#2351).
# Sourced with a stub python3.14, so it installs nothing and runs under Git for Windows' bash too.
if(TASKSMACK_BASH)
    add_test(NAME SetupDevShScript
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_setup_dev_sh.py)
    set_tests_properties(SetupDevShScript PROPERTIES ENVIRONMENT "TASKSMACK_TEST_BASH=${TASKSMACK_BASH}" TIMEOUT 120)
endif()

# tools/common.sh's find_python uses a Windows .venv only from a Windows bash, never in WSL (#2411).
# OSTYPE is set per case, so either kind of bash runs every case.
if(TASKSMACK_BASH)
    add_test(NAME CommonShScript
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_common_sh.py)
    set_tests_properties(CommonShScript PROPERTIES ENVIRONMENT "TASKSMACK_TEST_BASH=${TASKSMACK_BASH}" TIMEOUT 120)
endif()

# tools/check-prereqs.sh reads a two-component ccache version, and an unreadable or missing ccache
# (optional) never ends the check before its summary (#2350). The checker is Linux-only, so the
# test is too.
if(TASKSMACK_BASH AND NOT WIN32)
    add_test(NAME CheckPrereqsShScript
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_prereqs_sh.py)
    set_tests_properties(CheckPrereqsShScript PROPERTIES ENVIRONMENT "TASKSMACK_TEST_BASH=${TASKSMACK_BASH}" TIMEOUT 120)
endif()

# tools/iwyu.sh names a translation unit IWYU crashed on, and fails outside --report, instead of
# counting it clean (#2666). Stub IWYU binaries; the script and its CI job are Linux-only, so the test is too.
if(TASKSMACK_BASH AND NOT WIN32)
    add_test(NAME IwyuShScript
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_iwyu_sh.py)
    set_tests_properties(IwyuShScript PROPERTIES ENVIRONMENT "TASKSMACK_TEST_BASH=${TASKSMACK_BASH}" TIMEOUT 120)
endif()

# tools/check-benchmark-regression.py gates CI on benchmark regressions; Python3_EXECUTABLE is
# already required (GLAD generation, cmake/Dependencies.cmake).
add_test(NAME BenchmarkRegressionGateScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_benchmark_regression.py)

# tools/tidy-changed-files.py picks the PR clang-tidy jobs' translation units (#1406, #1626). The tests
# build throwaway git repositories (about 10 s idle). On a loaded Windows machine each git process is
# far slower and the run neared 180 s (#1797); 600 s keeps it clear of that and of the CI --timeout.
add_test(NAME TidyChangedFilesScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_tidy_changed_files.py)
set_tests_properties(TidyChangedFilesScript PROPERTIES TIMEOUT 600)

# tools/tidy-cache.py keys the clang-tidy jobs' per-file result cache (#1785).
add_test(NAME TidyCacheScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_tidy_cache.py)

# tools/merge-gtest-results.py folds the per-suite entries' gtest reports into CI's JUnit report (#1871).
add_test(NAME MergeGtestResultsScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_merge_gtest_results.py)

# tools/coverage-mismatches.py explains tools/coverage.sh's "functions have mismatched data" warning (#1544).
add_test(NAME CoverageMismatchesScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_coverage_mismatches.py)

# tools/check-cache-prune-triggers.py keeps cache-prune.yml's workflow_run list complete (#2410).
add_test(NAME CheckCachePruneTriggersScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_cache_prune_triggers.py)

# tools/check-cflite-paths.py keeps the ClusterFuzzLite baseline build's paths equal to PR fuzzing's (#2533).
add_test(NAME CheckCflitePathsScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_cflite_paths.py)

# tools/check-dll-loads.py keeps src/ from loading a DLL by bare name (#2530).
add_test(NAME CheckDllLoadsScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_dll_loads.py)

# tools/check-utf8.py refuses text that isn't UTF-8 or was encoded twice (#1648, #2529).
add_test(NAME CheckUtf8Script
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_utf8.py)

# src/main.cpp enables SeDebugPrivilege on Windows before the Application and its probes exist (#2128).
add_test(NAME StartupWiringScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_startup_wiring.py)

# The run: scripts of renovate.yml's dry-run choice (#2527), sanitizers.yml's job summary (#2532) and
# main-health.yml's per-job check (#2313), read from the workflows and run with bash against stubs
# (#2478). The Main Health cases also need jq on bash's PATH and are skipped without it.
if(TASKSMACK_BASH)
    add_test(NAME WorkflowRunStepsScript
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_workflow_run_steps.py)
    set_tests_properties(WorkflowRunStepsScript PROPERTIES ENVIRONMENT "TASKSMACK_TEST_BASH=${TASKSMACK_BASH}" TIMEOUT 120)
endif()

# tools/cache-prune-stale.py picks the superseded cache entries cache-prune.yml deletes (#1583, #2528).
add_test(NAME CachePruneStaleScript
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_cache_prune_stale.py)

# cmake/ReproducibleTarball.cmake makes the Linux .tar.gz byte-reproducible (#2636). It needs GNU
# tar, so the test is Linux-only, like the script's use.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME ReproducibleTarballScript
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_reproducible_tarball.py)
    set_tests_properties(ReproducibleTarballScript PROPERTIES
        # The project config CPack reads (cmake/Packaging.cmake), to check it hands the TGZ to the script.
        ENVIRONMENT "TASKSMACK_TEST_CMAKE=${CMAKE_COMMAND};TASKSMACK_TEST_CPACK_CONFIG=${CMAKE_BINARY_DIR}/CPackProjectConfig.cmake"
        TIMEOUT 120)
endif()

# The short Python script tests (under 6 s each, even with every script test running at once) get
# their own limit too, so a busy runner can't trip the CI steps' 60 s --timeout on them (#2054).
set_tests_properties(BenchmarkRegressionGateScript TidyCacheScript MergeGtestResultsScript CoverageMismatchesScript
    CheckCachePruneTriggersScript CheckDllLoadsScript CachePruneStaleScript CheckUtf8Script StartupWiringScript
    PROPERTIES TIMEOUT 120)

# Test sources. Every list is defined on every platform, so the registration check below can see
# all of them; the platform branch then picks its own.
set(COMMON_TEST_SOURCES
    test_main.cpp
    Benchmarks/test_AllocationCounter.cpp
    Core/test_Application.cpp
    Core/test_ConfigDirOverride.cpp
    Core/test_EnvUtils.cpp
    Core/test_FramebufferCapture.cpp
    Core/test_LocaleSetup.cpp
    Core/test_ApplicationEvents.cpp
    Core/test_Event.cpp
    Core/test_Layer.cpp
    Core/test_OpenGLRequirement.cpp
    Core/test_PathService.cpp
    Core/test_SaveFileDialog.cpp
    Core/test_VideoBackend.cpp
    Core/test_Window.cpp
    Core/test_WindowEventRouting.cpp
    Core/test_WindowEvents.cpp
    Core/test_WindowGeometry.cpp
    Domain/test_History.cpp
    Domain/test_SharedHistory.cpp
    Domain/test_ProcessConnections.cpp
    Domain/test_ProcessSecurity.cpp
    Domain/test_Numeric.cpp
    Domain/test_SingleLineText.cpp
    Domain/test_ProcessEnergyAttribution.cpp
    Domain/test_ProcessModel.cpp
    Domain/test_ProcessRowHistory.cpp
    Domain/test_SocketTrafficAccumulator.cpp
    Domain/test_GPUModel.cpp
    Domain/test_ProcessStatus.cpp
    Domain/test_ProcessState.cpp
    Domain/test_SystemModel.cpp
    Domain/AllocationFailureHook.cpp
    Domain/test_SystemModelAllocationFailure.cpp
    Domain/test_GPUModelAllocationFailure.cpp
    Domain/test_ProcessModelAllocationFailure.cpp
    Domain/test_StorageModel.cpp
    Domain/test_StartupModel.cpp
    Domain/test_SensorModel.cpp
    Domain/test_SystemInfoModel.cpp
    Domain/test_CrashHistory.cpp
    Domain/test_StorageModelAllocationFailure.cpp
    Domain/test_ServiceModel.cpp
    Domain/test_ProcessServices.cpp
    Domain/test_BackgroundSampler.cpp
    Domain/test_PriorityConfig.cpp
    Domain/test_SamplingConfig.cpp
    Domain/test_HistorySeries.cpp
    Domain/test_LruMap.cpp
    Domain/test_ProcessIconCache.cpp
    # Linux desktop-entry icon lookup (#1800): pure std::filesystem, run on every platform
    Platform/test_DesktopEntryIcons.cpp
    UI/test_ChartWidgets.cpp
    UI/test_ChartGridLayout.cpp
    UI/test_HistoryPlotHeight.cpp
    UI/test_FillPlotLayout.cpp
    UI/test_ChartGeometryBudget.cpp
    UI/test_NowBarsRender.cpp
    # ImPlot lines sample ImGui's baked line texture (#2744)
    UI/test_PlotLineTexture.cpp
    UI/test_ChartWidgetsEdgeCases.cpp
    UI/test_HistoryCsv.cpp
    UI/test_HistoryChartCsvRender.cpp
    UI/test_EmptyState.cpp
    UI/test_Format.cpp
    UI/test_SourceEncoding.cpp
    UI/test_RenderMetrics.cpp
    UI/test_RenderMetricsOverlay.cpp
    UI/test_RateAxis.cpp
    UI/test_InlineText.cpp
    UI/test_CellText.cpp
    UI/test_BundledThemes.cpp
    UI/test_ColorContrast.cpp
    UI/test_ColorDifference.cpp
    UI/test_StyleScale.cpp
    UI/test_DialogMetrics.cpp
    UI/test_ChromeLayout.cpp
    UI/test_ChromeWidgets.cpp
    UI/test_VerticalBar.cpp  # drawVerticalBarWithValue's text and tooltip (#1891)
    UI/test_LineLayout.cpp
    UI/test_SeriesLabelAlignment.cpp
    UI/test_IconEncoding.cpp
    App/test_AccessChoice.cpp  # Standard or Administrator: the notice decision and wording (#2728)
    App/test_DialogGeometry.cpp
    UI/test_ThemeCatalog.cpp
    UI/test_FormatProperties.cpp
    UI/test_ThemeLoader.cpp
    UI/test_AssetPath.cpp
    UI/test_ThemeHeader.cpp
    UI/test_WidgetsHeader.cpp
    UI/test_IconLoader.cpp
    UI/test_IconTextureCache.cpp
    App/test_ProcessIconView.cpp
    App/test_StartupServicesIcons.cpp
    UI/test_DpiScale.cpp
    UI/test_FontFileCache.cpp
    UI/test_TabularDigits.cpp
    App/test_ProcessColumnConfig.cpp
    App/test_ProcessSortUtils.cpp
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
    App/test_ProcessArchitectureNote.cpp
    App/test_ProcessCellWidths.cpp
    App/test_ProcessFilterCache.cpp
    App/test_ProcessDisplayFreeze.cpp
    App/test_CpuCoreGridIds.cpp
    App/test_CpuCoreKinds.cpp
    App/test_ProcessRowMeter.cpp
    App/test_ProcessRowSelection.cpp
    App/test_CpuSummaryText.cpp
    App/test_CpuDetailsText.cpp
    App/test_CpuDetailsBlockRender.cpp
    App/test_BatteryDetails.cpp
    Platform/test_CgroupContainer.cpp
    Platform/test_CgroupService.cpp
    Platform/test_ElfArchitecture.cpp
    Platform/test_ProcessArchitecture.cpp
    Platform/test_CgroupFreezerPath.cpp
    Platform/test_CpuDetails.cpp
    Platform/test_LinuxCpuDetails.cpp
    Platform/test_LinuxOsInfo.cpp
    Platform/test_NetworkdLinkJson.cpp
    Platform/test_DnsMessage.cpp
    Platform/test_LinuxDiskSmart.cpp
    Platform/test_SmbiosParser.cpp
    Platform/test_PartitionTable.cpp
    Platform/test_GraphicsInfo.cpp
    Platform/test_VulkanInfo.cpp
    Platform/test_DevicesInfo.cpp
    Platform/test_DriversInfo.cpp
    Platform/test_CrashesInfo.cpp
    Platform/test_WindowsSystemInfoReaders.cpp
    Platform/test_ProcMapsParser.cpp
    Platform/test_SmapsRollupParser.cpp
    Platform/test_ProcFdParser.cpp
    Platform/test_ProcStatusSecurityParser.cpp
    Platform/test_ProcessIdentity.cpp
    Platform/test_ProcessName.cpp
    Platform/test_ProcessEnvironment.cpp
    Platform/test_CpuAffinity.cpp
    Platform/test_AsciiTrim.cpp
    Platform/test_AsciiCase.cpp
    Platform/test_BatteryCapacity.cpp
    Platform/test_BrokerProtocol.cpp
    Platform/test_BrokerRequestMapping.cpp  # what either helper does with each request (#2517)
    Platform/test_BrokeredProcessActions.cpp
    Platform/test_BrokeredSystemInfoProbe.cpp
    Platform/test_ThreadName.cpp
    Platform/test_CurrentProcess.cpp
    Platform/test_Factory.cpp
    App/test_FpsCounter.cpp
    App/test_Panel.cpp
    App/test_PanelPlatforms.cpp  # the crash history reads through the helper (#2416)
    App/test_ProcessesPanel.cpp
    App/test_ShellLayer.cpp
    App/test_ShellLayerRender.cpp
    App/test_TitleBarGeometry.cpp
    App/test_TitleBarViewRender.cpp
    App/test_WindowInteraction.cpp
    App/test_UserConfig.cpp
    App/test_UserConfigHelpers.cpp
    App/test_UserConfigPersistence.cpp
    App/test_NonAsciiRoundTrips.cpp
    App/test_InstanceLock.cpp
    App/test_PanelTabs.cpp
    App/test_MainTabBarViewRender.cpp
    App/test_TabLabel.cpp
    App/test_PriorityHelpers.cpp
    App/test_ProcessGpuHelpers.cpp
    App/test_ProcessResourceHelpers.cpp
    App/test_ProcessHistoryHelpers.cpp
    App/test_ProcessDetailsHistory.cpp
    App/test_ProcessDetailsChartHelpers.cpp
    App/test_ProcessDetailsChartsRender.cpp
    App/test_ProcessSmoothedUsage.cpp
    App/test_ProcessActionHelpers.cpp
    App/test_ProcessActionsView.cpp
    App/test_AuthorizeWording.cpp
    App/test_ProcessDumpView.cpp
    App/test_ProcessActionConfirmPopup.cpp
    App/test_ProcessActionsBlockRender.cpp
    App/test_ProcessPriorityView.cpp
    App/test_ProcessPriorityViewRender.cpp
    App/test_ProcessIoPriorityView.cpp
    App/test_ProcessIoPriorityViewRender.cpp
    App/test_ProcessEnvironmentMasking.cpp
    App/test_ProcessEnvironmentView.cpp
    App/test_ProcessEnvironmentViewRender.cpp
    App/test_ProcessConnectionsView.cpp
    App/test_ProcessConnectionsViewRender.cpp
    App/test_ProcessModulesViewRender.cpp
    App/test_ProcessCrashHistoryView.cpp
    App/test_ProcessOpenFilesViewRender.cpp
    App/test_BackgroundTask.cpp
    App/test_LazyBackgroundRead.cpp
    App/test_ProcessSecurityViewRender.cpp
    App/test_ServiceActionsView.cpp
    App/test_ServicesViewRender.cpp
    App/test_StartupViewRender.cpp
    App/test_StartupActionsView.cpp
    App/test_SystemInfoView.cpp
    App/test_SystemInfoPanelRender.cpp
    App/test_SamplingGate.cpp
    App/test_SettingsLayerDetail.cpp
    App/test_SettingsDialogRender.cpp
    App/test_SettingsLayerRender.cpp
    App/test_NetInterfaceUtils.cpp
    App/test_PowerStatusText.cpp
    App/test_ElevationNoticeText.cpp
    App/test_StatusBarText.cpp
    App/test_StatusBarViewRender.cpp
    App/test_FontSizeChange.cpp
    App/test_KeyboardShortcuts.cpp
    App/test_ProcessTableNavigation.cpp
    App/test_ProcessSelection.cpp
    # The PGO UI training plan (#880): workload weights, frame allocation, the driver's command line
    Training/test_UiTrainingPlan.cpp
    App/test_ProcessBatchAction.cpp
    App/test_ProcessBatchRun.cpp
    App/test_ProcessActionRun.cpp
    App/test_ProcessBatchPriorityDialogRender.cpp
    App/test_ProcessAffinityDialog.cpp
    App/test_AboutDialogRender.cpp
    App/test_ElevationNoticeDialogRender.cpp
    App/test_HelpContent.cpp
    App/test_HelpWindowRender.cpp
    App/test_KeyboardInputRender.cpp
    App/test_MainTabSelectionRender.cpp
    App/test_EnvHooks.cpp
    App/test_DemoOverride.cpp
    App/test_ScreenshotOverride.cpp
    App/test_SelectOverride.cpp
    App/test_SyntheticScenario.cpp
    App/test_SystemSectionsRender.cpp
    App/test_GpuSectionRender.cpp
    App/test_SystemMetricsPanelRender.cpp
    App/test_ProcessDetailsPanelRender.cpp
    App/test_ProcessesPanelRender.cpp
    App/test_ServicesStartupPanelsRender.cpp
    App/test_WindowOverride.cpp
    Platform/test_SyntheticWorkload.cpp
)
# The Windows probes' pure *Math.h helpers include no Windows header, so their tests are built on
# every platform: Linux CI runs them under its sanitizers and coverage too (#1133). Tests that need
# a probe, Windows types or fakes of Windows APIs stay in WINDOWS_PLATFORM_TEST_SOURCES.
set(WINDOWS_MATH_TEST_SOURCES
    Platform/WindowsMath/test_AdapterPerfDataMath.cpp  # D3DKMT adapter perf data as GPU sensors (#2586)
    Platform/WindowsMath/test_DXGIGPUProbeMath.cpp
    Platform/WindowsMath/test_WindowsDiskProbeMath.cpp
    Platform/WindowsMath/test_WindowsEtwNetMath.cpp
    Platform/WindowsMath/test_WindowsGPUProbeMath.cpp
    Platform/WindowsMath/test_WindowsPathProviderMath.cpp
    Platform/WindowsMath/test_WindowsPowerProbeMath.cpp
    Platform/WindowsMath/test_WindowsEnergyMeterMath.cpp  # EMI metadata and measurement blocks, package channel names (#2587)
    Platform/WindowsMath/test_WindowsProcessProbeMath.cpp
    Platform/WindowsMath/test_WindowsStartupProbeMath.cpp
    Platform/WindowsMath/test_WindowsOsInfoMath.cpp
    Platform/WindowsMath/test_WindowsStorageSmartMath.cpp  # ATA SMART pages, thresholds and return status as AtaHealth (#2588)
    Platform/WindowsMath/test_WindowsServiceActionsMath.cpp
    Platform/WindowsMath/test_WindowsServiceProbeMath.cpp
    Platform/WindowsMath/test_WindowsSystemProbeMath.cpp
    Platform/WindowsMath/test_WindowsProcessIconMath.cpp
    Platform/WindowsMath/test_WindowsPackageLogoMath.cpp
    Platform/WindowsMath/test_WindowsAffinityMath.cpp
    Platform/WindowsMath/test_WindowsConversions.cpp
    Platform/WindowsMath/test_WindowsProcessDumpMath.cpp
    Platform/WindowsMath/test_WindowsProcessMachineMath.cpp
)
list(APPEND COMMON_TEST_SOURCES ${WINDOWS_MATH_TEST_SOURCES})
set(WINDOWS_PLATFORM_TEST_SOURCES
    Platform/test_ProcessProbeContract.cpp
    Platform/test_SystemProbeContract.cpp
    Platform/test_ProcessActionsContract.cpp
    Platform/test_PathProviderContract.cpp
    Platform/test_PowerProbeContract.cpp
    Platform/test_WindowsProcessProbe.cpp
    Platform/test_WindowsProcessProbeSeams.cpp
    Platform/test_WindowsEtwNetCollector.cpp
    Platform/test_WindowsSystemProbe.cpp
    Platform/test_WindowsSystemProbeSeams.cpp
    Platform/test_WindowsSystemProbeNetwork.cpp
    Platform/test_WindowsProcessActions.cpp
    Platform/test_WindowsProcessActionsSeams.cpp
    Platform/test_WindowsDebugPrivilege.cpp
    Platform/test_WindowsElevation.cpp
    Platform/test_BrokerPipe.cpp  # the privileged broker's named-pipe transport (#2311)
    Platform/test_BrokerHelper.cpp  # TaskSmackHelper.exe: its parent checks, its session and the exe itself (#2517)
    Platform/test_HelperLauncher.cpp  # starting the helper with ShellExecuteExW("runas"), through fakes (#2549)
    Platform/test_WindowsProcessIdentity.cpp
    Platform/test_WindowsProcessConnections.cpp
    Platform/test_WindowsProcessEnvironmentReader.cpp  # the environment block out of a fake process's PEB, and this process's own (#2569)
    Platform/test_WindowsProcessIconSource.cpp
    Platform/test_WindowsPackageLogo.cpp
    Platform/test_WindowsProcessModules.cpp
    Platform/test_WindowsProcessOpenFiles.cpp
    Platform/test_WindowsProcessSecurity.cpp
    Platform/test_WindowsDiskProbe.cpp
    Platform/test_WindowsDiskProbeSeams.cpp
    Platform/test_PerfProviderApartment.cpp
    Platform/test_WindowsPathProvider.cpp
    Platform/test_WindowsBatteryInfo.cpp
    Platform/test_WindowsPowerProbe.cpp
    Platform/test_WindowsEnergyMeter.cpp  # the EMI package energy through fake device calls, and the probe's reading (#2587)
    Platform/test_WindowsNVMLGPUProbe.cpp
    Platform/test_WindowsNVMLLoader.cpp
    Platform/test_WindowsPDHGPUProbe.cpp
    Platform/test_WindowsGPUProbe.cpp
    Platform/test_WindowsD3DKMTAdapterPerfData.cpp  # the kernel adapter's perf data through fake D3DKMT calls (#2586)
    Platform/test_WindowsDXGIGPUProbe.cpp
    Platform/test_WindowsDXGIGPUProbeDXCore.cpp
    Platform/test_WindowsGPURescan.cpp
    Platform/test_WindowsDriverNodes.cpp
    Platform/test_WindowsProcAddress.cpp
    Platform/test_WindowsStartupProbe.cpp
    Platform/test_WindowsStartupProbeSeams.cpp
    Platform/test_WindowsStartupActions.cpp
    Platform/test_WindowsSystemInfoProbe.cpp
    Platform/test_WindowsSystemInfoProbeSeams.cpp
    Platform/test_WindowsSystemInfoLiveReaders.cpp
    Platform/test_WindowsServiceActions.cpp
    Platform/test_WindowsServiceProbe.cpp
    Platform/test_WindowsServiceConfig.cpp
    Platform/test_WindowsHandles.cpp
    Platform/test_WindowsNetworkAdapters.cpp
    Platform/test_WindowsPublicIp.cpp
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
    Platform/test_LinuxSystemInfoProbe.cpp
    Platform/test_Nl80211.cpp  # Wi-Fi rates and standard from nl80211 (#2591)
    Platform/test_LinuxPublicIp.cpp
    Platform/test_NetworkTextProperties.cpp
    Platform/test_LinuxSystemProbe.cpp
    Core/test_RuntimeDir.cpp
    Platform/test_LinuxProcessActions.cpp
    Platform/test_BrokerServer.cpp  # the privileged helper's server and tasksmack-helper itself (#2307)
    Platform/test_SyscallTrace.cpp
    Platform/test_LinuxCoreDump.cpp  # "Create dump file" on Linux: names, gcore command, refusals (#807)
    Platform/test_DetachedSpawn.cpp
    Platform/test_SessionUser.cpp  # whom a run as root opens links and folders as (#2461)
    Platform/test_LinuxStartup.cpp  # the Startup tab on Linux: XDG autostart (#2541)
    Platform/test_LinuxIoPriority.cpp
    Platform/test_LinuxProcessEnvironmentReader.cpp
    Platform/test_LinuxProcessConnectionsReader.cpp
    Platform/test_LinuxProcessModulesReader.cpp
    Platform/test_LinuxProcessOpenFilesReader.cpp
    Platform/test_LinuxProcessSecurityReader.cpp
    Platform/test_SystemdBus.cpp
    Platform/test_SystemdBusFake.cpp
    Platform/test_SystemdServices.cpp
    Platform/test_SystemdActions.cpp  # systemd service control (#2006 phase 2)
    Platform/test_ProcParsing.cpp  # the file readers and parseDouble (#1891)
    Platform/test_SystemdJournal.cpp
    Platform/test_InetSocketTable.cpp
    Platform/test_LinuxProcessStatus.cpp
    Platform/test_LinuxDiskProbe.cpp
    Platform/test_LinuxPathProvider.cpp
    Platform/test_LinuxPowerProbe.cpp
    Platform/test_LinuxGPUProbe.cpp
    Platform/test_LinuxNVMLGPUProbe.cpp
    Platform/test_LinuxROCmGPUProbe.cpp
    Platform/test_NetlinkSocketStats.cpp
    Platform/test_DRMGPUProbe.cpp
    Platform/test_AccelNpuProbe.cpp
    Platform/test_LinuxProcessIconSource.cpp
    Core/test_X11ConnectionLoss.cpp  # a broken X connection exits without static destructors (#2657)
)
set(LINUX_INTEGRATION_TEST_SOURCES
    Integration/test_CrossLayer.cpp
    Integration/test_LinuxRealProbes.cpp
)

# Platform-specific test sources
if(WIN32)
    set(PLATFORM_TEST_SOURCES ${WINDOWS_PLATFORM_TEST_SOURCES})
    set(INTEGRATION_TEST_SOURCES ${WINDOWS_INTEGRATION_TEST_SOURCES})
    # The platform sources come from the app's own list (root CMakeLists.txt), so a new platform .cpp is
    # added in one place (#1702). Only the list is shared: the tests still compile their own selection
    # of sources (Mocks/ThemeStub.cpp for UI/Theme.cpp, no panels or layers), so they can't link
    # TaskSmackApp the way the benchmarks do (#1703).
    list(TRANSFORM PLATFORM_SOURCES_WINDOWS PREPEND "${CMAKE_SOURCE_DIR}/" OUTPUT_VARIABLE PLATFORM_SRC_UNDER_TEST)
else()
    set(PLATFORM_TEST_SOURCES ${LINUX_PLATFORM_TEST_SOURCES})
    set(INTEGRATION_TEST_SOURCES ${LINUX_INTEGRATION_TEST_SOURCES})
    list(TRANSFORM PLATFORM_SOURCES_LINUX PREPEND "${CMAKE_SOURCE_DIR}/" OUTPUT_VARIABLE PLATFORM_SRC_UNDER_TEST)
endif()

# Every tests/**/test_*.cpp must be in one of the lists above; otherwise it is never built and its
# tests silently never run, as test_Numeric.cpp wasn't for months (#1135). Compared against the
# lists themselves, not the text of this file, so a mention in a comment doesn't count.
# The real UI::Theme's tests, built into TaskSmackThemeTests below rather than TaskSmackTests (#1547)
set(THEME_REAL_TEST_SOURCES
    UI/test_ThemeReal.cpp
    UI/test_FontLoading.cpp
)
# Tests that need the whole app, built into TaskSmackAppTests below (#1395)
set(APP_REAL_TEST_SOURCES
    App/test_UiLayersOnDisplay.cpp
    App/test_ScreenshotOnDisplay.cpp  # TASKSMACK_SCREENSHOT and TASKSMACK_DEMO end to end (#2827)
    App/test_SettingsThemeApply.cpp  # a theme picked in Settings applies on Save, not before (#2713)
)
# Linux-only app tests, listed on every platform so the registration check below sees them
set(APP_REAL_LINUX_TEST_SOURCES
    App/test_PlatformOpen.cpp  # a fake xdg-open on PATH (#1395)
)
if(UNIX AND NOT APPLE)
    list(APPEND APP_REAL_TEST_SOURCES ${APP_REAL_LINUX_TEST_SOURCES})
endif()
set(TASKSMACK_REGISTERED_TEST_SOURCES
    ${COMMON_TEST_SOURCES}
    ${WINDOWS_PLATFORM_TEST_SOURCES}
    ${WINDOWS_INTEGRATION_TEST_SOURCES}
    ${LINUX_PLATFORM_TEST_SOURCES}
    ${LINUX_INTEGRATION_TEST_SOURCES}
    ${THEME_REAL_TEST_SOURCES}
    ${APP_REAL_TEST_SOURCES}
    ${APP_REAL_LINUX_TEST_SOURCES}
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
    ${CMAKE_SOURCE_DIR}/src/Core/ConfigDirOverride.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/LocaleSetup.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/PathService.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/VideoBackend.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/Window.cpp
    # The screenshot hook's framebuffer read and PNG writer (#2827): test_FramebufferCapture.cpp runs
    # the row flip and writePng() without a GL context
    ${CMAKE_SOURCE_DIR}/src/Core/FramebufferCapture.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/SaveFileDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/X11ConnectionLoss.cpp
    ${CMAKE_SOURCE_DIR}/src/Core/X11WindowManager.cpp
    # App layer
    ${CMAKE_SOURCE_DIR}/src/App/UserConfig.cpp
    ${CMAKE_SOURCE_DIR}/src/App/InstanceLock.cpp
    # Synthetic scenario (#1413): the env-var parser, composition helpers and history preload
    ${CMAKE_SOURCE_DIR}/src/App/SyntheticScenario.cpp
    # Startup-selection test hook (#1559): the platform name comparisons test_SelectOverride.cpp exercises
    ${CMAKE_SOURCE_DIR}/src/App/SelectOverride.cpp
    # The process-action confirm modal, the Overview's Actions block, the priority controls, the chart tabs and
    # the Environment and Connections sections, run headless (test_ProcessActionConfirmPopup.cpp,
    # test_ProcessPriorityViewRender.cpp, test_ProcessIoPriorityViewRender.cpp,
    # test_ProcessDetailsChartsRender.cpp, test_ProcessEnvironmentViewRender.cpp,
    # test_ProcessConnectionsViewRender.cpp), the Modules section (test_ProcessModulesViewRender.cpp, #802),
    # the Open files section (test_ProcessOpenFilesViewRender.cpp, #183), the Security section
    # (test_ProcessSecurityViewRender.cpp, #1526), and the batch priority dialog (#1484,
    # test_ProcessBatchPriorityDialogRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessActionConfirm.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessBatchPriorityDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessAffinityDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessActionsView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessDumpView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessActionsBlock.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessPriorityView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessIoPriorityView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessDetailsCharts.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessEnvironmentView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessConnectionsView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessModulesView.cpp
    # Process Details' Recent crashes line (#1675), run headless (test_ProcessCrashHistoryView.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessCrashHistoryView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessOpenFilesView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessSecurityView.cpp
    # The Overview's CPU Details block, run headless (test_CpuDetailsBlockRender.cpp, #809)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/CpuDetailsBlock.cpp
    # The System Metrics tab's Memory & Swap, CPU Cores, Disk I/O and Network sections and the render-metrics
    # overlay, run headless (test_SystemSectionsRender.cpp, test_RenderMetricsOverlay.cpp, #1395)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/MemorySection.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/CpuCoresSection.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/StorageSection.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/NetworkSection.cpp
    # The GPU tab, run headless (test_GpuSectionRender.cpp, #1545)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/GpuSection.cpp
    # The System Metrics panel as a whole, with mock probes through its seam (test_SystemMetricsPanelRender.cpp, #1546)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/SystemMetricsPanel.cpp
    # The Process Details panel as a whole, with mock actions and readers (test_ProcessDetailsPanelRender.cpp, #1546)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessDetailsPanel.cpp
    # The Processes panel as a whole, with a mock probe through its seam (test_ProcessesPanelRender.cpp, #1546)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessesPanel.cpp
    # The icon beside each process name (#1800): the panels draw it; test_ProcessIconView.cpp runs it
    # headless with a fake texture backend
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ProcessIconView.cpp
    ${CMAKE_SOURCE_DIR}/src/UI/IconTextureCache.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/ProcessIconCache.cpp
    ${CMAKE_SOURCE_DIR}/src/UI/RenderMetrics.cpp
    # The Services tab's table (#800), run headless (test_ServicesViewRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ServiceActionsView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ServicesView.cpp
    # The Startup tab's table (#801), run headless (test_StartupViewRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/StartupView.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/StartupActionsView.cpp
    # The Services and Startup panels as a whole, with fake probes and actions through their seam
    # (test_ServicesStartupPanelsRender.cpp, #1721)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/ServicesPanel.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/StartupPanel.cpp
    # The System Information page (#1399), run headless (test_SystemInfoView.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/Panels/SystemInfoSections.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/SystemInfoView.cpp
    # The keyboard shortcuts' ImGui adapter (#160, #170), run headless (test_KeyboardInputRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/KeyboardInput.cpp
    # The main tab bar (#1547), run headless (test_MainTabBarViewRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/MainTabBarView.cpp
    # The status bar (#1547), run headless (test_StatusBarViewRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/StatusBarView.cpp
    # The shell's frame (#1547): the main window, tab bar and status bar over the panels, drawn
    # headless with a fake host (test_ShellLayerRender.cpp), and what it pulls in
    ${CMAKE_SOURCE_DIR}/src/App/ShellLayer.cpp
    ${CMAKE_SOURCE_DIR}/src/App/ShellPanels.cpp
    ${CMAKE_SOURCE_DIR}/src/App/PanelPlatforms.cpp
    ${CMAKE_SOURCE_DIR}/src/App/NativeWindow.cpp
    ${CMAKE_SOURCE_DIR}/src/App/Panels/SystemInfoPanel.cpp
    ${CMAKE_SOURCE_DIR}/src/App/WindowOverride.cpp
    # The custom title bar (#1547), run headless (test_TitleBarViewRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/TitleBarView.cpp
    # The Settings dialog (#1547), run headless (test_SettingsDialogRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/SettingsDialog.cpp
    # The Settings layer around it (#1395), run headless through its seam (test_SettingsLayerRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/SettingsLayer.cpp
    # The About dialog (#1490), run headless (test_AboutDialogRender.cpp), the layer that opens it on
    # Core::OpenAboutEvent (#1600), and the link opener they call
    ${CMAKE_SOURCE_DIR}/src/App/AboutDialog.cpp
    ${CMAKE_SOURCE_DIR}/src/App/AboutLayer.cpp
    ${CMAKE_SOURCE_DIR}/src/App/PlatformOpen.cpp
    # The Limited Data notice (#1547), run headless (test_ElevationNoticeDialogRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/ElevationNoticeDialog.cpp
    # The Help window (#172), run headless (test_HelpWindowRender.cpp)
    ${CMAKE_SOURCE_DIR}/src/App/HelpWindow.cpp
    # The PGO UI training plan (#880), pure: test_UiTrainingPlan.cpp
    ${CMAKE_SOURCE_DIR}/src/Training/UiTrainingPlan.cpp
    # Domain layer
    ${CMAKE_SOURCE_DIR}/src/Domain/ProcessModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/ProcessRowHistory.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/GPUModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/StorageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/StartupModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SensorModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SensorTracker.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SystemInfoModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/CrashHistory.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/ServiceModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/BackgroundSampler.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/ThreadName.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/ThreadApartment.cpp # the sampler thread's COM apartment (#2731)
    ${CMAKE_SOURCE_DIR}/src/Platform/BrokeredProcessActions.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/BrokeredSystemInfoProbe.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/BrokerRequestMapping.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/CurrentProcess.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/Synthetic/SyntheticProbes.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/Synthetic/SyntheticWorkload.cpp
    # UI source files under test (Note: Theme.cpp excluded - requires ImGui/ImPlot runtime context)
    ${CMAKE_SOURCE_DIR}/src/UI/ThemeLoader.cpp
    ${CMAKE_SOURCE_DIR}/src/UI/AssetPath.cpp
    # IconLoader.cpp only needs imgui.h for type declarations (no ImGui:: calls) and its GL
    # calls resolve against glad_gl_core_33 (linked below) - unlike Theme.cpp it doesn't need
    # a live ImGui/GL context to link, only to actually load a texture successfully (#771).
    ${CMAKE_SOURCE_DIR}/src/UI/IconLoader.cpp
    # stb_image's implementation, which IconLoader.cpp and the Linux process icons call (#1800)
    ${CMAKE_SOURCE_DIR}/src/Platform/StbImage.cpp
    # Icons held as image files: desktop entries' PNGs, packaged apps' logos (#1800)
    ${CMAKE_SOURCE_DIR}/src/Platform/IconImageFile.cpp
    # Marker glyphs for the value strip and series, drawn by test_FillPlotLayout.cpp and
    # test_ChartGeometryBudget.cpp under a live ImGui context
    ${CMAKE_SOURCE_DIR}/src/UI/ChartLegend.cpp
    # A table cell's text, ellipsized with its full text as a tooltip when clipped (#914, #1920):
    # every table draws its cells with it; test_CellText.cpp runs it headless
    ${CMAKE_SOURCE_DIR}/src/UI/CellText.cpp
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
    rapidcheck_gtest  # property-based tests (#139)
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
    # tasksmack_sdbus: libsystemd and TASKSMACK_HAS_SDBUS when found, as for the app (#1525). No X11:
    # Core/X11WindowManager.cpp dlopen()s libX11, as for the app and the benchmarks (#1702).
    target_link_libraries(TaskSmackTests PRIVATE ${CMAKE_DL_LIBS} tasksmack_sdbus)

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
    tasksmack_apply_text_encoding(TaskSmackNVMLMock)

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
    tasksmack_apply_text_encoding(TaskSmackROCmMock)

    add_dependencies(TaskSmackTests TaskSmackNVMLMock TaskSmackROCmMock)
    target_compile_definitions(TaskSmackTests PRIVATE
        TASKSMACK_TEST_GPU_MOCK_DIR="${CMAKE_CURRENT_BINARY_DIR}/mocks"
    )
endif()

# test_BrokerProtocol.cpp checks fuzz_broker_protocol's seeds: one per message kind, each decoding (#2643).
target_compile_definitions(TaskSmackTests PRIVATE
    TASKSMACK_BROKER_FUZZ_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/fuzz/corpus/fuzz_broker_protocol"
)

# test_BrokerServer.cpp runs the privileged helper binary directly over a socketpair (#2307).
if(TARGET tasksmack-helper)
    add_dependencies(TaskSmackTests tasksmack-helper)
    target_compile_definitions(TaskSmackTests PRIVATE TASKSMACK_HELPER_PATH="$<TARGET_FILE:tasksmack-helper>")
endif()

# test_BrokerHelper.cpp runs TaskSmackHelper.exe directly, unelevated, as this user (#2517). The helper
# serves only the TaskSmack.exe beside it that started it, from an install its user can't change (#2549).
# The tests copy the helper and TaskSmackHelperTestParent.exe, as TaskSmack.exe, into a folder of their
# own, which that starts the helper from through HelperLauncher, with CreateProcessW in place of "runas",
# just as a program trying to use the helper on its own would: the helper must refuse it. Nothing is
# elevated, no prompt raised.
if(TARGET TaskSmackHelper)
    add_executable(TaskSmackHelperTestParent
        Platform/HelperTestParent.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/HelperLauncher.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/PipeBroker.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/BrokerHelper.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/BrokerPipe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/BrokerPipeServer.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsElevation.cpp # HelperLauncher::availability()
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsProcessIdentity.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WinString.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/ThreadName.cpp # PipeBroker names its launch thread (#2768)
    )
    target_compile_features(TaskSmackHelperTestParent PRIVATE cxx_std_23)
    tasksmack_apply_default_warnings(TaskSmackHelperTestParent)
    tasksmack_apply_text_encoding(TaskSmackHelperTestParent)
    tasksmack_apply_windows_platform(TaskSmackHelperTestParent)
    target_include_directories(TaskSmackHelperTestParent PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(TaskSmackHelperTestParent PRIVATE spdlog::spdlog_header_only)

    add_dependencies(TaskSmackTests TaskSmackHelper TaskSmackHelperTestParent)
    target_compile_definitions(TaskSmackTests PRIVATE
        TASKSMACK_WINDOWS_HELPER_PATH="$<TARGET_FILE:TaskSmackHelper>"
        TASKSMACK_WINDOWS_HELPER_TEST_PARENT_PATH="$<TARGET_FILE:TaskSmackHelperTestParent>"
        # Whether the helper is built with CFG and CETCOMPAT, which the tests then check it has (#2557)
        $<$<BOOL:${TASKSMACK_HELPER_HARDENED}>:TASKSMACK_WINDOWS_HELPER_HARDENED>
    )
endif()

# test_BundledThemes.cpp checks the shipped theme files themselves, from the source tree.
# test_ElevationNoticeDialogRender.cpp lays the notice out in the bundled Inter at each font preset (#1986).
target_compile_definitions(TaskSmackTests PRIVATE TASKSMACK_SOURCE_THEMES_DIR="${CMAKE_SOURCE_DIR}/assets/themes"
                                                  TASKSMACK_SOURCE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
# test_UserConfigPersistence.cpp loads a config.toml saved by the released v0.8.0 (#2725).
target_compile_definitions(TaskSmackTests PRIVATE TASKSMACK_APP_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/App/fixtures")
# The app's assets beside the test binaries, as the app has them beside itself (CMakeLists.txt): the
# real-display tests then run UILayer with the app's fonts, title font, chrome icon font and window icon
# instead of ImGui's built-in font and no icon ("Failed to load icon ... tests/assets/icons"), so the title
# bar is drawn and pressed as users see it (#1954).
add_custom_command(TARGET TaskSmackTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:TaskSmackTests>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/icons" "$<TARGET_FILE_DIR:TaskSmackTests>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/themes" "$<TARGET_FILE_DIR:TaskSmackTests>/assets/themes"
    COMMENT "Copying assets (fonts, icons, themes) beside the test binaries"
)

if(WIN32)
    # The same Windows definitions and system libraries as the app's targets, from the one list in
    # the root CMakeLists.txt (#1702), so a library the platform sources gain links here too.
    tasksmack_apply_windows_platform(TaskSmackTests)
    # The tests also link these, beyond the app's list; kept here so the app's link line is unchanged.
    target_link_libraries(TaskSmackTests PRIVATE
        dxguid # tests/Mocks/WindowsDXCoreFake.h: the SDK's DXCore interface and hardware-type GUIDs (#2039)
        pdh
        psapi
        userenv
    )
endif()

tasksmack_apply_default_warnings(TaskSmackTests)
tasksmack_apply_text_encoding(TaskSmackTests)

# Precompiled headers for tests (speeds up compilation)
if(TASKSMACK_ENABLE_PCH)
    include(../cmake/TestPrecompiledHeaders.cmake)
    target_precompile_headers(TaskSmackTests PRIVATE ${TASKSMACK_TEST_PCH_HEADERS})
endif()

# One ctest entry per gtest suite, not per case (#1871): see cmake/GTestSuites.cmake. Most suites take
# well under a second in one Windows debug process. ApplicationTest, about 12 s of many cases that
# each open a window, is split so it isn't a long pole for `ctest -j`. The next slowest are about 8 s
# and are one or two slow cases (SyntheticWorkloadTest.SystemBusyCoversProcessWorkWithinCapacity,
# WindowsSystemInfoProbeTest's driver reads), which shards would not split.
#
# An entry's TIMEOUT covers its whole suite, so it is set here rather than left to the CI steps'
# per-entry --timeout (60 s, 120 s under the sanitizers), which was sized for one case: a suite under
# TSan can run several times its debug time. It still ends a hung suite.
set(TASKSMACK_GTEST_PROPERTIES TIMEOUT 300)
if(NOT WIN32)
    # Prepend the mocks directory to LD_LIBRARY_PATH before the test process starts so
    # dlopen("libnvidia-ml.so.1") and dlopen("librocm_smi64.so.6") find the mock shared
    # libraries built in the mocks/ directory. The dynamic linker caches LD_LIBRARY_PATH at
    # process startup; runtime setenv() does not affect subsequent dlopen() calls, so the
    # path must be injected via CTest's ENVIRONMENT_MODIFICATION test property.
    # ENVIRONMENT_MODIFICATION prepends rather than replacing, preserving any SDL/driver/system
    # library paths already set by the developer or CI runner.
    list(APPEND TASKSMACK_GTEST_PROPERTIES
        ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:${CMAKE_CURRENT_BINARY_DIR}/mocks")
endif()
# On Windows, ImGui's clipboard is the system clipboard, which every process shares: suites that
# copy and read it back take turns, or one reads what another just wrote.
tasksmack_gtest_add_suites(TaskSmackTests
    SHARDS ApplicationTest=3
    PROPERTIES ${TASKSMACK_GTEST_PROPERTIES}
    SUITE_PROPERTIES
        HistoryChartCsvRenderTest RESOURCE_LOCK clipboard
        ProcessesPanelRenderTest RESOURCE_LOCK clipboard
        SystemInfoPanelRenderTest RESOURCE_LOCK clipboard
)

# The real UI::Theme (#1547), in its own executable: TaskSmackTests links Mocks/ThemeStub.cpp, which
# defines the same symbols, so Theme.cpp can't be linked there. Headless ImGui/ImPlot, no SDL or GL.
add_executable(TaskSmackThemeTests
    ${THEME_REAL_TEST_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/UI/Theme.cpp
    ${CMAKE_SOURCE_DIR}/src/UI/ThemeLoader.cpp
    # The app's fonts registered with the real Theme (test_FontLoading.cpp)
    ${CMAKE_SOURCE_DIR}/src/UI/FontLoading.cpp
    ${CMAKE_SOURCE_DIR}/src/UI/TabularDigits.cpp
)
target_include_directories(TaskSmackThemeTests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)  # FallbackTheme.h
target_link_libraries(TaskSmackThemeTests PRIVATE gtest gtest_main spdlog::spdlog_header_only tomlplusplus::tomlplusplus imgui_lib implot_lib
                      Freetype::Freetype) # TabularDigits.cpp
target_compile_definitions(TaskSmackThemeTests PRIVATE TASKSMACK_SOURCE_THEMES_DIR="${CMAKE_SOURCE_DIR}/assets/themes"
                                                       TASKSMACK_SOURCE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
tasksmack_apply_linux_toolchain(TaskSmackThemeTests)
tasksmack_apply_default_warnings(TaskSmackThemeTests)
tasksmack_apply_text_encoding(TaskSmackThemeTests)
tasksmack_gtest_add_suites(TaskSmackThemeTests PROPERTIES TIMEOUT 300)

# Tests that need the whole app (#1395): a real Core::Application with the real UI::Theme, UILayer and
# TitleBarLayer on a display. Built from the app's own objects (TaskSmackApp, everything but main()),
# the way the benchmarks are, so it links what the app links. tools/coverage.sh runs it too: its
# counters match the app binary's functions by name and hash, so they count for the app's files.
add_executable(TaskSmackAppTests
    App/AppTestsMain.cpp  # its own main(): a temporary TASKSMACK_CONFIG_DIR before any test runs
    ${APP_REAL_TEST_SOURCES}
    $<TARGET_OBJECTS:TaskSmackApp>
)
tasksmack_configure_app_target(TaskSmackAppTests)
target_include_directories(TaskSmackAppTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # Core/HeadlessVideoDriverTestUtils.h
target_link_libraries(TaskSmackAppTests PRIVATE gtest)
target_compile_definitions(TaskSmackAppTests PRIVATE TASKSMACK_SOURCE_THEMES_DIR="${CMAKE_SOURCE_DIR}/assets/themes"
                                                     # test_UiLayersOnDisplay.cpp's first-run golden config.toml (#2727)
                                                     TASKSMACK_APP_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/App/fixtures")
# The real-display tests are in this binary: the same assets beside it (see TaskSmackTests above), so
# building this target alone still runs them with the app's fonts and icon (#1954).
add_custom_command(TARGET TaskSmackAppTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:TaskSmackAppTests>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/icons" "$<TARGET_FILE_DIR:TaskSmackAppTests>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/themes" "$<TARGET_FILE_DIR:TaskSmackAppTests>/assets/themes"
    COMMENT "Copying assets (fonts, icons, themes) beside TaskSmackAppTests"
)
tasksmack_gtest_add_suites(TaskSmackAppTests)

# The headless PGO UI training driver (#880) runs its whole plan for a few frames and exits 0: every
# main tab drawn by the real panels, with no window, once on the real probes and once on a small
# synthetic machine. It fails if a workload never brought its tab forward. No PGO toolchain needed.
if(TARGET TaskSmackUiTraining)
    add_test(NAME UiTrainingSmokeReal
        COMMAND $<TARGET_FILE:TaskSmackUiTraining> --probes real --frames 100 --fps 0)
    add_test(NAME UiTrainingSmokeSynthetic
        COMMAND $<TARGET_FILE:TaskSmackUiTraining> --synthetic processes=300,history=60 --frames 100 --fps 0)
    set_tests_properties(UiTrainingSmokeReal UiTrainingSmokeSynthetic PROPERTIES TIMEOUT 180 LABELS "ui-training")
endif()
