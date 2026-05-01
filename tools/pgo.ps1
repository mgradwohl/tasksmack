# tools/pgo.ps1 – End-to-end Profile-Guided Optimization workflow for Windows
#
# Usage:
#   pwsh tools/pgo.ps1             # Full PGO workflow (generate + merge + use)
#   pwsh tools/pgo.ps1 generate    # Phase 1 only: instrumented build + run to collect data
#   pwsh tools/pgo.ps1 merge       # Phase 2 only: merge *.profraw → profiles/tasksmack.profdata
#   pwsh tools/pgo.ps1 use         # Phase 3 only: build PGO-optimized binary
#
# The resulting binary is at: build\win-pgo-use\bin\TaskSmack.exe
#
# Requirements:
#   - LLVM/Clang (LLVM_ROOT must be set, see CONTRIBUTING.md)
#   - cmake, ninja
#
# Background: Clang PGO works in four steps:
#   1. Build with -fprofile-instr-generate (instrumented binary that records branch counts)
#   2. Run the instrumented binary; LLVM_PROFILE_FILE controls output filename
#   3. Merge the per-run .profraw files into a single .profdata with llvm-profdata
#   4. Build again with -fprofile-instr-use=<path>.profdata for an optimized binary

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $ScriptDir
$ProfilesDir = Join-Path $Root 'profiles'
$ProfrawPattern = Join-Path $ProfilesDir 'tasksmack-%p.profraw'
$Profdata = Join-Path $ProfilesDir 'tasksmack.profdata'
$BenchBin = Join-Path $Root 'build\win-pgo-generate\bin\TaskSmackBenchmarks.exe'
$AppBin = Join-Path $Root 'build\win-pgo-generate\bin\TaskSmack.exe'

# ── helpers ──────────────────────────────────────────────────────────────────

function Write-Step([string]$message) {
    Write-Host ''
    Write-Host '──────────────────────────────────────────'
    Write-Host "  $message"
    Write-Host '──────────────────────────────────────────'
}

# Invoke a native executable and abort if it returns a non-zero exit code.
# $ErrorActionPreference = 'Stop' does not cover native executables in PowerShell,
# so each call must be checked explicitly.
# $args[0] is the executable; remaining elements are its arguments (splatted).
function Invoke-Native {
    $exe  = $args[0]
    $rest = if ($args.Count -gt 1) { $args[1..($args.Count - 1)] } else { @() }
    # Flatten nested arrays so array-valued arguments (e.g. a list of .profraw paths)
    # are expanded into individual arguments for the native process rather than being
    # passed as a single "System.Object[]" string.
    $flatRest = [string[]]@($rest | ForEach-Object {
        if ($_ -is [array]) { $_ | ForEach-Object { [string]$_ } } else { [string]$_ }
    })
    & $exe @flatRest
    if ($LASTEXITCODE -ne 0) {
        # Flatten nested arrays to strings so the error message shows the actual
        # command line rather than "System.Object[]" for array-valued arguments.
        $cmdLine = ($args | ForEach-Object { if ($_ -is [array]) { $_ -join ' ' } else { [string]$_ } }) -join ' '
        Write-Error "Command failed with exit code ${LASTEXITCODE}: $cmdLine"
        exit $LASTEXITCODE
    }
}

# Resolve llvm-profdata: prefer $env:LLVM_ROOT\bin (same as tools/coverage.ps1),
# then fall back to PATH.
$LlvmProfdata = $null
if ($env:LLVM_ROOT) {
    $candidate = Join-Path $env:LLVM_ROOT 'bin\llvm-profdata.exe'
    if (Test-Path $candidate) { $LlvmProfdata = $candidate }
}
if (-not $LlvmProfdata) {
    $cmd = Get-Command 'llvm-profdata' -ErrorAction SilentlyContinue
    if ($cmd) { $LlvmProfdata = $cmd.Source }
}
if (-not $LlvmProfdata) {
    Write-Error "llvm-profdata not found. Set LLVM_ROOT or add LLVM bin to PATH."
    exit 1
}

# ── phase 1: instrumented build and profiling run ─────────────────────────────

function Invoke-Generate {
    Write-Step 'Phase 1 – Instrumented build (win-pgo-generate preset)'

    Invoke-Native cmake --preset win-pgo-generate -S $Root
    Invoke-Native cmake --build --preset win-pgo-generate

    if (-not (Test-Path $ProfilesDir)) {
        New-Item -ItemType Directory -Path $ProfilesDir | Out-Null
    }

    # Remove stale profraw files so the merge step is deterministic
    Get-ChildItem -Path $ProfilesDir -Filter '*.profraw' -ErrorAction SilentlyContinue | Remove-Item -Force

    Write-Step 'Phase 1 – Running benchmarks to collect profile data'

    if (-not (Test-Path $BenchBin)) {
        Write-Error "Benchmark binary not found: $BenchBin"
        exit 1
    }

    # Run all benchmarks; LLVM_PROFILE_FILE drives per-process .profraw output.
    # %p expands to the PID so parallel processes don't clobber each other.
    # Save and restore any pre-existing LLVM_PROFILE_FILE so the user's environment
    # is not permanently modified if one was already set before this script ran.
    $prevLlvmProfileFile = $env:LLVM_PROFILE_FILE
    $env:LLVM_PROFILE_FILE = $ProfrawPattern
    try {
        Invoke-Native $BenchBin --benchmark_min_time=0.5
    }
    finally {
        if ($null -eq $prevLlvmProfileFile) {
            Remove-Item Env:\LLVM_PROFILE_FILE -ErrorAction SilentlyContinue
        } else {
            $env:LLVM_PROFILE_FILE = $prevLlvmProfileFile
        }
    }

    if (Test-Path $AppBin) {
        Write-Host ''
        Write-Host 'Tip: You can run the main application to capture additional UI profile data:'
        Write-Host "  `$env:LLVM_PROFILE_FILE = '$ProfrawPattern'"
        Write-Host "  & '$AppBin'"
        Write-Host '  (Use it for a few seconds, then exit.)'
    }

    Write-Host ''
    Write-Host "Profile data written to: $ProfilesDir\*.profraw"
}

# ── phase 2: merge ────────────────────────────────────────────────────────────

function Invoke-Merge {
    Write-Step "Phase 2 – Merging profraw files → $Profdata"

    $profrawFiles = @(Get-ChildItem -Path $ProfilesDir -Filter '*.profraw' -ErrorAction SilentlyContinue)

    if (-not $profrawFiles -or $profrawFiles.Count -eq 0) {
        Write-Error "No .profraw files found in $ProfilesDir. Run phase 1 first (pwsh tools/pgo.ps1 generate)."
        exit 1
    }

    Write-Host "Merging $($profrawFiles.Count) .profraw file(s)..."
    Invoke-Native $LlvmProfdata merge -sparse ($profrawFiles | ForEach-Object { $_.FullName }) -o $Profdata

    $size = (Get-Item $Profdata).Length / 1KB
    Write-Host "Profile data merged: $Profdata"
    Write-Host "  Size: $([Math]::Round($size, 1)) KB"
}

# ── phase 3: PGO-optimized build ──────────────────────────────────────────────

function Invoke-Use {
    Write-Step 'Phase 3 – PGO-optimized build (win-pgo-use preset)'

    if (-not (Test-Path $Profdata)) {
        Write-Error "Profile data not found: $Profdata. Run merge step first (pwsh tools/pgo.ps1 merge)."
        exit 1
    }

    Invoke-Native cmake --preset win-pgo-use -S $Root
    Invoke-Native cmake --build --preset win-pgo-use

    $finalBin = Join-Path $Root 'build\win-pgo-use\bin\TaskSmack.exe'
    Write-Host ''
    Write-Host "PGO-optimized binary: $finalBin"

    if (Test-Path $finalBin) {
        $size = (Get-Item $finalBin).Length / 1KB
        Write-Host "Binary size: $([Math]::Round($size, 1)) KB"
    }
}

# ── main ──────────────────────────────────────────────────────────────────────

$cmd = if ($args.Count -gt 0) { $args[0] } else { 'all' }

switch ($cmd) {
    'generate' { Invoke-Generate }
    'merge'    { Invoke-Merge }
    'use'      { Invoke-Use }
    'all' {
        Invoke-Generate
        Invoke-Merge
        Invoke-Use
        Write-Step 'PGO workflow complete'
        Write-Host "  Instrumented binary : build\win-pgo-generate\bin\TaskSmack.exe"
        Write-Host "  Profile data        : profiles\tasksmack.profdata"
        Write-Host "  Optimized binary    : build\win-pgo-use\bin\TaskSmack.exe"
    }
    default {
        Write-Host 'Usage: pwsh tools/pgo.ps1 [generate|merge|use|all]'
        Write-Host '  generate  – instrumented build + collect profile data'
        Write-Host '  merge     – merge *.profraw files into tasksmack.profdata'
        Write-Host '  use       – build PGO-optimized binary from tasksmack.profdata'
        Write-Host '  all       – run all three phases in order (default)'
        exit 1
    }
}
