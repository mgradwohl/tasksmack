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

# Absolute paths inside compiler flags (#1445 review). The flag string is split into arguments
# and each argument is scrubbed on its own, step for step as hide_absolute_paths in
# tools/bench-manifest.py does, with the same switch lists:
#  1. Split like a shell, but with no backslash escapes (Windows paths keep their backslashes):
#     whitespace separates arguments, and "..." or '...' quotes a span that may hold spaces. Each
#     argument remembers its first quote character and where that quote opened.
#  2. Peel the switch: a prefix-map switch (its OLD=NEW value is split at the first '=', as
#     clang does, and each side scrubbed on its own); a comma-list switch (-Wl, and friends:
#     each item scrubbed); a generic "-opt=" / "--opt=" (the value after the first '='); or a
#     joined switch (-I, -isystem, /I, ...) when what follows it is a path. Otherwise the whole
#     argument is the operand.
#  3. An operand is a path when it starts with a drive (C:\ or C:/), a UNC or device path (\\,
#     //, \\?\, \\.\), a POSIX path of two or more segments (/home/u/x; so /DWIN32 stays) or ~.
#     A path inside the checkout becomes <source>/relative, any other <abs>/<file name>. A ';'
#     list is scrubbed item by item, and an operand with a drive, UNC or device path inside it
#     (FOO:C:/x) is scrubbed from there.
#  4. Re-join with single spaces, putting each argument's quote back before the piece it opened
#     on (or around the whole argument when that piece no longer exists).
$script:PrefixMapSwitches = @('-ffile-prefix-map=', '-fdebug-prefix-map=', '-fmacro-prefix-map=', '-fprofile-prefix-map=')
$script:ListSwitches = @('-Wl,', '-Wa,', '-Wp,')
# Longest first, so -isystem is not read as -I... (case matters: -I is not -i).
$script:JoinedSwitches = @('-iwithprefixbefore', '-iwithprefix', '-idirafter', '/LIBPATH:', '-isysroot', '-iprefix', '-imacros',
    '-isystem', '-include', '-iquote', '-imsvc', '/FI', '/Fo', '/Fd', '/Fe', '/Fp', '-I', '-L', '-B', '-F', '/I')
$script:PathHead = [regex]'^(?:[A-Za-z]:[\\/]|\\\\|//|/[^/\\]+/|~[^/\\]*(?:[/\\]|$))'
$script:EmbeddedHead = [regex]'[A-Za-z]:[\\/](?![\\/])|\\\\|(?<!:)//'

function Split-FlagArguments {
    # Split a flag string into arguments: Text (unquoted), Quote (first quote char or $null) and
    # QuoteStart (the offset in Text where that quote opened).
    param([string]$Flags)
    $arguments = [System.Collections.Generic.List[object]]::new()
    $index = 0
    $length = $Flags.Length
    while ($index -lt $length) {
        if ([char]::IsWhiteSpace($Flags[$index])) { $index++; continue }
        $text = [System.Text.StringBuilder]::new()
        $quote = $null
        $quoteStart = 0
        while ($index -lt $length -and -not [char]::IsWhiteSpace($Flags[$index])) {
            $char = $Flags[$index]
            if ($char -eq [char]'"' -or $char -eq [char]"'") {
                $close = $Flags.IndexOf($char, $index + 1)
                if ($close -lt 0) { $close = $length }
                if ($null -eq $quote) { $quote = [string]$char; $quoteStart = $text.Length }
                [void]$text.Append($Flags.Substring($index + 1, $close - $index - 1))
                $index = $close + 1
            }
            else {
                [void]$text.Append($char)
                $index++
            }
        }
        $arguments.Add([pscustomobject]@{ Text = $text.ToString(); Quote = $quote; QuoteStart = $quoteStart })
    }
    return , $arguments
}

function Hide-AbsolutePaths {
    # Replace absolute paths in compiler flags so no user profile or checkout path is recorded: a
    # path inside the source tree becomes <source>/relative/path, any other <abs>/<file name>.
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
    $operand = {
        param([string]$Value)
        if ($Value.Contains(';')) { return (@($Value.Split(';') | ForEach-Object { & $operand $_ }) -join ';') }
        if ($script:PathHead.IsMatch($Value)) { return (& $scrub $Value) }
        $embedded = $script:EmbeddedHead.Match($Value)
        if ($embedded.Success) { return $Value.Substring(0, $embedded.Index) + (& $scrub $Value.Substring($embedded.Index)) }
        return $Value
    }
    $pieces = {
        # (offset in the argument, scrubbed text) for each part of the argument.
        param([string]$Argument)
        foreach ($switch in $script:PrefixMapSwitches) {
            if ($Argument.StartsWith($switch, [StringComparison]::Ordinal)) {
                $value = $Argument.Substring($switch.Length)
                $equals = $value.IndexOf('=')
                if ($equals -lt 0) { return , @(, @(0, $switch), @($switch.Length, (& $operand $value))) }
                $old = $value.Substring(0, $equals)
                $new = $value.Substring($equals + 1)
                return , @(@(0, $switch), @($switch.Length, (& $operand $old)), @(($switch.Length + $old.Length), '='), @(($switch.Length + $old.Length + 1), (& $operand $new)))
            }
        }
        foreach ($switch in $script:ListSwitches) {
            if ($Argument.StartsWith($switch, [StringComparison]::Ordinal)) {
                $result = [System.Collections.Generic.List[object]]::new()
                $result.Add(@(0, $switch))
                $offset = $switch.Length
                $position = 0
                foreach ($item in $Argument.Substring($switch.Length).Split(',')) {
                    if ($position -gt 0) { $result.Add(@($offset, ',')); $offset++ }
                    $result.Add(@($offset, (& $operand $item)))
                    $offset += $item.Length
                    $position++
                }
                return , $result.ToArray()
            }
        }
        if (($Argument.StartsWith('-') -or $Argument.StartsWith('/')) -and $Argument.Contains('=')) {
            $head = $Argument.Substring(0, $Argument.IndexOf('=') + 1)
            if ($head.Substring(1) -notmatch '[\\/]') {
                return , @(@(0, $head), @($head.Length, (& $operand $Argument.Substring($head.Length))))
            }
        }
        foreach ($switch in $script:JoinedSwitches) {
            if ($Argument.StartsWith($switch, [StringComparison]::Ordinal) -and $script:PathHead.IsMatch($Argument.Substring($switch.Length))) {
                return , @(@(0, $switch), @($switch.Length, (& $operand $Argument.Substring($switch.Length))))
            }
        }
        return , @(, @(0, (& $operand $Argument)))
    }

    $joined = foreach ($argument in (Split-FlagArguments $Flags)) {
        $parts = & $pieces $argument.Text
        if ($null -eq $argument.Quote) { ($parts | ForEach-Object { $_[1] }) -join ''; continue }
        $at = 0
        for ($i = 0; $i -lt $parts.Count; $i++) { if ($parts[$i][0] -eq $argument.QuoteStart) { $at = $i; break } }
        $before = if ($at -gt 0) { ($parts[0..($at - 1)] | ForEach-Object { $_[1] }) -join '' } else { '' }
        $after = ($parts[$at..($parts.Count - 1)] | ForEach-Object { $_[1] }) -join ''
        $before + $argument.Quote + $after + $argument.Quote
    }
    return (@($joined) -join ' ')
}

function Hide-Identity {
    # Defensive last pass over every string in the manifest (#1445 review): any home-directory
    # prefix ($HOME, $env:USERPROFILE, both slash forms) becomes <home> (always), and the user
    # name, when it is at least 3 characters and stands alone between separators (start or end,
    # whitespace, a slash, a quote, '=', ':', ',' or ';'), becomes <user> -- so a user named
    # "build" leaves -DBUILD=1 alone but still hides -DBUILT_BY=build and C:/Users/build.
    # Kept in step with hide_identity in tools/bench-manifest.py.
    param($Value, [string[]]$Homes, [string]$User)
    if (-not $PSBoundParameters.ContainsKey('Homes')) {
        $Homes = @([Environment]::GetFolderPath('UserProfile'), $HOME, $env:HOME, $env:USERPROFILE)
    }
    if (-not $PSBoundParameters.ContainsKey('User')) { $User = [Environment]::UserName }
    if ($Value -is [System.Collections.IDictionary]) {
        $copy = [ordered]@{}
        foreach ($key in $Value.Keys) { $copy[$key] = Hide-Identity $Value[$key] -Homes $Homes -User $User }
        return $copy
    }
    if ($Value -is [string]) {
        $trimmed = @($Homes | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\', '/') } | Where-Object { $_.Length -gt 3 })
        $prefixes = @($trimmed | ForEach-Object { $_; $_.Replace('\', '/'); $_.Replace('/', '\') } | Sort-Object -Unique | Sort-Object Length -Descending)
        foreach ($prefix in $prefixes) { $Value = [regex]::Replace($Value, [regex]::Escape($prefix), '<home>', 'IgnoreCase') }
        if ($User -and $User.Length -ge 3) {
            $separated = '\s/\\"''=:,;'
            $Value = [regex]::Replace($Value, "(?<![^$separated])" + [regex]::Escape($User) + "(?![^$separated])", '<user>', 'IgnoreCase')
        }
        return $Value
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        # A list stays a list (written as a JSON array even with one item).
        return , [object[]]@(foreach ($item in $Value) { Hide-Identity $item -Homes $Homes -User $User })
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
