<#
.SYNOPSIS
    Generate code coverage report using llvm-cov (Windows).
.DESCRIPTION
    Builds with coverage instrumentation, runs tests, and generates HTML report.
.PARAMETER Preset
    CMake preset to use for the coverage build (default: win-coverage)
.PARAMETER ShowDetails
    Show verbose output
.PARAMETER OpenReport
    Open HTML report in browser after generation
.EXAMPLE
    .\coverage.ps1
.EXAMPLE
    .\coverage.ps1 -ShowDetails -OpenReport
#>
[CmdletBinding()]
param(
    [string]$Preset = "win-coverage",
    [switch]$ShowDetails,
    [switch]$OpenReport
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
. (Join-Path $ScriptDir "common.ps1")
# CMake presets derive compiler paths from LLVM_ROOT; ensure it is set.
Initialize-LlvmRoot
$BuildDir = Join-Path $ProjectRoot "build\$Preset"
$CoverageDir = Join-Path $ProjectRoot "coverage"

# Find llvm tools
$LlvmProfdata = Find-LlvmTool -Name "llvm-profdata"
$LlvmCov = Find-LlvmTool -Name "llvm-cov"

if (-not $LlvmProfdata) {
    Write-Error "llvm-profdata not found. Set LLVM_ROOT or add LLVM bin to PATH."
    exit 1
}

if ($ShowDetails) {
    Write-Host "Using llvm-profdata: $LlvmProfdata"
    Write-Host "Using llvm-cov: $LlvmCov"
}

# Step 1: Configure and build with coverage
Write-Host "==> Configuring coverage build..."
& cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Building..."
& cmake --build --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Step 2: Run tests to generate profraw data
Write-Host "==> Running tests..."
Push-Location $BuildDir
try {
    Remove-Item -Path "*.profraw", "default.profdata" -ErrorAction SilentlyContinue

    # Set profraw output location
    $env:LLVM_PROFILE_FILE = "$BuildDir\coverage-%p.profraw"

    # Run the test executable directly
    & "$BuildDir\tests\TaskSmackTests.exe"
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Some tests failed, but continuing with coverage report..."
    }

    # Step 3: Merge profraw files
    Write-Host "==> Merging coverage data..."
    $profrawFiles = Get-ChildItem -Path $BuildDir -Filter "*.profraw" | Select-Object -ExpandProperty FullName
    & $LlvmProfdata merge -sparse $profrawFiles -o "$BuildDir\default.profdata"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # Files excluded from coverage: generated/third-party paths, test sources,
    # and ImGui-only rendering files that cannot be exercised in unit tests.
    # Keep in sync with tools/coverage.sh COV_IGNORE_REGEX.
    $IgnoreRegex = ".*(\\|/)(build|_deps|tests|\.cache)(\\|/).*|(\\|/)UI(\\|/)Widgets\.h|(\\|/)App(\\|/)Panels(\\|/)(ProcessDetailsPanel|ProcessesPanel|SystemMetricsPanel)\.h"

    # Step 4: Generate HTML report
    Write-Host "==> Generating HTML report..."
    New-Item -ItemType Directory -Force -Path $CoverageDir | Out-Null

    & $LlvmCov show `
        "$BuildDir\tests\TaskSmackTests.exe" `
        "-instr-profile=$BuildDir\default.profdata" `
        -format=html `
        "-output-dir=$CoverageDir" `
        -show-line-counts-or-regions `
        -show-instantiations=false `
        "-ignore-filename-regex=$IgnoreRegex"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # Step 5: Generate LCOV file for Codecov
    Write-Host "==> Generating LCOV report..."
    $lcovPath = Join-Path $CoverageDir "coverage.lcov"
    & $LlvmCov export `
        "$BuildDir\tests\TaskSmackTests.exe" `
        "-instr-profile=$BuildDir\default.profdata" `
        -format=lcov `
        "-ignore-filename-regex=$IgnoreRegex" `
        | Set-Content -Path $lcovPath -Encoding utf8NoBOM
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # Step 6: Generate summary
    Write-Host "==> Coverage Summary:"
    & $LlvmCov report `
        "$BuildDir\tests\TaskSmackTests.exe" `
        "-instr-profile=$BuildDir\default.profdata" `
        "-ignore-filename-regex=$IgnoreRegex"

    Write-Host ""
    Write-Host "HTML report generated at: $CoverageDir\index.html" -ForegroundColor Green
    Write-Host "LCOV report generated at: $lcovPath" -ForegroundColor Green

    # Open in browser if requested
    if ($OpenReport) {
        Start-Process "$CoverageDir\index.html"
    }
}
finally {
    Pop-Location
}
