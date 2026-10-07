#!/usr/bin/env pwsh
# bench.ps1 - Run TaskSmack benchmarks with consistent settings on Windows.
#
# Usage:
#   pwsh tools/bench.ps1 [preset] [-BenchmarkBinary <path>] [-OutputDirectory <dir>] [-- <extra args>]
#
# Preset defaults to 'win-benchmark'.
# Produces JSON output at perf-data/<preset>-<timestamp>.json.
#
# -BenchmarkBinary overrides build/<preset>/bin/TaskSmackBenchmarks.exe and -OutputDirectory
# overrides perf-data/ (the script tests point them at a stub and a scratch directory). The output
# file is the script's own: an extra --benchmark_out or --benchmark_out_format is refused, because
# the redaction below must find the file the benchmark wrote.
#
# Exits non-zero if the benchmark binary fails or crashes (#1423). Partial output is still redacted
# (or deleted when it cannot be parsed), so it never keeps the host name, but it is not reported as
# usable; output that cannot be redacted after a successful exit is deleted too.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Preset = "win-benchmark",

    [string]$BenchmarkBinary = "",

    [string]$OutputDirectory = "",

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
# The benchmark's exit code is read from $LASTEXITCODE. A session or profile that enables
# native-command error promotion would otherwise make a failing benchmark throw before its exit
# code is read, skipping the redaction of its partial output (#1423).
$PSNativeCommandUseErrorActionPreference = $false

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

# Normalize $Preset and $ExtraArgs BEFORE computing any paths so that the
# invocation `bench.ps1 -- --benchmark_filter=Foo` (where PowerShell binds
# "--" to the first positional parameter) correctly uses the default preset.
if ($Preset -eq "--") {
    $Preset = "win-benchmark"
}

# Allow callers to pass an explicit -- separator, e.g.:
#   bench.ps1 win-benchmark -- --benchmark_filter=Foo
if ($ExtraArgs.Count -gt 0 -and $ExtraArgs[0] -eq "--") {
    $ExtraArgs = if ($ExtraArgs.Count -gt 1) { $ExtraArgs[1..($ExtraArgs.Count - 1)] } else { @() }
}

# Google Benchmark takes the last --benchmark_out(_format), so an extra one would write somewhere
# the redaction never looks.
foreach ($arg in $ExtraArgs) {
    if ($arg -match '^--benchmark_out(_format)?(=|$)') {
        throw "'$arg' is not allowed: bench.ps1 sets the benchmark output file and format itself. Use -OutputDirectory to choose where results are written."
    }
}

$outDir = if ($OutputDirectory) { $OutputDirectory } else { Join-Path $repoRoot "perf-data" }
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outFile = Join-Path $outDir "$Preset-$timestamp.json"
$benchBin = if ($BenchmarkBinary) { $BenchmarkBinary } else { Join-Path $repoRoot "build/$Preset/bin/TaskSmackBenchmarks.exe" }

if (-not (Test-Path -LiteralPath $benchBin)) {
    Write-Error "Benchmark binary not found: $benchBin`nBuild first: cmake --build --preset $Preset"
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$benchArgs = @(
    "--benchmark_repetitions=10",
    "--benchmark_min_time=0.5s",
    "--benchmark_report_aggregates_only=true",
    "--benchmark_display_aggregates_only=true",
    "--benchmark_out=$outFile",
    "--benchmark_out_format=json"
) + $ExtraArgs

Write-Host "Running benchmarks (preset=$Preset) -> $outFile"
Write-Host "Binary: $benchBin"
Write-Host ""

& $benchBin @benchArgs
# $ErrorActionPreference does not cover a native command's exit code (#1423): read it explicitly.
$benchExit = $LASTEXITCODE
if ($null -eq $benchExit) { $benchExit = 0 }

function Invoke-ResultRedaction {
    # Redact machine-identifying context so results are safe to commit (repo convention:
    # host_name "redacted", bare executable name).
    $json = Get-Content -LiteralPath $outFile -Raw | ConvertFrom-Json
    $json.context.host_name = 'redacted'
    $json.context.executable = Split-Path $json.context.executable -Leaf
    $json | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $outFile -Encoding utf8
}

if ($benchExit -ne 0) {
    # A failed or crashed run may have left a partial JSON file holding the host name: redact it
    # if it parses, otherwise delete it, then fail.
    if (Test-Path -LiteralPath $outFile) {
        try { Invoke-ResultRedaction }
        catch {
            Remove-Item -LiteralPath $outFile -Force
            Write-Warning "Deleted unparseable partial benchmark output '$outFile' (it could not be redacted)."
        }
    }
    throw "Benchmark binary exited with code $benchExit; results in '$outFile' are not usable."
}

# Fails closed via ErrorActionPreference=Stop: if redaction cannot run, the script aborts before
# reporting the results as ready to use.
if (-not (Test-Path -LiteralPath $outFile)) {
    throw "Benchmark output '$outFile' not found; cannot redact machine-identifying context."
}
# Output that cannot be redacted (empty or truncated JSON despite exit code 0) may still hold the
# host name, so it is deleted rather than left behind.
try { Invoke-ResultRedaction }
catch {
    Remove-Item -LiteralPath $outFile -Force
    throw "Benchmark output '$outFile' could not be redacted and was deleted: $_"
}

Write-Host ""
Write-Host "Results written to: $outFile"
Write-Host "Compare two runs with Google Benchmark's compare.py:"
Write-Host "  python -m google_benchmark.compare perf-data/win-baseline.json $outFile"
