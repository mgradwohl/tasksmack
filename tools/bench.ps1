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

# The script owns the output file: Google Benchmark takes the last --benchmark_out(_format), so an
# extra one would write somewhere the redaction and the manifest never look.
foreach ($arg in $ExtraArgs) {
    if ($arg -match '^--benchmark_out(_format)?(=|$)') {
        throw "'$arg' is not allowed: bench.ps1 sets the benchmark output file and format itself. Use -OutputDirectory to choose where results are written."
    }
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
    # The same patterns as hide_absolute_paths in tools/bench-manifest.py (#1445 review):
    # - Found anywhere in a token, so after any joined switch (-IC:/x, -isystem\\host\x,
    #   /I//host/x): a drive path with either slash (not "://" as in a URL), a UNC path with either
    #   slash (not after ':'), and the \\?\ and \\.\ device paths.
    # - Found at the start, after whitespace, '=', ',' or a quote, optionally with a switch glued
    #   on (-I, -L, -B, -isystem, -idirafter, -iquote, -imsvc, any other -x/--x switch; MSVC /I,
    #   /FI, /Fo, /Fd, /Fe, /Fp, /LIBPATH:): a POSIX path of two or more segments (so MSVC defines
    #   such as /DWIN32 stay) and a home-relative ~/x or ~u/x.
    # - A path ends at whitespace, a quote, '=', ',' or ';', so both sides of
    #   -fdebug-prefix-map=OLD=NEW / -ffile-prefix-map=OLD=NEW, --sysroot=PATH, -fprofile-*=PATH
    #   and space-separated values are matched on their own.
    # - Quoted paths, which can hold spaces, are handled first, with the same heads and switches.
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
    $tail = '[^\s"''=,;]*'
    $switch = '(?:-{1,2}[A-Za-z][\w+-]*?|/(?:I|FI|Fo|Fd|Fe|Fp|LIBPATH:))'
    $anywhereHead = '(?:[A-Za-z]:[\\/](?![\\/])|\\\\|(?<!:)//)'
    $boundaryHead = '(?:/[^/\s"''=,;]+/|~[^/\s"''=,;]*/)'
    $quoted = [regex]('(?<q>["''])(?<pre>' + $switch + '?)(?<path>(?:[A-Za-z]:[\\/]|\\\\|//|/[^/"'']+/|~[^/"'']*/)[^"'']*)\k<q>')
    $bare = [regex]('(?<anywhere>' + $anywhereHead + $tail + ')|(?<pre>(?:^|(?<=[\s=,"'']))' + $switch + '?)(?<path>' + $boundaryHead + $tail + ')')
    $Flags = $quoted.Replace($Flags, { param($m) $m.Groups['q'].Value + $m.Groups['pre'].Value + (& $scrub $m.Groups['path'].Value) + $m.Groups['q'].Value })
    return $bare.Replace($Flags, {
            param($m)
            if ($m.Groups['anywhere'].Success) { return (& $scrub $m.Groups['anywhere'].Value) }
            $m.Groups['pre'].Value + (& $scrub $m.Groups['path'].Value)
        })
}

function Hide-Identity {
    # Defensive last pass over every string in the manifest (#1445 review): any home-directory
    # prefix ($HOME, $env:USERPROFILE, both slash forms) becomes <home> and the user name, as a
    # whole word, becomes <user>; MSVC defines such as /DWIN32 and everything else are left alone.
    # Kept in step with hide_identity in tools/bench-manifest.py.
    param($Value)
    if ($Value -is [System.Collections.IDictionary]) {
        $copy = [ordered]@{}
        foreach ($key in $Value.Keys) { $copy[$key] = Hide-Identity $Value[$key] }
        return $copy
    }
    if ($Value -is [string]) {
        $homes = @([Environment]::GetFolderPath('UserProfile'), $HOME, $env:HOME, $env:USERPROFILE) | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\', '/') } | Where-Object { $_.Length -gt 3 }
        $prefixes = @($homes | ForEach-Object { $_; $_.Replace('\', '/'); $_.Replace('/', '\') } | Sort-Object -Unique | Sort-Object Length -Descending)
        foreach ($prefix in $prefixes) { $Value = [regex]::Replace($Value, [regex]::Escape($prefix), '<home>', 'IgnoreCase') }
        $user = [Environment]::UserName
        if ($user -and $user.Length -ge 2) {
            $Value = [regex]::Replace($Value, '(?<![A-Za-z0-9])' + [regex]::Escape($user) + '(?![A-Za-z0-9])', '<user>', 'IgnoreCase')
        }
        return $Value
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        # A list stays a list (written as a JSON array even with one item).
        return , [object[]]@(foreach ($item in $Value) { Hide-Identity $item })
    }
    return $Value
}

function Get-CMakeCompilerFile {
    # CMakeFiles/<major.minor.patch>/CMakeCXXCompiler.cmake for the CMake version in the cache: a
    # reused build tree keeps one directory per CMake that configured it, so the first match can
    # be stale. A development CMake names it with a suffix (4.1.20250101-gabc), so a single
    # directory starting with the version and '-' is accepted too; otherwise $null (unknown,
    # rather than a guess). Kept in step with cmake_compiler_file in tools/bench-manifest.py.
    param([string]$BuildDirectory, [hashtable]$Cache)
    $parts = @('MAJOR', 'MINOR', 'PATCH') | ForEach-Object { $Cache["CMAKE_CACHE_$($_)_VERSION"] }
    if (@($parts | Where-Object { $_ }).Count -ne 3) { return $null }
    $version = $parts -join '.'
    $cmakeFiles = Join-Path $BuildDirectory 'CMakeFiles'
    $exact = Join-Path (Join-Path $cmakeFiles $version) 'CMakeCXXCompiler.cmake'
    if (Test-Path -LiteralPath $exact -PathType Leaf) { return $exact }
    $candidates = @(Get-ChildItem -LiteralPath $cmakeFiles -Directory -Filter "$version-*" -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'CMakeCXXCompiler.cmake' } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    if ($candidates.Count -eq 1) { return $candidates[0] }
    return $null
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
    $compilerFile = Get-CMakeCompilerFile -BuildDirectory $buildDir -Cache $cache
    if ($compilerFile) {
        $text = Get-Content -LiteralPath $compilerFile -Raw
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

function Test-BenchmarkTruthy {
    # Google Benchmark's IsTruthyFlagValue (src/commandlineflags.cc): one character is true when
    # alphanumeric and not 0/f/F/n/N; a longer value is true unless false/no/off (any case); an
    # empty value is true.
    param([string]$Value)
    if ($Value.Length -eq 1) { return ($Value -cmatch '^[A-Za-z0-9]$') -and ($Value -cnotmatch '^[0fFnN]$') }
    if ($Value.Length -gt 1) { return @('false', 'no', 'off') -notcontains $Value.ToLowerInvariant() }
    return $true
}

function Get-EffectiveReportAggregatesOnly {
    # The setting Google Benchmark ends up with: its default comes from the
    # BENCHMARK_REPORT_AGGREGATES_ONLY environment variable (else false), then every
    # --benchmark_report_aggregates_only[=value] argument is applied in order, the last one winning.
    # A bare flag is true. Kept in step with report_aggregates_only in tools/bench-manifest.py.
    param([string[]]$Arguments)
    $flag = '--benchmark_report_aggregates_only'
    $value = if ($null -ne $env:BENCHMARK_REPORT_AGGREGATES_ONLY) { Test-BenchmarkTruthy $env:BENCHMARK_REPORT_AGGREGATES_ONLY } else { $false }
    foreach ($arg in $Arguments) {
        if ($arg -ceq $flag) { $value = $true }
        elseif ($arg.StartsWith("$flag=", [StringComparison]::Ordinal)) { $value = Test-BenchmarkTruthy $arg.Substring($flag.Length + 1) }
    }
    return $value
}

function Write-BenchManifest {
    param([int]$ExitCode)
    # The output path is reduced to its file name, so the manifest carries no user-profile path.
    $recordedArgs = [string[]]@($benchArgs | ForEach-Object {
            if ($_ -like '--benchmark_out=*') { "--benchmark_out=$(Split-Path -Leaf $outFile)" } else { $_ }
        })
    $aggregatesOnly = Get-EffectiveReportAggregatesOnly $benchArgs
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
            raw_repetitions        = -not $aggregatesOnly
            report_aggregates_only = $aggregatesOnly
        }
        machine        = Get-MachineClass
    }
    Hide-Identity $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestFile -Encoding utf8
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
