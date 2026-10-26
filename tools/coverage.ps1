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
# llvm-cov writes UTF-8 source paths into the LCOV export piped below; decode it as UTF-8 (#1648).
# Without a console (a detached process) setting it throws; the default is kept then.
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try { [Console]::OutputEncoding = $OutputEncoding } catch { Write-Verbose "Console encoding unchanged: $_" }

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
if (-not $LlvmCov) {
    Write-Error "llvm-cov not found. Set LLVM_ROOT or add LLVM bin to PATH."
    exit 1
}

# Structural exclusions (#2039): lines no test can run on Windows, such as a modal shell dialog, the real
# ShellExecuteW launch or the WinMain entry point. The source marks them LCOV_EXCL_START ... LCOV_EXCL_STOP
# (inclusive) or LCOV_EXCL_LINE, each with its reason. llvm-cov has no such markers, so the LCOV report
# drops them here; the HTML report and the llvm-cov summary still count them. Only Windows-only code
# (src/Platform/Windows/ and _WIN32 blocks) is marked, and tools/coverage.sh honours no markers.

# The 1-based line numbers of @SourcePath inside a marked region.
function Get-ExcludedLine([string]$SourcePath) {
    $excluded = [System.Collections.Generic.HashSet[int]]::new()
    if (-not [System.IO.File]::Exists($SourcePath)) {
        return , $excluded
    }
    $lines = [System.IO.File]::ReadAllLines($SourcePath)
    $inside = $false
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $text = $lines[$i]
        if ($text.Contains("LCOV_EXCL_START")) {
            $inside = $true
        }
        if ($inside -or $text.Contains("LCOV_EXCL_LINE")) {
            [void]$excluded.Add($i + 1)
        }
        if ($text.Contains("LCOV_EXCL_STOP")) {
            $inside = $false
        }
    }
    if ($inside) {
        throw "${SourcePath}: LCOV_EXCL_START without a matching LCOV_EXCL_STOP"
    }
    return , $excluded
}

# Rewrite the LCOV file at @LcovPath without the marked lines, their branches and the functions that start
# on them. Each record's LF/LH, BRF/BRH and FNF/FNH lose what was dropped; they are llvm-cov's own counts,
# not a count of the records, so they are reduced rather than recounted. Returns how many line records it dropped.
function Remove-ExcludedLcovLine([string]$LcovPath) {
    $output = [System.Collections.Generic.List[string]]::new()
    $excluded = $null
    $droppedFunctions = $null
    $dropped = @{}
    $droppedLines = 0
    foreach ($record in [System.IO.File]::ReadAllLines($LcovPath)) {
        if ($record.StartsWith("SF:")) {
            $excluded = Get-ExcludedLine $record.Substring(3)
            $droppedFunctions = [System.Collections.Generic.HashSet[string]]::new()
            $dropped = @{ LF = 0; LH = 0; BRF = 0; BRH = 0; FNF = 0; FNH = 0 }
            $output.Add($record)
            continue
        }
        if ($null -eq $excluded -or $excluded.Count -eq 0) {
            $output.Add($record)
            continue
        }
        # A switch's `continue` would leave only the switch, so each case says whether to keep the record.
        $tag, $value = $record -split ":", 2
        $keep = $true
        switch ($tag) {
            "FN" {
                $line, $name = $value -split ",", 2
                if ($excluded.Contains([int]$line)) {
                    [void]$droppedFunctions.Add($name)
                    $keep = $false
                }
            }
            "FNDA" {
                $hits, $name = $value -split ",", 2
                if ($droppedFunctions.Contains($name)) {
                    $dropped.FNF++
                    if ([long]$hits -gt 0) { $dropped.FNH++ }
                    $keep = $false
                }
            }
            "BRDA" {
                $fields = $value -split ","
                if ($excluded.Contains([int]$fields[0])) {
                    $dropped.BRF++
                    if ($fields[3] -ne "-" -and [long]$fields[3] -gt 0) { $dropped.BRH++ }
                    $keep = $false
                }
            }
            "DA" {
                $fields = $value -split ","
                if ($excluded.Contains([int]$fields[0])) {
                    $droppedLines++
                    $dropped.LF++
                    if ([long]$fields[1] -gt 0) { $dropped.LH++ }
                    $keep = $false
                }
            }
            { $_ -in "LF", "LH", "BRF", "BRH", "FNF", "FNH" } {
                $output.Add("${tag}:$([long]$value - $dropped[$tag])")
                $keep = $false
            }
        }
        if ($keep) {
            $output.Add($record)
        }
    }
    [System.IO.File]::WriteAllLines($LcovPath, $output, [System.Text.UTF8Encoding]::new($false))
    return $droppedLines
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
$failedSuites = [System.Collections.Generic.List[string]]::new()
Push-Location $BuildDir
try {
    Remove-Item -Path "*.profraw", "default.profdata" -ErrorAction SilentlyContinue

    # Set profraw output location
    $env:LLVM_PROFILE_FILE = "$BuildDir\coverage-%p.profraw"

    # Run the test executables directly. TaskSmackThemeTests links the real UI/Theme.cpp, which
    # TaskSmackTests replaces with a stub (#1547); TaskSmackAppTests is built from the app's own
    # objects, for the layers that need the whole app on a display (#1395). Keep in sync with
    # tools/coverage.sh. A failing suite still gets its coverage reported, so the failure can be
    # diagnosed from the report, but the script exits non-zero at the end (#2251).
    #
    # The UI scenarios (#2711) run on a synthetic machine: tests/CMakeLists.txt gives UiScenarioTest
    # TASKSMACK_SYNTHETIC, which the app reads once per process, so they get a run of their own here with
    # the same value, as under CTest, and the rest of TaskSmackAppTests runs without it (#2927).
    $syntheticMatch = Select-String -Path (Join-Path $ProjectRoot "tests\CMakeLists.txt") `
        -Pattern 'UiScenarioTest ENVIRONMENT "TASKSMACK_SYNTHETIC=([^"]*)"' | Select-Object -First 1
    if ($null -eq $syntheticMatch) {
        Write-Error "No UiScenarioTest TASKSMACK_SYNTHETIC in tests/CMakeLists.txt; keep this script in step with it."
        exit 1
    }
    $uiScenarioSynthetic = $syntheticMatch.Matches[0].Groups[1].Value
    # Each run: the suite, its gtest filter and its TASKSMACK_SYNTHETIC (either may be empty).
    $runs = @(
        @{ Suite = "TaskSmackTests"; Filter = ""; Synthetic = "" },
        @{ Suite = "TaskSmackThemeTests"; Filter = ""; Synthetic = "" },
        @{ Suite = "TaskSmackAppTests"; Filter = "-UiScenarioTest.*"; Synthetic = "" },
        @{ Suite = "TaskSmackAppTests"; Filter = "UiScenarioTest.*"; Synthetic = $uiScenarioSynthetic }
    )
    foreach ($run in $runs) {
        $suite = $run.Suite
        $label = if ($run.Filter) { "$suite ($($run.Filter))" } else { $suite }
        # Built up, not returned from an if: PowerShell unwraps a one-element array returned from an
        # expression into a string, and splatting that passed no filter at all (#2927).
        $arguments = [System.Collections.Generic.List[string]]::new()
        if ($run.Filter) { $arguments.Add("--gtest_filter=$($run.Filter)") }
        if ($run.Synthetic) { $env:TASKSMACK_SYNTHETIC = $run.Synthetic }
        try {
            & "$BuildDir\tests\$suite.exe" $arguments.ToArray()
            $suiteExit = $LASTEXITCODE
        }
        finally {
            Remove-Item Env:TASKSMACK_SYNTHETIC -ErrorAction SilentlyContinue
        }
        if ($suiteExit -ne 0) {
            Write-Warning "$label failed (exit code $suiteExit); generating the coverage report, then failing."
            $failedSuites.Add($label)
        }
    }

    # Step 3: Merge profraw files
    Write-Host "==> Merging coverage data..."
    $profrawFiles = Get-ChildItem -Path $BuildDir -Filter "*.profraw" | Select-Object -ExpandProperty FullName
    & $LlvmProfdata merge -sparse $profrawFiles -o "$BuildDir\default.profdata"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # Files excluded from coverage: generated/third-party paths and test sources only (benchmarks/
    # holds test-support headers the tests include, #1395). ImGui panel
    # code is deliberately NOT excluded (#1131): untested code has to count against the total.
    # Keep in sync with tools/coverage.sh COV_IGNORE_REGEX.
    $IgnoreRegex = ".*(\\|/)(build|_deps|tests|benchmarks|\.cache)(\\|/).*"

    # llvm-cov only reports files compiled into the binaries it is given, so with the test binary
    # alone everything never linked into TaskSmackTests (panels, layers, Theme.cpp, main.cpp, ...)
    # drops out of the denominator (#1131). The coverage preset instruments every target, so also
    # pass the app binary as an extra -object; files the tests never execute then report 0%.
    # Keep in sync with tools/coverage.sh COV_OBJECTS.
    $AppBinary = Join-Path $BuildDir "bin\TaskSmack.exe"
    if (-not (Test-Path $AppBinary)) {
        Write-Error "$AppBinary not found; without it the report would only count files linked into TaskSmackTests."
        exit 1
    }
    $CovObjects = @("$BuildDir\tests\TaskSmackTests.exe", "-object", "$BuildDir\tests\TaskSmackThemeTests.exe", "-object", "$BuildDir\tests\TaskSmackAppTests.exe", "-object", $AppBinary)

    # Step 4: Generate HTML report
    Write-Host "==> Generating HTML report..."
    New-Item -ItemType Directory -Force -Path $CoverageDir | Out-Null

    & $LlvmCov show `
        @CovObjects `
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
        @CovObjects `
        "-instr-profile=$BuildDir\default.profdata" `
        -format=lcov `
        "-ignore-filename-regex=$IgnoreRegex" `
        | Set-Content -Path $lcovPath -Encoding utf8NoBOM
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $droppedLines = Remove-ExcludedLcovLine $lcovPath
    Write-Host "Left $droppedLines structurally untestable line(s) out of the LCOV report (LCOV_EXCL markers, #2039)."

    # Step 6: Generate summary
    Write-Host "==> Coverage Summary:"
    & $LlvmCov report `
        @CovObjects `
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

if ($failedSuites.Count -gt 0) {
    Write-Host "Test failures in: $($failedSuites -join ', '). The coverage report was still generated." -ForegroundColor Red
    exit 1
}
