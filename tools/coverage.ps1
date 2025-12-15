<#
.SYNOPSIS
    Generate code coverage report using llvm-cov (Windows).
.DESCRIPTION
    Builds with coverage instrumentation, runs tests, and generates HTML report.
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
    [switch]$ShowDetails,
    [switch]$OpenReport
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ProjectRoot "build\win-coverage"
$CoverageDir = Join-Path $ProjectRoot "coverage"

# Find llvm tools
$LlvmProfdata = $null
$LlvmCov = $null

if ($env:LLVM_ROOT) {
    $LlvmProfdata = Join-Path $env:LLVM_ROOT "bin\llvm-profdata.exe"
    $LlvmCov = Join-Path $env:LLVM_ROOT "bin\llvm-cov.exe"
    if (-not (Test-Path $LlvmProfdata)) {
        $LlvmProfdata = $null
        $LlvmCov = $null
    }
}

if (-not $LlvmProfdata) {
    $LlvmProfdata = Get-Command llvm-profdata -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    $LlvmCov = Get-Command llvm-cov -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
}

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
& cmake --preset win-coverage
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Building..."
& cmake --build --preset win-coverage
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Step 2: Run tests to generate profraw data
Write-Host "==> Running tests..."
Push-Location $BuildDir
try {
    Remove-Item -Path "*.profraw", "default.profdata" -ErrorAction SilentlyContinue

    # Set profraw output location
    $env:LLVM_PROFILE_FILE = "$BuildDir\coverage-%p.profraw"

    # Run the test executable directly
    & "$BuildDir\tests\MyProject_tests.exe"
    if ($LASTEXITCODE -ne 0) { 
        Write-Warning "Some tests failed, but continuing with coverage report..."
    }

    # Step 3: Merge profraw files
    Write-Host "==> Merging coverage data..."
    $profrawFiles = Get-ChildItem -Path $BuildDir -Filter "*.profraw" | Select-Object -ExpandProperty FullName
    & $LlvmProfdata merge -sparse $profrawFiles -o "$BuildDir\default.profdata"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # Step 4: Generate HTML report
    Write-Host "==> Generating HTML report..."
    New-Item -ItemType Directory -Force -Path $CoverageDir | Out-Null

    & $LlvmCov show `
        "$BuildDir\tests\MyProject_tests.exe" `
        "-instr-profile=$BuildDir\default.profdata" `
        -format=html `
        "-output-dir=$CoverageDir" `
        -show-line-counts-or-regions `
        -show-instantiations=false `
        "-ignore-filename-regex=.*(\\|/)(build|_deps|tests)(\\|/).*"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # Step 5: Generate summary
    Write-Host "==> Coverage Summary:"
    & $LlvmCov report `
        "$BuildDir\tests\MyProject_tests.exe" `
        "-instr-profile=$BuildDir\default.profdata" `
        "-ignore-filename-regex=.*(\\|/)(build|_deps|tests)(\\|/).*"

    Write-Host ""
    Write-Host "HTML report generated at: $CoverageDir\index.html" -ForegroundColor Green

    # Open in browser if requested
    if ($OpenReport) {
        Start-Process "$CoverageDir\index.html"
    }
}
finally {
    Pop-Location
}
