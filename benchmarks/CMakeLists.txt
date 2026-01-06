# Google Benchmark setup
include(FetchContent)

FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG 96afad55c79e02f5dfca1374e772c2be72ba631b  # v1.9.1 - pinned to SHA for supply chain security
    SYSTEM  # Treat as system headers to suppress warnings
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
        # Windows-specific benchmarks (if any)
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
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/D3DKMTGPUProbe.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/Windows/WinString.cpp
    )
else()
    set(PLATFORM_BENCH_SOURCES
        # Linux-specific benchmarks (if any)
    )
    set(PLATFORM_SRC_UNDER_BENCH
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

# Benchmark executable
add_executable(TaskSmackBenchmarks
    bench_main.cpp
    bench_History.cpp
    bench_ProcessModel.cpp
    bench_Format.cpp
    ${PLATFORM_BENCH_SOURCES}
    # Source files under benchmark
    ${CMAKE_SOURCE_DIR}/src/Domain/ProcessModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/GPUModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/SystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/StorageModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Domain/BackgroundSampler.cpp
    ${PLATFORM_SRC_UNDER_BENCH}
)

target_include_directories(TaskSmackBenchmarks PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
    ${implot_SOURCE_DIR}
)

target_link_libraries(TaskSmackBenchmarks PRIVATE
    benchmark::benchmark
    benchmark::benchmark_main
    spdlog::spdlog_header_only
)

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

# Output location
set_target_properties(TaskSmackBenchmarks PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
