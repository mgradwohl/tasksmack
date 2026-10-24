# Minimum CMake version rationale:
#   3.25: FetchContent_Declare(SYSTEM) keyword (suppresses warnings from third-party headers)
#   3.25: CMakePresets.json schema version 6 (workflowPresets, include directives)
#   3.24: FETCHCONTENT_TRY_FIND_PACKAGE_MODE variable
#   3.29: Declared as minimum in CMakePresets.json (cmakeMinimumRequired field)
cmake_minimum_required(VERSION 3.29)

# Enable IPO policy explicitly (avoids third-party warnings when targets set IPO properties)
if(POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
endif()

# RC-compiler pre-detection lives in its own focused module (see cmake/RCCompiler.cmake for the
# full rationale) but must be included here, before project(), so CMake's platform detection
# doesn't fail when looking for the RC compiler -- the module can't be included alongside
# Options/CompilerOptions/Dependencies.cmake below since those run after project().
include(cmake/RCCompiler.cmake)

# Initialize with both C and CXX (required by dependencies like SDL3, GLAD)
project(TaskSmack VERSION 0.9.0 LANGUAGES C CXX)

# 64-bit targets only (x64, ARM64): nothing builds, tests or ships 32-bit, and the code no longer
# carries 32-bit branches (#2214). Fail here, before any dependency is configured, rather than
# deep inside a build.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "TaskSmack builds only 64-bit targets (x64, ARM64); this toolchain targets "
                        "${CMAKE_SIZEOF_VOID_P}-byte pointers. "
                        "See https://github.com/mgradwohl/tasksmack/issues/2214.")
endif()

# Focused modules keep this root file declarative. Order matters:
#   Options          - cache options consumed by every later module
#   CompilerOptions  - toolchain, warnings, hardening, ccache, IPO, linker selection
#                      (global flags must be set before dependencies are configured)
#   Dependencies     - FetchContent orchestration and third-party target wiring
include(cmake/Options.cmake)
include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

# Needed here (not just in cmake/InstallRules.cmake, which includes it again -- harmless,
# GNUInstallDirs guards against double inclusion) so CMAKE_INSTALL_DATADIR is already set
# when version.h.in is configured below. Without this, @CMAKE_INSTALL_DATADIR@ substitutes to
# an empty string and TASKSMACK_INSTALL_DATADIR ships empty, breaking every installed-asset
# search candidate in src/UI/AssetPath.cpp that depends on it.
include(GNUInstallDirs)

# Generate version header from template
string(TOUPPER ${PROJECT_NAME} PROJECT_NAME_UPPER)
string(TOLOWER ${PROJECT_NAME} PROJECT_NAME_LOWER)

# Capture git commit (short). Fallback to "unknown" if git is unavailable.
set(GIT_COMMIT "unknown")
execute_process(
    COMMAND git rev-parse --short=8 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIT_COMMIT_RESULT
)
if(GIT_COMMIT_RESULT EQUAL 0 AND NOT GIT_COMMIT_OUTPUT STREQUAL "")
    set(GIT_COMMIT ${GIT_COMMIT_OUTPUT})
endif()

set(GIT_FULL_COMMIT "unknown")
set(GIT_SOURCE_STATE "unknown")
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_FULL_COMMIT_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIT_FULL_COMMIT_RESULT
)
execute_process(
    COMMAND git status --porcelain --untracked-files=normal
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_STATUS_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIT_STATUS_RESULT
)
if(GIT_FULL_COMMIT_RESULT EQUAL 0 AND NOT GIT_FULL_COMMIT_OUTPUT STREQUAL "")
    set(GIT_FULL_COMMIT ${GIT_FULL_COMMIT_OUTPUT})
endif()
if(GIT_STATUS_RESULT EQUAL 0)
    if(GIT_STATUS_OUTPUT STREQUAL "")
        set(GIT_SOURCE_STATE "clean")
    else()
        set(GIT_SOURCE_STATE "dirty")
        # A release build (SOURCE_DATE_EPOCH set) from a dirty tree records "dirty" in version.h, so its
        # binary differs from a rebuild of a clean checkout of the same tag. Name what made it dirty: an
        # untracked .ccache/ that CI's ccache action left in the checkout did that once.
        if(DEFINED ENV{SOURCE_DATE_EPOCH})
            message(WARNING "SOURCE_DATE_EPOCH is set but the source tree is dirty, so version.h records "
                            "\"dirty\" and this build won't match one from a clean checkout:\n${GIT_STATUS_OUTPUT}")
        endif()
    endif()
endif()

# Build date/time embedded in version.h. string(TIMESTAMP) honors the
# reproducible-builds.org SOURCE_DATE_EPOCH environment variable when it is
# set, producing an identical value for a given commit instead of true
# wall-clock time -- release builds export SOURCE_DATE_EPOCH from the tagged
# commit (see .github/workflows/release.yml) so two builds of the same tag
# embed the same timestamp. Local/dev builds leave it unset and get the
# actual configure-time timestamp, which is fine since dev builds have no
# reproducibility requirement.
string(TIMESTAMP TASKSMACK_BUILD_DATE "%b %d %Y" UTC)
string(TIMESTAMP TASKSMACK_BUILD_TIME "%H:%M:%S" UTC)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
    @ONLY
)

# The built-in fallback theme is Arctic Fire's file, embedded at configure time so the two never
# drift (#1196). Re-run configure whenever the theme file changes.
set(TASKSMACK_FALLBACK_THEME_FILE ${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/arctic-fire.toml)
file(READ ${TASKSMACK_FALLBACK_THEME_FILE} TASKSMACK_FALLBACK_THEME_TOML)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TASKSMACK_FALLBACK_THEME_FILE})
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UI/FallbackTheme.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/FallbackTheme.h
    @ONLY
)

set(TASKSMACK_SOURCES
    src/main.cpp
    src/Core/Application.cpp
    src/Core/ConfigDirOverride.cpp
    src/Core/LocaleSetup.cpp
    src/Core/PathService.cpp
    src/Core/VideoBackend.cpp
    src/Core/Window.cpp
    src/Core/FramebufferCapture.cpp
    src/Core/SaveFileDialog.cpp
    src/Core/X11ConnectionLoss.cpp
    src/Core/X11WindowManager.cpp
    src/UI/AssetPath.cpp
    src/UI/CellText.cpp
    src/UI/ChartLegend.cpp
    src/UI/TabularDigits.cpp
    src/UI/FontLoading.cpp
    src/UI/UILayer.cpp
    src/UI/RenderMetrics.cpp
    src/UI/Theme.cpp
    src/UI/ThemeLoader.cpp
    src/UI/IconLoader.cpp
    src/UI/IconTextureCache.cpp
    src/App/AboutDialog.cpp
    src/App/AboutLayer.cpp
    src/App/DemoLayer.cpp
    src/App/DemoOverride.cpp
    src/App/ElevationNoticeDialog.cpp
    src/App/ElevationNoticeLayer.cpp
    src/App/HelpLayer.cpp
    src/App/HelpWindow.cpp
    src/App/InstanceLock.cpp
    src/App/KeyboardInput.cpp
    src/App/PlatformOpen.cpp
    src/App/SettingsDialog.cpp
    src/App/SettingsLayer.cpp
    src/App/SelectOverride.cpp
    src/App/ScreenshotLayer.cpp
    src/App/ScreenshotOverride.cpp
    src/App/MainTabBarView.cpp
    src/App/ShellHost.cpp
    src/App/ShellLayer.cpp
    src/App/ShellPanels.cpp
    src/App/PanelPlatforms.cpp
    src/App/NativeWindow.cpp
    src/App/StatusBarView.cpp
    src/App/SyntheticScenario.cpp
    src/App/TitleBarLayer.cpp
    src/App/TitleBarView.cpp
    src/App/UserConfig.cpp
    src/App/WindowOverride.cpp
    src/App/Panels/ProcessesPanel.cpp
    src/App/Panels/ProcessIconView.cpp
    src/App/Panels/ProcessDetailsPanel.cpp
    src/App/Panels/ProcessDetailsCharts.cpp
    src/App/Panels/ProcessActionConfirm.cpp
    src/App/Panels/ProcessBatchPriorityDialog.cpp
    src/App/Panels/ProcessAffinityDialog.cpp
    src/App/Panels/ProcessActionsView.cpp
    src/App/Panels/ProcessDumpView.cpp
    src/App/Panels/ProcessActionsBlock.cpp
    src/App/Panels/ProcessPriorityView.cpp
    src/App/Panels/ProcessIoPriorityView.cpp
    src/App/Panels/ProcessEnvironmentView.cpp
    src/App/Panels/ProcessConnectionsView.cpp
    src/App/Panels/ProcessModulesView.cpp
    src/App/Panels/ProcessCrashHistoryView.cpp
    src/App/Panels/ProcessOpenFilesView.cpp
    src/App/Panels/ProcessSecurityView.cpp
    src/App/Panels/SystemMetricsPanel.cpp
    src/App/Panels/CpuCoresSection.cpp
    src/App/Panels/CpuDetailsBlock.cpp
    src/App/Panels/MemorySection.cpp
    src/App/Panels/NetworkSection.cpp
    src/App/Panels/StorageSection.cpp
    src/App/Panels/GpuSection.cpp
    src/App/Panels/ServicesPanel.cpp
    src/App/Panels/ServiceActionsView.cpp
    src/App/Panels/ServicesView.cpp
    src/App/Panels/StartupActionsView.cpp
    src/App/Panels/StartupPanel.cpp
    src/App/Panels/StartupView.cpp
    src/App/Panels/SystemInfoPanel.cpp
    src/App/Panels/SystemInfoSections.cpp
    src/App/Panels/SystemInfoView.cpp
    src/Domain/ProcessModel.cpp
    src/Domain/ProcessRowHistory.cpp
    src/Domain/BackgroundSampler.cpp
    src/Domain/SystemModel.cpp
    src/Domain/StorageModel.cpp
    src/Domain/GPUModel.cpp
    src/Domain/StartupModel.cpp
    src/Domain/ServiceModel.cpp
    src/Domain/SensorModel.cpp
    src/Domain/SensorTracker.cpp
    src/Domain/SystemInfoModel.cpp
    src/Domain/CrashHistory.cpp
    src/Domain/ProcessIconCache.cpp
    src/Platform/CurrentProcess.cpp
    src/Platform/ThreadName.cpp
    src/Platform/ThreadApartment.cpp         # a sampler thread's COM apartment (#2731)
    src/Platform/BrokeredProcessActions.cpp  # retries a refused action through the privileged helper (#2308)
    src/Platform/BrokeredSystemInfoProbe.cpp # System Information's root-only facts through the helper (#2310)
    src/Platform/BrokerRequestMapping.cpp    # what either OS's helper does with each request (#2517)
    # stb_image's implementation, for UI/IconLoader.cpp and the Linux process icons (#1800)
    src/Platform/StbImage.cpp
    # Icons held as image files: desktop entries' PNGs, packaged apps' logos (#1800)
    src/Platform/IconImageFile.cpp
    # Synthetic probes (#1413): constructed only when TASKSMACK_SYNTHETIC is set
    src/Platform/Synthetic/SyntheticProbes.cpp
    src/Platform/Synthetic/SyntheticWorkload.cpp
)

# Platform-specific sources
set(PLATFORM_SOURCES_LINUX
    src/Platform/Linux/LinuxProcessProbe.cpp
    src/Platform/Linux/LinuxProcessControl.cpp
    src/Platform/Linux/LinuxProcessActions.cpp
    # The privileged broker (#2307): its socket transport and the helper side, which tests drive directly
    src/Platform/Linux/BrokerSocket.cpp
    src/Platform/Linux/BrokerServer.cpp
    src/Platform/Linux/HelperPrivileges.cpp
    src/Platform/Linux/PkexecBroker.cpp
    src/Platform/Linux/LinuxProcessIconSource.cpp
    src/Platform/Linux/DetachedSpawn.cpp
    src/Platform/Linux/LinuxProcessEnvironmentReader.cpp
    src/Platform/Linux/LinuxProcessConnectionsReader.cpp
    src/Platform/Linux/LinuxProcessModulesReader.cpp
    src/Platform/Linux/LinuxProcessOpenFilesReader.cpp
    src/Platform/Linux/LinuxProcessSecurityReader.cpp
    src/Platform/Linux/LinuxSystemProbe.cpp
    src/Platform/Linux/SystemdUnits.cpp
    src/Platform/Linux/UDisks2Smart.cpp
    src/Platform/Linux/NetworkManager.cpp
    src/Platform/Linux/NetworkdResolved.cpp
    src/Platform/Linux/LinuxServiceActions.cpp
    src/Platform/Linux/LinuxServiceProbe.cpp
    src/Platform/Linux/SystemdJournal.cpp
    src/Platform/Linux/LinuxSystemInfoProbe.cpp
    src/Platform/Linux/LinuxPublicIp.cpp
    src/Platform/Linux/LinuxDiskProbe.cpp
    src/Platform/Linux/LinuxPathProvider.cpp
    src/Platform/Linux/LinuxPowerProbe.cpp
    src/Platform/Linux/LinuxGPUProbe.cpp
    src/Platform/Linux/AccelNpuProbe.cpp
    src/Platform/Linux/NVMLGPUProbe.cpp
    src/Platform/Linux/DRMGPUProbe.cpp
    src/Platform/Linux/ROCmGPUProbe.cpp
    src/Platform/Linux/NetlinkSocketStats.cpp
    src/Platform/Linux/Factory.cpp
)

set(PLATFORM_SOURCES_WINDOWS
    src/Platform/Windows/WindowsProcessProbe.cpp
    src/Platform/Windows/WindowsEtwNetCollector.cpp
    src/Platform/Windows/WindowsProcessActions.cpp
    src/Platform/Windows/WindowsDebugPrivilege.cpp
    src/Platform/Windows/WindowsElevation.cpp
    # The privileged broker's named-pipe transport (#2311): the pipe, the helper's serve loop, the client
    src/Platform/Windows/BrokerPipe.cpp
    src/Platform/Windows/BrokerPipeServer.cpp
    src/Platform/Windows/PipeBroker.cpp
    src/Platform/Windows/BrokerHelper.cpp # TaskSmackHelper.exe's session: parent checks, pipe, serve (#2517)
    src/Platform/Windows/HelperLauncher.cpp # starts TaskSmackHelper.exe elevated: ShellExecuteExW("runas") (#2549)
    src/Platform/Windows/WindowsProcessDump.cpp
    src/Platform/Windows/WindowsProcessMachine.cpp # the machine a process runs as: dumps and the Architecture column (#2394)
    src/Platform/Windows/WindowsProcessIdentity.cpp
    src/Platform/Windows/WindowsFolderPicker.cpp
    src/Platform/Windows/WindowsProcessConnections.cpp
    src/Platform/Windows/WindowsProcessEnvironmentReader.cpp # a process's environment block, out of its PEB (#2569)
    src/Platform/Windows/WindowsProcessIconSource.cpp
    src/Platform/Windows/WindowsProcessModules.cpp
    src/Platform/Windows/WindowsProcessOpenFiles.cpp
    src/Platform/Windows/WindowsProcessSecurity.cpp
    src/Platform/Windows/WindowsSystemProbe.cpp
    src/Platform/Windows/WindowsSystemInfoProbe.cpp
    src/Platform/Windows/WindowsCrashEvents.cpp
    src/Platform/Windows/WindowsBootPerformance.cpp
    src/Platform/Windows/WindowsPlatformSecurity.cpp
    src/Platform/Windows/WindowsThermalZones.cpp
    src/Platform/Windows/WindowsNetworkAdapters.cpp
    src/Platform/Windows/WindowsPublicIp.cpp
    src/Platform/Windows/WindowsDiskProbe.cpp
    src/Platform/Windows/WindowsPathProvider.cpp
    src/Platform/Windows/WindowsBatteryInfo.cpp
    src/Platform/Windows/WindowsPowerProbe.cpp
    src/Platform/Windows/WindowsGPUProbe.cpp
    src/Platform/Windows/WindowsDriverNodes.cpp # the SetupAPI driver-node lister the GPU probe and System Information share (#2565)
    src/Platform/Windows/DXGIGPUProbe.cpp
    src/Platform/Windows/NVMLGPUProbe.cpp
    src/Platform/Windows/PDHGPUProbe.cpp
    src/Platform/Windows/PerfProviderApartment.cpp
    src/Platform/Windows/WindowsStartupProbe.cpp
    src/Platform/Windows/WindowsStartupActions.cpp
    src/Platform/Windows/WindowsServiceActions.cpp
    src/Platform/Windows/WindowsServiceProbe.cpp
    src/Platform/Windows/WinString.cpp
    src/Platform/Windows/Factory.cpp
)

# Optional systemd D-Bus client (sd-bus, from libsystemd) on Linux: the System Information page's
# Boot performance section reads systemd's boot timestamps through it (#1525). Found with pkg-config;
# without it (or with TASKSMACK_WITH_SDBUS=OFF) the app still builds and the section says it is
# unavailable. An INTERFACE target, so the app and the tests (which compile the same probe) both get
# the library and TASKSMACK_HAS_SDBUS together.
option(TASKSMACK_WITH_SDBUS "Read systemd over D-Bus with libsystemd (sd-bus) when it is installed (Linux)" ON)
add_library(tasksmack_sdbus INTERFACE)
if(UNIX AND NOT APPLE AND TASKSMACK_WITH_SDBUS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSYSTEMD QUIET IMPORTED_TARGET libsystemd)
    endif()
    if(LIBSYSTEMD_FOUND)
        target_link_libraries(tasksmack_sdbus INTERFACE PkgConfig::LIBSYSTEMD)
        target_compile_definitions(tasksmack_sdbus INTERFACE TASKSMACK_HAS_SDBUS=1)
        message(STATUS "libsystemd ${LIBSYSTEMD_VERSION}: systemd is read over D-Bus (Boot performance section)")
    else()
        message(STATUS "libsystemd not found (install libsystemd-dev): the Boot performance section will be unavailable")
    endif()
endif()

if(UNIX AND NOT APPLE)
    list(APPEND TASKSMACK_SOURCES ${PLATFORM_SOURCES_LINUX})
elseif(WIN32)
    list(APPEND TASKSMACK_SOURCES ${PLATFORM_SOURCES_WINDOWS})
else()
    message(WARNING "Platform sources not defined for ${CMAKE_SYSTEM_NAME}")
endif()

set(TASKSMACK_HEADERS
    src/Core/Application.h
    src/Core/ConfigDirOverride.h
    src/Core/LocaleSetup.h
    src/Core/LogFileSink.h
    src/Core/PathService.h
    src/Core/Utf8Path.h
    src/Core/Window.h
    src/Core/GraphicsHostInfo.h
    src/Core/Layer.h
    src/UI/AssetPath.h
    src/UI/TabularDigits.h
    src/UI/FontLoading.h
    src/UI/UILayer.h
    src/UI/RenderMetrics.h
    src/UI/HistoryCsv.h
    src/App/ShellHost.h
    src/App/ShellLayer.h
    src/App/ShellPanels.h
    src/App/PanelPlatforms.h
    src/App/SyntheticScenario.h
    src/App/WindowOverride.h
    src/App/Panels/ProcessesPanel.h
    src/Platform/Factory.h
    src/Platform/IDiskProbe.h
    src/Platform/IGPUProbe.h
    src/Platform/IPowerProbe.h
    src/Platform/IProcessProbe.h
    src/Platform/BrokerProtocol.h
    src/Platform/IPrivilegedBroker.h
    src/Platform/BrokerSessionRules.h
    src/Platform/BrokerRequestMapping.h
    src/Platform/IProcessActions.h
    src/Platform/IProcessEnvironment.h
    src/Platform/IProcessIconSource.h
    src/Platform/IconImageFile.h
    src/Platform/ProcessEnvironment.h
    src/Platform/IProcessConnections.h
    src/Platform/IProcessModules.h
    src/Platform/IProcessOpenFiles.h
    src/Platform/IProcessSecurity.h
    src/Platform/IStartupProbe.h
    src/Platform/IStartupActions.h
    src/Platform/IServiceActions.h
    src/Platform/IServiceProbe.h
    src/Platform/ISystemProbe.h
    src/Platform/ISystemInfoProbe.h
    src/Platform/EdidParser.h
    src/Platform/VulkanInfo.h
    src/Platform/HwIdsParser.h
    src/Platform/GPUTypes.h
    src/Platform/PowerTypes.h
    src/Platform/ProcessTypes.h
    src/Platform/ProcessArchitecture.h
    src/Platform/CurrentProcess.h
    src/Platform/ThreadName.h
    src/Platform/Synthetic/SyntheticProbes.h
    src/Platform/Synthetic/SyntheticWorkload.h
    src/Domain/ProcessSnapshot.h
    src/Domain/ProcessModel.h
    src/Domain/SocketTrafficAccumulator.h
    src/Domain/ProcessConnections.h
    src/Domain/ProcessModules.h
    src/Domain/ProcessOpenFiles.h
    src/Domain/ProcessSecurity.h
    src/Domain/ISamplable.h
    src/Domain/PublicationSlot.h
    src/Domain/SharedHistory.h
    src/Domain/GPUSnapshot.h
    src/Domain/GPUModel.h
    src/Domain/StartupModel.h
    src/Domain/ServiceModel.h
    src/Domain/SensorModel.h
    src/Domain/SensorTracker.h
    src/Domain/SystemInfoModel.h
    src/Domain/CrashHistory.h
    src/Domain/LruMap.h
    src/Domain/ProcessIconCache.h
    src/UI/IconTextureCache.h
    src/App/Panels/ProcessIconView.h
)

# Platform-specific headers
if(UNIX AND NOT APPLE)
    list(APPEND TASKSMACK_HEADERS
        src/Platform/Linux/LinuxProcessProbe.h
        src/Platform/Linux/LinuxProcessEnvironmentReader.h
        src/Platform/Linux/LinuxProcessIconSource.h
        src/Platform/Linux/DesktopEntryIcons.h
        src/Platform/Linux/LinuxProcessConnectionsReader.h
        src/Platform/Linux/LinuxProcessModulesReader.h
        src/Platform/Linux/LinuxProcessSecurityReader.h
        src/Platform/Linux/ProcStatusSecurityParser.h
        src/Platform/Linux/CgroupContainer.h
        src/Platform/Linux/ElfArchitecture.h
        src/Platform/Linux/ProcMapsParser.h
        src/Platform/Linux/LinuxProcessOpenFilesReader.h
        src/Platform/Linux/ProcFdParser.h
        src/Platform/Linux/InetSocketTable.h
        src/Platform/Linux/LinuxDiskProbe.h
        src/Platform/Linux/LinuxPowerProbe.h
        src/Platform/Linux/LinuxGPUProbe.h
        src/Platform/Linux/NVMLGPUProbe.h
        src/Platform/Linux/DRMGPUProbe.h
        src/Platform/Linux/LinuxSystemInfoProbe.h
        src/Platform/Linux/LinuxOsInfo.h
        src/Platform/Linux/LinuxCommitPaging.h
        src/Platform/Linux/LinuxDiskSmart.h
        src/Platform/Linux/LinuxStorage.h
        src/Platform/Linux/LinuxGraphics.h
        src/Platform/Linux/LinuxDevices.h
        src/Platform/Linux/LinuxKernelModules.h
        src/Platform/Linux/LinuxCoredumps.h
        src/Platform/Linux/LinuxPlatformSecurity.h
        src/Platform/Linux/LinuxBootTimes.h
        src/Platform/Linux/SystemdBus.h
        src/Platform/Linux/SdBusCall.h
        src/Platform/Linux/SystemdJournal.h
        src/Platform/Linux/LinuxSensors.h
        src/Platform/Linux/LinuxNetworkAdapters.h
    )
elseif(WIN32)
    list(APPEND TASKSMACK_HEADERS
        src/Platform/Windows/WindowsProcessProbe.h
        src/Platform/Windows/WindowsProcessActions.h
        src/Platform/Windows/WindowsDebugPrivilege.h
        src/Platform/Windows/WindowsElevation.h
        src/Platform/Windows/BrokerPipe.h
        src/Platform/Windows/BrokerPipeServer.h
        src/Platform/Windows/PipeBroker.h
        src/Platform/Windows/BrokerHelper.h
        src/Platform/Windows/WindowsProcessDump.h
        src/Platform/Windows/WindowsProcessIdentity.h
        src/Platform/Windows/WindowsProcessDumpMath.h
        src/Platform/Windows/WindowsProcessMachine.h
        src/Platform/Windows/WindowsProcessMachineMath.h
        src/Platform/Windows/WindowsFolderPicker.h
        src/Platform/Windows/WindowsProcessConnections.h
        src/Platform/Windows/WindowsProcessEnvironmentReader.h
        src/Platform/Windows/WindowsProcessIconSource.h
        src/Platform/Windows/WindowsProcessIconMath.h
        src/Platform/Windows/WindowsPackageLogoMath.h
        src/Platform/Windows/WindowsProcessModules.h
        src/Platform/Windows/WindowsProcessOpenFiles.h
        src/Platform/Windows/WindowsProcessSecurity.h
        src/Platform/Windows/WindowsSystemProbe.h
        src/Platform/Windows/ProcessorPerformanceCounter.h
        src/Platform/Windows/CpuBaseClock.h
        src/Platform/Windows/WindowsHandles.h
        src/Platform/Windows/WindowsNtQuery.h
        src/Platform/Windows/WindowsDiskProbe.h
        src/Platform/Windows/WindowsBatteryInfo.h
        src/Platform/Windows/WindowsPowerProbe.h
        src/Platform/Windows/WindowsGPUProbe.h
        src/Platform/Windows/DXGIGPUProbe.h
        src/Platform/Windows/NVMLGPUProbe.h
        src/Platform/Windows/WindowsStartupProbe.h
        src/Platform/Windows/WindowsStartupActions.h
        src/Platform/Windows/WindowsServiceProbe.h
        src/Platform/Windows/WindowsServiceConfig.h
        src/Platform/Windows/WindowsServiceActions.h
        src/Platform/Windows/WinString.h
        src/Platform/Windows/WindowsSystemInfoProbe.h
        src/Platform/Windows/WindowsOsInfoMath.h
        src/Platform/Windows/WindowsInstalledMemory.h
        src/Platform/Windows/WindowsCommitPaging.h
        src/Platform/Windows/WindowsStorage.h
        src/Platform/Windows/WindowsGraphics.h
        src/Platform/Windows/WindowsDriverNodes.h
        src/Platform/Windows/WindowsDevices.h
        src/Platform/Windows/WindowsDrivers.h
        src/Platform/Windows/WindowsCrashEvents.h
        src/Platform/Windows/WindowsBootPerformance.h
        src/Platform/Windows/WindowsPlatformSecurity.h
        src/Platform/Windows/WindowsThermalZones.h
        src/Platform/Windows/WindowsNetworkAdapters.h
        src/Platform/Windows/WindowsPublicIp.h
        src/Platform/Windows/ComScope.h
    )
endif()

# Static analysis targets consume TASKSMACK_SOURCES/TASKSMACK_HEADERS as defined
# above — include before the Windows .rc file is appended so clang-tidy and
# clang-format only see real C++ sources.
include(cmake/StaticAnalysis.cmake)

# Everything but main(): compiled once, as an object library, and linked into TaskSmack and into the
# headless PGO UI training driver (TaskSmackUiTraining, src/Training/, #880). Sharing the objects keeps
# the driver from compiling the app a second time, and means the profile it records is for exactly
# the code TaskSmack is built from.
set(TASKSMACK_APP_SOURCES ${TASKSMACK_SOURCES})
list(REMOVE_ITEM TASKSMACK_APP_SOURCES src/main.cpp)

# Windows resource file (icon, version info, manifest). CMAKE_RC_COMPILER is guaranteed
# to be set by this point on WIN32 -- the pre-project() block above fails configuration
# outright if no RC compiler can be found.
if(WIN32)
    set(TASKSMACK_RC_IN ${CMAKE_SOURCE_DIR}/assets/tasksmack.rc.in)
    set(TASKSMACK_RC_OUT ${CMAKE_BINARY_DIR}/tasksmack.rc)
    set(TASKSMACK_ICO_FILE ${CMAKE_SOURCE_DIR}/assets/icons/tasksmack.ico)

    if(NOT EXISTS ${TASKSMACK_RC_IN})
        message(FATAL_ERROR "Windows resource template not found: ${TASKSMACK_RC_IN}\n"
                           "This is required for Windows builds.")
    endif()

    # Check if ICO file exists and set the define for configure_file
    if(EXISTS ${TASKSMACK_ICO_FILE})
        set(TASKSMACK_HAS_ICON TRUE)
        message(STATUS "Including application icon: ${TASKSMACK_ICO_FILE}")
    else()
        set(TASKSMACK_HAS_ICON FALSE)
        message(STATUS "Icon file not found (run tools/generate-icons.ps1 to create): ${TASKSMACK_ICO_FILE}")
    endif()

    # Configure the RC file with version info
    set(TASKSMACK_RC_FILE_DESCRIPTION "TaskSmack - System Monitor")
    set(TASKSMACK_RC_INTERNAL_NAME "TaskSmack")
    configure_file(${TASKSMACK_RC_IN} ${TASKSMACK_RC_OUT} @ONLY)
    # The same for TaskSmackHelper.exe (#2557), so its UAC prompt and Explorer name TaskSmack's helper,
    # not just a file name.
    set(TASKSMACK_HELPER_RC_OUT ${CMAKE_BINARY_DIR}/tasksmack-helper.rc)
    set(TASKSMACK_RC_FILE_DESCRIPTION "TaskSmack administrator helper")
    set(TASKSMACK_RC_INTERNAL_NAME "TaskSmackHelper")
    configure_file(${TASKSMACK_RC_IN} ${TASKSMACK_HELPER_RC_OUT} @ONLY)
    unset(TASKSMACK_RC_FILE_DESCRIPTION)
    unset(TASKSMACK_RC_INTERNAL_NAME)
    message(STATUS "Configured Windows resource files with version ${PROJECT_VERSION}")

    # Configure and use our custom manifest for DPI awareness and Windows compatibility
    set(TASKSMACK_MANIFEST_IN ${CMAKE_SOURCE_DIR}/assets/app.manifest.in)
    set(TASKSMACK_MANIFEST_OUT ${CMAKE_BINARY_DIR}/app.manifest)
    if(NOT EXISTS ${TASKSMACK_MANIFEST_IN})
        message(FATAL_ERROR "Application manifest template not found: ${TASKSMACK_MANIFEST_IN}\n"
                           "This is required for Windows builds.")
    endif()

    configure_file(${TASKSMACK_MANIFEST_IN} ${TASKSMACK_MANIFEST_OUT} @ONLY)
    message(STATUS "Configured application manifest with version ${PROJECT_VERSION}")
endif()

# Enable unity compilation only after third-party targets have been declared. Setting
# CMAKE_UNITY_BUILD in a preset would also combine dependency sources and can trigger
# collisions between their private macros and identifiers.
if(TASKSMACK_ENABLE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 16)
endif()

# The Windows compile definitions and system libraries the platform sources need, in one place for
# every target that compiles them: tasksmack_configure_app_target() below and TaskSmackTests
# (tests/CMakeLists.txt), so the two can't drift (#1702). The tests link pdh, psapi and userenv on
# top of it. Call it only when WIN32. tasksmack_apply_windows_definitions() is the definitions alone, for
# TaskSmackHelper.exe, which links a smaller list of its own (#2557).
function(tasksmack_apply_windows_definitions target)
    # Windows SDK version: GetIfTable2 and MIB_IF_TABLE2 are available on Vista+,
    # but we intentionally target Windows 10+ via _WIN32_WINNT=0x0A00 / WINVER=0x0A00.
    # NTDDI_VERSION is required for iphlpapi.h to include netioapi.h (GetIfTable2 APIs).
    target_compile_definitions(${target} PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NTDDI_VERSION=0x0A000000
        # Make spdlog's filename_t std::wstring on Windows so the log file is opened by its
        # native UTF-16 path (Core/LogFileSink.h, which static_asserts it). Kept after the
        # manifest's activeCodePage=UTF-8 (#1648): without it spdlog uses std::string +
        # _fsopen, which goes through the active code page -- UTF-8 only in TaskSmack.exe.
        # TaskSmackUiTraining and the tests have no manifest, so there a %TEMP% under a
        # non-ASCII user name fails or names another file; tests/App/test_NonAsciiRoundTrips.cpp
        # shows it. The same on every target built from the app's sources and on TaskSmackTests,
        # so spdlog's templates agree across them; spdlog is consumed header-only
        # (spdlog::spdlog_header_only), so there is no prebuilt library whose ABI could disagree.
        SPDLOG_WCHAR_FILENAMES
    )
endfunction()

function(tasksmack_apply_windows_platform target)
    tasksmack_apply_windows_definitions(${target})
    target_link_libraries(${target} PRIVATE
        iphlpapi
        ws2_32
        dxgi
        gdi32
        cfgmgr32 # DisplayDevicePower: a GPU's PnP power state (#1265)
        version
        powrprof # CallNtPowerInformation: the rated base CPU clock (#1530)
        setupapi # WindowsBatteryInfo: the battery devices (#1523)
        wevtapi # WindowsCrashEvents: the Application log's crash and hang events (#1524)
        wintrust # WindowsSystemInfoProbe: driver images' signatures, embedded and catalog (#1661)
        crypt32 # WindowsSystemInfoProbe: a driver signer's certificate name (#1661)
        advapi32 # WindowsEtwNetCollector: the opt-in UDP event trace (StartTraceW, ProcessTrace, ...) (#1492)
        bcrypt # BrokerPipe: the broker pipe's random name (BCryptGenRandom) (#2311)
    )
endfunction()

# The settings every target built from the app's sources shares: TaskSmackApp (the objects),
# TaskSmack (main() and the Windows resources), TaskSmackUiTraining (src/Training/, #880) and
# TaskSmackBenchmarks (benchmarks/, #1691). Paths name the project's own directories, not
# CMAKE_CURRENT_*, since benchmarks/CMakeLists.txt calls it from a subdirectory.
function(tasksmack_configure_app_target target)
    target_compile_features(${target} PRIVATE cxx_std_23)

    # Compile-time flamegraph tracing is limited to TaskSmack sources so FetchContent
    # dependencies do not emit unrelated traces or incur tracing overhead.
    if(TASKSMACK_ENABLE_TIME_TRACE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-ftime-trace>")
    endif()

    # Keep libc++, pthread, and the selected linker consistent across all
    # TaskSmack-owned executables and configurations.
    tasksmack_apply_linux_toolchain(${target})

    tasksmack_apply_default_warnings(${target})
    tasksmack_apply_text_encoding(${target})

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Debug>:-glldb>
            $<$<CONFIG:RelWithDebInfo>:-glldb>
        )
    endif()

    target_include_directories(${target} PRIVATE
        ${TaskSmack_SOURCE_DIR}/src
        ${TaskSmack_BINARY_DIR}/generated
    )

    # SYSTEM (not PRIVATE above): stb is third-party, and the FetchContent_Declare(SYSTEM)
    # keyword only marks CMake's own view of the dependency, not this consuming include path -
    # without this, its headers were reported as project code by warnings-as-errors, papered
    # over by manual #pragma diagnostic guards at the one #include site (IconLoader.cpp) that
    # would silently stop covering any future stb-touching file or warning class.
    target_include_directories(${target} SYSTEM PRIVATE ${stb_SOURCE_DIR})

    target_link_libraries(${target} PRIVATE
        spdlog::spdlog_header_only
        imgui_lib
        implot_lib
        tomlplusplus::tomlplusplus
        OpenGL::GL
    )

    if(WIN32)
        tasksmack_apply_windows_platform(${target})
    elseif(UNIX AND NOT APPLE)
        # tasksmack_sdbus: libsystemd and TASKSMACK_HAS_SDBUS when found (#1525)
        target_link_libraries(${target} PRIVATE dl tasksmack_sdbus)
    endif()
endfunction()

if(TASKSMACK_ENABLE_TIME_TRACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(STATUS "Compile-time tracing enabled (-ftime-trace). Run tools/time-trace.sh after build.")
    else()
        message(WARNING "TASKSMACK_ENABLE_TIME_TRACE requires Clang. Ignoring on ${CMAKE_CXX_COMPILER_ID}.")
    endif()
endif()

# Everything but main(), compiled once (see TASKSMACK_APP_SOURCES above).
add_library(TaskSmackApp OBJECT
    ${TASKSMACK_APP_SOURCES}
    ${TASKSMACK_HEADERS}
)
tasksmack_configure_app_target(TaskSmackApp)

# Precompiled headers, for the app's sources; main.cpp, alone in TaskSmack, is not worth one.
if(TASKSMACK_ENABLE_PCH)
    include(cmake/PrecompiledHeaders.cmake)
    target_precompile_headers(TaskSmackApp PRIVATE ${TASKSMACK_PCH_HEADERS})
endif()

add_executable(TaskSmack WIN32
    src/main.cpp
    ${TASKSMACK_RC_OUT} # Windows resources (icon, version info); unset elsewhere
    $<TARGET_OBJECTS:TaskSmackApp>
)
tasksmack_configure_app_target(TaskSmack)

# Embed custom manifest on Windows (for DPI awareness and OS compatibility)
if(WIN32 AND EXISTS ${CMAKE_BINARY_DIR}/app.manifest)
    target_link_options(TaskSmack PRIVATE
        "LINKER:/MANIFESTINPUT:${CMAKE_BINARY_DIR}/app.manifest"
    )
endif()

# For multi-config generators (e.g., Visual Studio, Ninja Multi-Config), include
# $<CONFIG> in the output path to prevent Debug/Release binary collisions.
# For single-config generators (Ninja), emit directly to bin/ so script and doc
# paths (e.g., build/pgo-use/bin/TaskSmack) remain correct.
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_is_multi_config)
    set_target_properties(TaskSmack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
else()
    set_target_properties(TaskSmack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()
unset(_is_multi_config)

# Copy assets (fonts and themes) to build directory
set(TASKSMACK_ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets")
add_custom_command(TARGET TaskSmack POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TaskSmack>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${TASKSMACK_ASSETS_DIR}/fonts"
        "$<TARGET_FILE_DIR:TaskSmack>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TaskSmack>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${TASKSMACK_ASSETS_DIR}/icons"
        "$<TARGET_FILE_DIR:TaskSmack>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TaskSmack>/assets/themes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${TASKSMACK_ASSETS_DIR}/themes"
        "$<TARGET_FILE_DIR:TaskSmack>/assets/themes"
    COMMENT "Copying assets (fonts, themes) to build directory"
)

# The headless PGO UI training driver (#880): the real panels, on the app's own objects, rendered
# for a fixed, weighted number of frames without a window. tools/pgo.sh and tools/pgo.ps1 run it
# after the benchmarks; ctest runs it for a few frames as a smoke test (tests/CMakeLists.txt).
if(TASKSMACK_BUILD_UI_TRAINING)
    add_executable(TaskSmackUiTraining
        src/Training/main.cpp
        src/Training/UiTrainingDriver.cpp
        src/Training/UiTrainingPlan.cpp
        src/Training/UiTrainingDriver.h
        src/Training/UiTrainingPlan.h
        $<TARGET_OBJECTS:TaskSmackApp>
    )
    tasksmack_configure_app_target(TaskSmackUiTraining)
    # Beside TaskSmack, whose POST_BUILD step copies the fonts and themes the driver loads.
    add_dependencies(TaskSmackUiTraining TaskSmack)
    get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_is_multi_config)
        set_target_properties(TaskSmackUiTraining PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    else()
        set_target_properties(TaskSmackUiTraining PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
    unset(_is_multi_config)
endif()

# tasksmack-helper (#2307; #2181): the privileged broker TaskSmack starts with pkexec when the user
# authenticates for an action it can't do itself. Platform sources only -- no SDL, ImGui, GL, config
# or themes -- so as little as possible runs as root. Of the process actions it links only the
# process-control half (LinuxProcessControl), so nothing that starts strace, gcore or a terminal is in it,
# and it never searches PATH for them (#2640). Linux only; Windows has its own transport (#2311).
if(UNIX AND NOT APPLE)
    add_executable(tasksmack-helper
        src/Platform/Linux/HelperMain.cpp
        src/Platform/Linux/HelperPrivileges.cpp # drops to the capabilities its requests need (#2640)
        src/Platform/BrokerRequestMapping.cpp
        src/Platform/Linux/BrokerServer.cpp
        src/Platform/Linux/BrokerSocket.cpp
        src/Platform/Linux/LinuxProcessControl.cpp # not LinuxProcessActions: no trace, dump or spawn (#2640)
        src/Platform/Linux/LinuxProcessEnvironmentReader.cpp
        src/Platform/Linux/LinuxProcessModulesReader.cpp
        src/Platform/Linux/LinuxProcessOpenFilesReader.cpp
        src/Platform/Linux/LinuxProcessConnectionsReader.cpp
        src/Platform/Linux/NetlinkSocketStats.cpp
        src/Platform/Linux/SystemdJournal.cpp # every user's crashes for System Information (#2310)
    )
    target_compile_features(tasksmack-helper PRIVATE cxx_std_23)
    tasksmack_apply_linux_toolchain(tasksmack-helper)
    tasksmack_apply_default_warnings(tasksmack-helper)
    tasksmack_apply_text_encoding(tasksmack-helper)
    tasksmack_apply_privileged_hardening(tasksmack-helper) # runs as root: full RELRO, CET/BTI, FORTIFY 3 (#2640)
    target_include_directories(tasksmack-helper PRIVATE ${TaskSmack_SOURCE_DIR}/src ${TaskSmack_BINARY_DIR}/generated)
    target_link_libraries(tasksmack-helper PRIVATE spdlog::spdlog_header_only)
    if(TARGET tasksmack_sdbus)
        target_link_libraries(tasksmack-helper PRIVATE tasksmack_sdbus) # sd-journal, when libsystemd is installed
    endif()
    # Beside TaskSmack in the build tree, where the tests find it.
    set_target_properties(tasksmack-helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# TaskSmackHelper.exe (#2517; #2181): the elevated broker TaskSmack starts with ShellExecuteExW("runas")
# when the user chooses an action it can't do itself. Platform sources only -- no SDL, ImGui, GL, config
# or themes -- so as little as possible runs elevated. A Windows-subsystem program, so starting it opens
# no console window. Windows only; Linux has tasksmack-helper above.
if(WIN32)
    add_executable(TaskSmackHelper WIN32
        src/Platform/Windows/HelperMain.cpp
        src/Platform/Windows/BrokerHelper.cpp
        src/Platform/Windows/BrokerPipe.cpp
        src/Platform/Windows/BrokerPipeServer.cpp
        src/Platform/BrokerRequestMapping.cpp
        src/Platform/Windows/WindowsDebugPrivilege.cpp
        src/Platform/Windows/WindowsElevation.cpp
        src/Platform/Windows/WindowsProcessActions.cpp
        src/Platform/Windows/WindowsProcessIdentity.cpp
        src/Platform/Windows/WindowsProcessMachine.cpp
        src/Platform/Windows/WindowsProcessModules.cpp
        src/Platform/Windows/WindowsProcessOpenFiles.cpp
        src/Platform/Windows/WindowsProcessConnections.cpp
        src/Platform/Windows/WinString.cpp
        src/Platform/ThreadName.cpp
        ${TASKSMACK_HELPER_RC_OUT} # version info (#2557): "TaskSmack administrator helper" in its UAC prompt
    )
    target_compile_features(TaskSmackHelper PRIVATE cxx_std_23)
    tasksmack_apply_default_warnings(TaskSmackHelper)
    tasksmack_apply_text_encoding(TaskSmackHelper)
    tasksmack_apply_windows_definitions(TaskSmackHelper)
    target_include_directories(TaskSmackHelper PRIVATE ${TaskSmack_SOURCE_DIR}/src ${TaskSmack_BINARY_DIR}/generated)
    # Only what the helper calls (#2557), not the app's whole list. No dump writer and no COM folder
    # picker: WindowsProcessActions.cpp leaves its dump members to IProcessActions' refusals without them.
    target_compile_definitions(TaskSmackHelper PRIVATE TASKSMACK_NO_PROCESS_DUMPS)
    target_link_libraries(TaskSmackHelper PRIVATE spdlog::spdlog_header_only
        advapi32 # tokens, security descriptors and AccessCheck (BrokerHelper, BrokerPipe)
        bcrypt # BCryptGenRandom (BrokerPipe)
        iphlpapi # the process's TCP and UDP tables (WindowsProcessConnections)
        ws2_32 # ntohs and the address formatting there
        version # a module's file description and version (WindowsProcessModules)
    )
    # Exploit mitigations for the one image that runs elevated (#2557): Control Flow Guard (the compiler's
    # checks on indirect calls, and the linker's table of their valid targets) and CET shadow-stack
    # compatibility, so Windows checks returns against a hardware shadow stack where the CPU has one.
    # Not under a sanitizer, whose runtime isn't built for CFG; TASKSMACK_HELPER_HARDENED tells the tests
    # what to expect.
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _helper_config)
    if("${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_helper_config}}" MATCHES "-fsanitize")
        set(TASKSMACK_HELPER_HARDENED OFF)
        message(STATUS "TaskSmackHelper: CFG and CETCOMPAT off under a sanitizer")
    else()
        set(TASKSMACK_HELPER_HARDENED ON)
        # clang-cl's /guard:cf; the plain driver has no spelling of its own for an MSVC target (-mguard is
        # MinGW's). A module flag, so ThinLTO's link-time code generation keeps it.
        target_compile_options(TaskSmackHelper PRIVATE "SHELL:-Xclang -cfguard")
        target_link_options(TaskSmackHelper PRIVATE "LINKER:/guard:cf" "LINKER:/CETCOMPAT")
    endif()
    unset(_helper_config)
    # The app's manifest: asInvoker (so only the launcher's "runas" elevates it, and Windows' installer
    # detection never does), long paths and the UTF-8 code page. And every DLL its imports load comes
    # from System32 only (LOAD_LIBRARY_SEARCH_SYSTEM32), never the helper's own folder, which a
    # portable TaskSmack may have unpacked somewhere its user can write; HelperMain.cpp sets the same
    # for anything loaded later.
    target_link_options(TaskSmackHelper PRIVATE
        "LINKER:/MANIFESTINPUT:${TASKSMACK_MANIFEST_OUT}"
        "LINKER:/DEPENDENTLOADFLAG:0x800"
    )
    # Beside TaskSmack.exe, where TaskSmack starts it from, the tests find it, and release packaging
    # (and its Authenticode signing, #2302) picks it up.
    get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_is_multi_config)
        set_target_properties(TaskSmackHelper PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    else()
        set_target_properties(TaskSmackHelper PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
    unset(_is_multi_config)
endif()

include(cmake/InstallRules.cmake)

if(TASKSMACK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TASKSMACK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(cmake/Packaging.cmake)
