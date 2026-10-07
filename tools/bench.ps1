#!/usr/bin/env pwsh
# bench.ps1 - Run TaskSmack benchmarks with consistent settings on Windows.
#
# Usage:
#   pwsh tools/bench.ps1 [preset] [-BenchmarkBinary <path>] [-OutputDirectory <dir>] [-- <extra args>]
#
# Preset defaults to 'win-benchmark'.
# Produces JSON output at perf-data/<preset>-<timestamp>.json, holding every repetition plus the
# mean/median/stddev/cv aggregates, and a provenance sidecar at
# perf-data/<preset>-<timestamp>.manifest.json (git state, binary SHA-256, build config, benchmark
# args, anonymized machine class; see CONTRIBUTING.md "Benchmark Output").
#
# -BenchmarkBinary overrides build/<preset>/bin/TaskSmackBenchmarks.exe (the script tests point
# it at a stub). -OutputDirectory overrides perf-data/.
#
# Exits non-zero if the benchmark binary fails or crashes; any partial output is still redacted
# (or deleted when it cannot be) and the manifest records the exit code.

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
# Native exit codes are read from $LASTEXITCODE (the benchmark's and git's). A session or profile
# that enables native-command error promotion would otherwise make a failing benchmark throw before
# its exit code is captured, skipping the manifest and the redaction of its partial output (#1423).
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

$outDir = if ($OutputDirectory) { $OutputDirectory } else { Join-Path $repoRoot "perf-data" }
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outFile = Join-Path $outDir "$Preset-$timestamp.json"
$manifestFile = Join-Path $outDir "$Preset-$timestamp.manifest.json"
$benchBin = if ($BenchmarkBinary) { $BenchmarkBinary } else { Join-Path $repoRoot "build/$Preset/bin/TaskSmackBenchmarks.exe" }

if (-not (Test-Path -LiteralPath $benchBin)) {
    Write-Error "Benchmark binary not found: $benchBin`nBuild first: cmake --build --preset $Preset"
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# Every repetition is kept in the JSON file (no --benchmark_report_aggregates_only), so the
# distribution can be re-analysed; Google Benchmark still appends the aggregate rows, which
# tools/check-benchmark-regression.py compares. Only the console shows aggregates alone.
$benchArgs = @(
    "--benchmark_repetitions=10",
    "--benchmark_min_time=0.5s",
    "--benchmark_display_aggregates_only=true",
    "--benchmark_out=$outFile",
    "--benchmark_out_format=json"
) + $ExtraArgs

function Get-GitProvenance {
    $git = [ordered]@{ commit = $null; branch = $null; dirty = $null }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return $git }
    $commit = & git -C $repoRoot rev-parse HEAD 2>$null
    if ($LASTEXITCODE -ne 0) { return $git }
    $git.commit = "$commit".Trim()
    $branch = & git -C $repoRoot rev-parse --abbrev-ref HEAD 2>$null
    if ($LASTEXITCODE -eq 0) { $git.branch = "$branch".Trim() }
    # Tracked changes only: untracked scratch files do not change what was built.
    $status = & git -C $repoRoot status --porcelain --untracked-files=no 2>$null
    if ($LASTEXITCODE -eq 0) { $git.dirty = [bool]("$status".Trim()) }
    return $git
}

function Hide-AbsolutePaths {
    # Replace absolute paths in compiler flags so no user profile or checkout path is recorded: a
    # path inside the source tree becomes <source>/relative/path, any other <abs>/<file name>.
    # Matches a drive or UNC path, or a POSIX path of two or more segments (so MSVC-style switches
    # such as /DWIN32 are left alone), after the start, whitespace, '=' or ',', optionally with a
    # one-letter switch glued on (-I/x, -LC:/x); quoted paths, which can hold spaces, first.
    # Kept in step with hide_absolute_paths in tools/bench-manifest.py.
    param([string]$Flags)
    if (-not $Flags) { return $Flags }
    $root = ([IO.Path]::GetFullPath($repoRoot)).Replace('\', '/').TrimEnd('/')
    $scrub = {
        param([string]$Path)
        $normalized = $Path.Replace('\', '/')
        $fold = $IsWindows -or $normalized -match '^[A-Za-z]:/'
        $comparison = if ($fold) { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
        if ($normalized.Equals($root, $comparison) -or $normalized.StartsWith("$root/", $comparison)) {
            return '<source>' + $normalized.Substring($root.Length)
        }
        return '<abs>/' + ($normalized.TrimEnd('/') -split '/')[-1]
    }
    $quoted = [regex]'(["''])((?:[A-Za-z]:[\\/]|\\\\|/)[^"'']*)\1'
    $bare = [regex]'(?<pre>(?:^|[\s=,])(?:-[A-Za-z])?)(?<path>(?:[A-Za-z]:[\\/]|\\\\)[^\s"'']*|/[^/\s"'']+/[^\s"'']*)'
    $Flags = $quoted.Replace($Flags, { param($m) $m.Groups[1].Value + (& $scrub $m.Groups[2].Value) + $m.Groups[1].Value })
    return $bare.Replace($Flags, { param($m) $m.Groups['pre'].Value + (& $scrub $m.Groups['path'].Value) })
}

function Get-BuildProvenance {
    # The build tree is the binary's bin/ parent (build/<preset>); read from its CMakeCache.txt.
    # Only the compiler's file name is kept: its full path can sit under a user profile.
    $build = [ordered]@{
        build_type       = $null
        generator        = $null
        compiler         = $null
        compiler_id      = $null
        compiler_version = $null
        cxx_flags        = $null
        cxx_flags_config = $null
        ipo              = $null
    }
    $buildDir = Split-Path -Parent (Split-Path -Parent ([IO.Path]::GetFullPath($benchBin)))
    $cachePath = Join-Path $buildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath)) { return $build }
    $cache = @{}
    foreach ($line in Get-Content -LiteralPath $cachePath) {
        if ($line -match '^(?<name>[A-Za-z0-9_]+)(:[A-Za-z]+)?=(?<value>.*)$') { $cache[$Matches.name] = $Matches.value }
    }
    $build.build_type = $cache['CMAKE_BUILD_TYPE']
    $build.generator = $cache['CMAKE_GENERATOR']
    if ($cache['CMAKE_CXX_COMPILER']) { $build.compiler = Split-Path -Leaf $cache['CMAKE_CXX_COMPILER'] }
    # Flags can embed absolute paths (the PGO presets' -fprofile-instr-use=${sourceDir}/...).
    $build.cxx_flags = Hide-AbsolutePaths $cache['CMAKE_CXX_FLAGS']
    if ($build.build_type) { $build.cxx_flags_config = Hide-AbsolutePaths $cache["CMAKE_CXX_FLAGS_$($build.build_type.ToUpperInvariant())"] }
    $build.ipo = if ($cache.ContainsKey('CMAKE_INTERPROCEDURAL_OPTIMIZATION')) { $cache['CMAKE_INTERPROCEDURAL_OPTIMIZATION'] }
    elseif ($cache.ContainsKey('TASKSMACK_ENABLE_IPO')) { $cache['TASKSMACK_ENABLE_IPO'] }
    else { $null }
    $compilerFile = Get-ChildItem -Path (Join-Path $buildDir 'CMakeFiles') -Filter 'CMakeCXXCompiler.cmake' -Recurse -Depth 1 -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($compilerFile) {
        $text = Get-Content -LiteralPath $compilerFile.FullName -Raw
        if ($text -match 'set\(CMAKE_CXX_COMPILER_ID "(?<v>[^"]*)"\)') { $build.compiler_id = $Matches.v }
        if ($text -match 'set\(CMAKE_CXX_COMPILER_VERSION "(?<v>[^"]*)"\)') { $build.compiler_version = $Matches.v }
    }
    return $build
}

function Get-MachineClass {
    # An anonymized machine class: CPU model, logical cores and OS. Never the host name, user
    # name or other processes.
    $cpu = $null
    if ($IsWindows) {
        $cpu = (Get-ItemProperty -Path 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0' -Name ProcessorNameString -ErrorAction SilentlyContinue).ProcessorNameString
    }
    elseif (Test-Path -LiteralPath '/proc/cpuinfo') {
        $line = Select-String -LiteralPath '/proc/cpuinfo' -Pattern '^model name\s*:\s*(.*)$' | Select-Object -First 1
        if ($line) { $cpu = $line.Matches[0].Groups[1].Value }
    }
    if ($cpu) { $cpu = ($cpu -replace '\s+', ' ').Trim() }
    $osName = if ($IsWindows) { 'Windows' } elseif ($IsLinux) { 'Linux' } elseif ($IsMacOS) { 'Darwin' } else { 'unknown' }
    $osVersion = [Environment]::OSVersion.Version.ToString()
    $cores = [Environment]::ProcessorCount
    $arch = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    return [ordered]@{
        label         = "$(if ($cpu) { $cpu } else { 'unknown CPU' }) / $cores logical cores / $osName $osVersion"
        cpu_model     = $cpu
        logical_cores = $cores
        os_name       = $osName
        os_version    = $osVersion
        arch          = $arch
    }
}

function Write-BenchManifest {
    param([int]$ExitCode)
    # The output path is reduced to its file name, so the manifest carries no user-profile path.
    $recordedArgs = [string[]]@($benchArgs | ForEach-Object {
            if ($_ -like '--benchmark_out=*') { "--benchmark_out=$(Split-Path -Leaf $outFile)" } else { $_ }
        })
    $manifest = [ordered]@{
        schema_version = 1
        generator      = 'tools/bench.ps1'
        created_utc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        preset         = $Preset
        result_file    = Split-Path -Leaf $outFile
        exit_code      = $ExitCode
        git            = Get-GitProvenance
        binary         = [ordered]@{
            name   = Split-Path -Leaf $benchBin
            sha256 = (Get-FileHash -LiteralPath $benchBin -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        build          = Get-BuildProvenance
        benchmark      = [ordered]@{
            args                   = $recordedArgs
            raw_repetitions        = -not ($recordedArgs -contains '--benchmark_report_aggregates_only=true')
            report_aggregates_only = $recordedArgs -contains '--benchmark_report_aggregates_only=true'
        }
        machine        = Get-MachineClass
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestFile -Encoding utf8
}

function Invoke-ResultRedaction {
    # Redact machine-identifying context so results are safe to commit (repo convention:
    # host_name "redacted", bare executable name).
    $json = Get-Content -LiteralPath $outFile -Raw | ConvertFrom-Json
    $json.context.host_name = 'redacted'
    $json.context.executable = Split-Path $json.context.executable -Leaf
    # -Depth 100: per-repetition rows and user counters nest deeper than a flat aggregate file.
    $json | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $outFile -Encoding utf8
}

Write-Host "Running benchmarks (preset=$Preset) -> $outFile"
Write-Host "Binary: $benchBin"
Write-Host ""

& $benchBin @benchArgs
# $ErrorActionPreference does not cover a native command's exit code (#1423): read it explicitly.
$benchExit = $LASTEXITCODE
if ($null -eq $benchExit) { $benchExit = 0 }

# The manifest is written before the result is redacted, so the result file stays the newest one
# in the output directory (heavy-checks.yml picks the latest perf-data/benchmark-*.json).
# A manifest failure is held until the result has been redacted, then fails the script.
$manifestError = $null
try { Write-BenchManifest -ExitCode $benchExit }
catch { $manifestError = $_ }

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
    throw "Benchmark binary exited with code $benchExit; results in '$outFile' are not usable. Manifest: $manifestFile"
}

# Fails closed via ErrorActionPreference=Stop: if redaction cannot run, the script aborts before
# reporting the results as ready to use.
if (-not (Test-Path -LiteralPath $outFile)) {
    throw "Benchmark output '$outFile' not found; cannot redact machine-identifying context."
}
Invoke-ResultRedaction

if ($manifestError) {
    throw "Writing the provenance manifest '$manifestFile' failed: $manifestError"
}

Write-Host ""
Write-Host "Results written to: $outFile"
Write-Host "Provenance manifest: $manifestFile"
Write-Host "Compare two runs with Google Benchmark's compare.py:"
Write-Host "  python -m google_benchmark.compare perf-data/win-baseline.json $outFile"
