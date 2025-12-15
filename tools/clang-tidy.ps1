<#
.SYNOPSIS
    Run clang-tidy on source files (Windows).
.DESCRIPTION
    Configures if needed, then runs clang-tidy via CMake target.
    Uses .clang-tidy configuration from project root.
.PARAMETER BuildType
    Build type to use: debug, relwithdebinfo (default: debug)
.PARAMETER ShowDetails
    Show verbose output
.EXAMPLE
    .\clang-tidy.ps1
.EXAMPLE
    .\clang-tidy.ps1 -ShowDetails debug
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("debug", "relwithdebinfo")]
    [string]$BuildType = "debug",

    [switch]$ShowDetails
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

# Build directory
$BuildDir = Join-Path $ProjectRoot "build\win-$BuildType"

# Configure if needed (using CMake presets)
$NinjaFile = Join-Path $BuildDir "build.ninja"
if (-not (Test-Path $NinjaFile)) {
    Write-Host "Build not configured. Running cmake --preset win-$BuildType..."
    & cmake --preset "win-$BuildType"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($ShowDetails) {
    Write-Host "Running clang-tidy for $BuildType build..."
}

# Copy compile_commands.json and run clang-tidy
& cmake --build $BuildDir --target copy-compile-commands run-clang-tidy
exit $LASTEXITCODE
