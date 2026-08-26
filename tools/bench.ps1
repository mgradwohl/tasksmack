#!/usr/bin/env pwsh
# bench.ps1 - Run TaskSmack benchmarks with consistent settings on Windows.
#
# Usage:
#   pwsh tools/bench.ps1 [preset] [-- <extra args>]
#
# Preset defaults to 'win-benchmark'.
# Produces JSON output at perf-data/<preset>-<timestamp>.json.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Preset = "win-benchmark",

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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

$outDir = Join-Path $repoRoot "perf-data"
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outFile = Join-Path $outDir "$Preset-$timestamp.json"
$benchBin = Join-Path $repoRoot "build/$Preset/bin/TaskSmackBenchmarks.exe"

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

# Redact machine-identifying context so results are safe to commit (repo convention:
# host_name "redacted", bare executable name).
if (Test-Path $outFile) {
    $json = Get-Content $outFile -Raw | ConvertFrom-Json
    if ($json.context) {
        $json.context.host_name = 'redacted'
        $json.context.executable = Split-Path $json.context.executable -Leaf
        $json | ConvertTo-Json -Depth 10 | Set-Content $outFile -Encoding utf8
    }
}

Write-Host ""
Write-Host "Results written to: $outFile"
Write-Host "Compare two runs with Google Benchmark's compare.py:"
Write-Host "  python -m google_benchmark.compare perf-data/win-baseline.json $outFile"
