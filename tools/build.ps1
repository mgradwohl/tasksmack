<#
.SYNOPSIS
    DEPRECATED: Use CMake presets instead.
.DESCRIPTION
    This script is deprecated. Use CMake presets instead:
        cmake --build --preset win-debug
        cmake --build --preset win-release
    Run 'cmake --list-presets' to see all available presets.

    This script is kept for backwards compatibility.
    Runs CMake build for the specified build type.
.PARAMETER BuildType
    Build type: debug, relwithdebinfo, release, optimized (default: debug)
.PARAMETER Target
    Build specific target(s)
.PARAMETER ShowDetails
    Show verbose build output
.EXAMPLE
    .\build.ps1 debug
.EXAMPLE
    .\build.ps1 -Target run-clang-tidy debug
.EXAMPLE
    .\build.ps1 -ShowDetails optimized
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("debug", "relwithdebinfo", "release", "optimized")]
    [string]$BuildType = "debug",

    [string]$Target = "",

    [switch]$ShowDetails
)

$ErrorActionPreference = "Stop"

Write-Warning "This script is deprecated. Consider using CMake presets instead:"
Write-Host "  cmake --build --preset win-debug"
Write-Host "  cmake --build --preset win-release"
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

# Build directory
$BuildDir = Join-Path $ProjectRoot "build\win-$BuildType"

# Check if configured
$NinjaFile = Join-Path $BuildDir "build.ninja"
if (-not (Test-Path $NinjaFile)) {
    Write-Error "Build directory '$BuildDir' not configured. Run 'tools\configure.ps1 $BuildType' first."
    exit 1
}

# Build arguments
$CMakeArgs = @("--build", $BuildDir)

if ($Target) {
    $CMakeArgs += @("--target", $Target)
}

if ($ShowDetails) {
    $CMakeArgs += "--verbose"
    Write-Host "Building $BuildType in $BuildDir"
    if ($Target) {
        Write-Host "Target: $Target"
    }
}

& cmake $CMakeArgs
exit $LASTEXITCODE
