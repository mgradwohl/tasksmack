# Google Benchmark setup
include(FetchContent)

FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG 192ef10025eb2c4cdd392bc502f0c852196baa48  # v1.9.5 - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings
    BINARY_DIR "${TASKSMACK_DEPS_BINARY_DIR}/benchmark-build"  # per-preset build tree (#1308)
)

# Disable Google Benchmark tests to speed up build
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(benchmark)

# Suppress warnings from benchmark library
if(TARGET benchmark)
    get_target_property(_includes benchmark INTERFACE_INCLUDE_DIRECTORIES)
    if(_includes)
        set_target_properties(benchmark PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_includes}"
        )
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(benchmark PRIVATE -w)
    endif()
endif()

# Platform-specific benchmark sources
if(WIN32)
    set(PLATFORM_BENCH_SOURCES
        bench_WindowsPDHGPUProbe.cpp
    )
    set(PLATFORM_SRC_UNDER_BENCH
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
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WindowsServiceProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WinString.cpp
    )
else()
    set(PLATFORM_BENCH_SOURCES
        # Linux-specific benchmarks
        bench_NetlinkSocketStats.cpp
        bench_LinuxProcessProbe.cpp
    )
    set(PLATFORM_SRC_UNDER_BENCH
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/Factory.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxProcessProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxSystemProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxProcessActions.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxProcessEnvironmentReader.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Linux/LinuxProcessConnectionsReader.cpp
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

# Benchmark executable
add_executable(TaskSmackBenchmarks
    bench_main.cpp
    bench_History.cpp
    bench_ProcessModel.cpp
    bench_SystemModel.cpp
    bench_StorageModel.cpp
    bench_GPUModel.cpp
    bench_Numeric.cpp
    bench_Format.cpp
    bench_UI.cpp
    bench_ChartGeometry.cpp
    bench_ModelLimits.cpp
    bench_ProcessDetailsHistory.cpp
    ${PLATFORM_BENCH_SOURCES}
    # Source files under benchmark
    ${CMAKE_SOURCE_DIR}/src/Domain/ProcessModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/GPUModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/StorageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/BackgroundSampler.cpp
    # bench_ChartGeometry.cpp draws the real charts under a headless ImGui/ImPlot context (#1421):
    # ChartLegend.cpp for the series marker glyphs, and the test binary's Theme stub (a visible colour
    # scheme, no fonts) in place of Theme.cpp, which needs a renderer to load its fonts.
    ${CMAKE_SOURCE_DIR}/src/UI/ChartLegend.cpp
    ${CMAKE_SOURCE_DIR}/tests/Mocks/ThemeStub.cpp
    ${CMAKE_SOURCE_DIR}/src/Platform/ThreadName.cpp
    ${PLATFORM_SRC_UNDER_BENCH}
)

target_include_directories(TaskSmackBenchmarks PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests  # Mocks/MockProbes.h, Mocks/MockDiskProbe.h (bench_ModelLimits.cpp, #1422)
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
    ${implot_SOURCE_DIR}
)

target_link_libraries(TaskSmackBenchmarks PRIVATE
    benchmark::benchmark
    spdlog::spdlog_header_only
    implot_lib  # And imgui_lib through it: bench_ChartGeometry.cpp renders headless frames (#1421)
)
tasksmack_apply_linux_toolchain(TaskSmackBenchmarks)

# Platform-specific library linking
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(TaskSmackBenchmarks PRIVATE X11::X11 ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    target_compile_definitions(TaskSmackBenchmarks PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NTDDI_VERSION=0x0A000000
    )
    target_link_libraries(TaskSmackBenchmarks PRIVATE
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

# Apply project warnings (benchmarks should compile clean too)
tasksmack_apply_default_warnings(TaskSmackBenchmarks)

# Disable warnings that conflict with Google Benchmark's use of __COUNTER__ and DoNotOptimize
# These are from the benchmark library's macros, not our code
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(TaskSmackBenchmarks PRIVATE
        -Wno-c2y-extensions              # __COUNTER__ in benchmark macros
        -Wno-deprecated-declarations     # DoNotOptimize deprecation
    )
endif()

# Output location – use a config-specific subdirectory under multi-config
# generators (Visual Studio, Ninja Multi-Config) to avoid Debug/Release
# binary collisions; single-config generators (Ninja) get a flat bin/ path
# so PGO scripts and docs remain correct.
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_is_multi_config)
    set_target_properties(TaskSmackBenchmarks PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
else()
    set_target_properties(TaskSmackBenchmarks PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()
unset(_is_multi_config)

# Build information for the benchmark provenance manifests (tools/bench.ps1, tools/bench-manifest.py),
# written next to the binary for each configuration: TaskSmackBenchmarks.buildinfo.json. It records
# the interprocedural optimization (IPO) that configuration really builds with, mirroring CMake's
# own rule (cmGeneratorTarget::GetFeature): INTERPROCEDURAL_OPTIMIZATION_<CONFIG> when set, else
# INTERPROCEDURAL_OPTIMIZATION -- both initialized at target creation from the
# CMAKE_INTERPROCEDURAL_OPTIMIZATION[_<CONFIG>] variables, which cmake/CompilerOptions.cmake sets as
# a normal variable when TASKSMACK_ENABLE_IPO is on and IPO is supported (so the cache can still
# say OFF). Being a generate-time file, it is per configuration under multi-config generators too.
# (CMake's last resort, a directory-level INTERPROCEDURAL_OPTIMIZATION property, is not used here.)
#
# It is generated into this directory and copied next to the binary only when the target links
# (POST_BUILD), so it always describes the binary beside it: a reconfigure (IPO ON to OFF) without
# a rebuild leaves the ON binary with its ON file.
#
# A per-configuration value that is set but empty (-DCMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG:BOOL=)
# still wins in CMake, as false, but a generator expression cannot tell it from an unset one and
# would fall back to the generic setting. It is made an explicit OFF first: the same as CMake's
# reading of an empty value, so the build does not change.
#
# It also records the SHA-256 of the cached CMAKE_CXX_FLAGS and CMAKE_CXX_FLAGS_<CONFIG> this
# configuration builds with -- the exact cache values, UTF-8, as the writers hash them from
# CMakeCache.txt; null for an entry that is not in the cache -- so a reconfigure of the flags
# without a rebuild does not change what the binary is reported to be built with either.
if(CMAKE_CONFIGURATION_TYPES)
    set(_benchmarks_configs ${CMAKE_CONFIGURATION_TYPES})
else()
    set(_benchmarks_configs ${CMAKE_BUILD_TYPE})
endif()
if(DEFINED CACHE{CMAKE_CXX_FLAGS})
    string(SHA256 _benchmarks_hash "$CACHE{CMAKE_CXX_FLAGS}")
    set(_benchmarks_flags_hash "\"${_benchmarks_hash}\"")
else()
    set(_benchmarks_flags_hash "null")
endif()
# The configuration's hash, chosen by $<CONFIG>; null for a configuration not listed here (a
# single-config tree without CMAKE_BUILD_TYPE).
set(_benchmarks_config_hashes "")
set(_benchmarks_config_names "")
foreach(_benchmarks_cfg IN LISTS _benchmarks_configs)
    string(TOUPPER "${_benchmarks_cfg}" _benchmarks_cfg)
    if(DEFINED CACHE{CMAKE_CXX_FLAGS_${_benchmarks_cfg}})
        string(SHA256 _benchmarks_hash "$CACHE{CMAKE_CXX_FLAGS_${_benchmarks_cfg}}")
        string(APPEND _benchmarks_config_hashes "$<$<CONFIG:${_benchmarks_cfg}>:\"${_benchmarks_hash}\">")
    else()
        string(APPEND _benchmarks_config_hashes "$<$<CONFIG:${_benchmarks_cfg}>:null>")
    endif()
    list(APPEND _benchmarks_config_names "${_benchmarks_cfg}")
    get_property(_benchmarks_ipo_set TARGET TaskSmackBenchmarks PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_benchmarks_cfg} SET)
    get_property(_benchmarks_ipo_value TARGET TaskSmackBenchmarks PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_benchmarks_cfg})
    if(_benchmarks_ipo_set AND _benchmarks_ipo_value STREQUAL "")
        set_property(TARGET TaskSmackBenchmarks PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_benchmarks_cfg} OFF)
    endif()
endforeach()
if(_benchmarks_config_names)
    list(JOIN _benchmarks_config_names "," _benchmarks_config_names)
    set(_benchmarks_config_flags_hash "$<IF:$<CONFIG:${_benchmarks_config_names}>,${_benchmarks_config_hashes},null>")
else()
    set(_benchmarks_config_flags_hash "null")
endif()
unset(_benchmarks_configs)
unset(_benchmarks_cfg)
unset(_benchmarks_hash)
unset(_benchmarks_config_hashes)
unset(_benchmarks_config_names)
unset(_benchmarks_ipo_set)
unset(_benchmarks_ipo_value)
set(_benchmarks_ipo_config "$<TARGET_PROPERTY:TaskSmackBenchmarks,INTERPROCEDURAL_OPTIMIZATION_$<UPPER_CASE:$<CONFIG>>>")
set(_benchmarks_ipo_generic "$<TARGET_PROPERTY:TaskSmackBenchmarks,INTERPROCEDURAL_OPTIMIZATION>")
set(_benchmarks_ipo "$<IF:$<STREQUAL:${_benchmarks_ipo_config},>,${_benchmarks_ipo_generic},${_benchmarks_ipo_config}>")
set(_benchmarks_buildinfo "${CMAKE_CURRENT_BINARY_DIR}/buildinfo/$<CONFIG>/TaskSmackBenchmarks.buildinfo.json")
file(GENERATE
    OUTPUT "${_benchmarks_buildinfo}"
    CONTENT "{\n  \"schema_version\": 1,\n  \"config\": \"$<CONFIG>\",\n  \"ipo\": \"$<IF:$<BOOL:${_benchmarks_ipo}>,ON,OFF>\",\n  \"cxx_flags_sha256\": ${_benchmarks_flags_hash},\n  \"cxx_flags_config_sha256\": ${_benchmarks_config_flags_hash}\n}\n"
)
add_custom_command(TARGET TaskSmackBenchmarks POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "${_benchmarks_buildinfo}" "$<TARGET_FILE_DIR:TaskSmackBenchmarks>/TaskSmackBenchmarks.buildinfo.json"
    COMMENT "Recording the build information of TaskSmackBenchmarks"
    VERBATIM
)
unset(_benchmarks_buildinfo)
unset(_benchmarks_flags_hash)
unset(_benchmarks_config_flags_hash)
unset(_benchmarks_ipo_config)
unset(_benchmarks_ipo_generic)
unset(_benchmarks_ipo)
