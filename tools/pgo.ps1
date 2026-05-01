# tools/pgo.ps1 – End-to-end Profile-Guided Optimization workflow for Windows
#
# Usage:
#   pwsh tools/pgo.ps1             # Full PGO workflow (generate + merge + use)
#   pwsh tools/pgo.ps1 generate    # Step 1 only: instrumented build + run to collect data
#   pwsh tools/pgo.ps1 merge       # Step 2 only: merge *.profraw → profiles/tasksmack.profdata
#   pwsh tools/pgo.ps1 use         # Step 3 only: build PGO-optimized binary
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

function Require-Cmd([string]$cmd) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
        Write-Error "'$cmd' not found. Ensure LLVM is installed and LLVM_ROOT/bin is on PATH."
        exit 1
    }
}

# ── phase 1: instrumented build and profiling run ─────────────────────────────

function Invoke-Generate {
    Write-Step 'Phase 1 – Instrumented build (win-pgo-generate preset)'

    cmake --preset win-pgo-generate -S $Root
    cmake --build --preset win-pgo-generate

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
    $env:LLVM_PROFILE_FILE = $ProfrawPattern
    & $BenchBin --benchmark_min_time=0.5
    Remove-Item Env:\LLVM_PROFILE_FILE -ErrorAction SilentlyContinue

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

    Require-Cmd 'llvm-profdata'

    $profrawFiles = @(Get-ChildItem -Path $ProfilesDir -Filter '*.profraw' -ErrorAction SilentlyContinue)

    if (-not $profrawFiles -or $profrawFiles.Count -eq 0) {
        Write-Error "No .profraw files found in $ProfilesDir. Run phase 1 first (pwsh tools/pgo.ps1 generate)."
        exit 1
    }

    Write-Host "Merging $($profrawFiles.Count) .profraw file(s)..."
    & llvm-profdata merge -sparse ($profrawFiles | ForEach-Object { $_.FullName }) -o $Profdata

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

    cmake --preset win-pgo-use -S $Root
    cmake --build --preset win-pgo-use

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
