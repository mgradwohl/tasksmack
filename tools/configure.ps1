<#
.SYNOPSIS
    DEPRECATED: Use CMake presets instead.
.DESCRIPTION
    This script is deprecated. Use CMake presets instead:
        cmake --preset win-debug
        cmake --preset win-release
    Run 'cmake --list-presets' to see all available presets.

    This script is kept for backwards compatibility.
    Runs CMake configure with appropriate settings for the specified build type.
.PARAMETER BuildType
    Build type: debug, relwithdebinfo, release, optimized (default: debug)
.PARAMETER ShowDetails
    Show verbose CMake output
.EXAMPLE
    .\configure.ps1 debug
.EXAMPLE
    .\configure.ps1 -ShowDetails optimized
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("debug", "relwithdebinfo", "release", "optimized")]
    [string]$BuildType = "debug",

    [switch]$ShowDetails
)

$ErrorActionPreference = "Stop"

Write-Warning "This script is deprecated. Consider using CMake presets instead:"
Write-Host "  cmake --preset win-debug"
Write-Host "  cmake --preset win-release"
Write-Host "Run 'cmake --list-presets' to see all available presets."
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

# Validate environment
if (-not $env:LLVM_ROOT) {
    Write-Error "LLVM_ROOT environment variable not set"
    exit 1
}

# Paths
$ClangCXX = "$env:LLVM_ROOT\bin\clang++.exe"
$ClangC = "$env:LLVM_ROOT\bin\clang.exe"

# Build directory (Windows uses win- prefix)
$BuildDir = Join-Path $ProjectRoot "build\win-$BuildType"

# Common CMake args
$CMakeArgs = @(
    "-S", $ProjectRoot
    "-B", $BuildDir
    "-G", "Ninja"
    "-DCMAKE_CXX_COMPILER=$ClangCXX"
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    "-DCMAKE_CXX_EXTENSIONS=OFF"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"
)

# Add vcpkg toolchain if available
if ($env:VCPKG_ROOT) {
    $VcpkgToolchain = "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
    if (Test-Path $VcpkgToolchain) {
        $CMakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
    }
}

# Build type specific args
switch ($BuildType) {
    "debug" {
        $CMakeArgs += "-DCMAKE_BUILD_TYPE=Debug"
    }
    "relwithdebinfo" {
        $CMakeArgs += "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
    }
    "release" {
        $CMakeArgs += "-DCMAKE_BUILD_TYPE=Release"
    }
    "optimized" {
        $CMakeArgs += @(
            "-DCMAKE_BUILD_TYPE=Release"
            "-DMYPROJECT_ENABLE_IPO=ON"
            "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON"
            "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-s"
            "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-s"
            "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -march=x86-64-v3 -fomit-frame-pointer"
        )
    }
}

if ($ShowDetails) {
    Write-Host "Configuring $BuildType build in $BuildDir"
    Write-Host "CMake args:"
    $CMakeArgs | ForEach-Object { Write-Host "  $_" }
}

& cmake $CMakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Configuration complete. Run 'tools\build.ps1 $BuildType' to build."
