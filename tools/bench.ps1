#!/usr/bin/env pwsh
# bench.ps1 - Run TaskSmack benchmarks with consistent settings on Windows.
#
# Usage:
#   pwsh tools/bench.ps1 [preset] [-BenchmarkBinary <path>] [-OutputDirectory <dir>] [<extra args>]
#
# Extra args go to the benchmark binary (e.g. --benchmark_filter=Foo). A "--" separator before
# them works when the script is called from PowerShell (& tools/bench.ps1 win-benchmark -- ...),
# but not through "pwsh -File", whose own parameter binding rejects a bare "--".
#
# Preset defaults to 'win-benchmark'.
# Produces JSON output at perf-data/<preset>-<timestamp>.json (-2, -3, ... appended when a run in
# the same second already wrote that name; a user or host name in the preset becomes "user" /
# "host" in the file names), holding every repetition plus the
# mean/median/stddev/cv aggregates, and a provenance sidecar at
# perf-data/<preset>-<timestamp>.manifest.json (git state, binary SHA-256, build config, benchmark
# args, anonymized machine class; see CONTRIBUTING.md "Benchmark Output").
#
# -BenchmarkBinary overrides build/<preset>/bin/TaskSmackBenchmarks.exe (the script tests point
# it at a stub); a relative path or bare name is relative to the PowerShell location, resolved
# once to an absolute path for the check, the hash, the manifest and the launch, and never looked
# up on PATH. -OutputDirectory overrides perf-data/.
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
# Called from PowerShell as `& bench.ps1 -- --benchmark_filter=Foo`, the "--" ends PowerShell's own
# parameters and the first benchmark flag binds to $Preset: it belongs with the extra args.
elseif ($Preset.StartsWith("--")) {
    $ExtraArgs = @($Preset) + $ExtraArgs
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
# Resolved with PowerShell's path resolver, so a relative -BenchmarkBinary means the same file
# for the launch, the hash and the build-tree lookup (.NET APIs would resolve it against the
# process directory, which Set-Location does not change).
$benchBin = if ($BenchmarkBinary) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BenchmarkBinary) }
else { Join-Path $repoRoot "build/$Preset/bin/TaskSmackBenchmarks.exe" }

if (-not (Test-Path -LiteralPath $benchBin)) {
    Write-Error "Benchmark binary not found: $benchBin`nBuild first: cmake --build --preset $Preset"
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
# .NET file APIs resolve against the process directory, not the PowerShell location.
$outDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($outDir)

function Get-GitProvenance {
    # The checkout's commit, branch and dirty flag; a user or host name in the branch is hidden
    # (Hide-NameIdentity).
    param([string]$User = [Environment]::UserName, [string[]]$Hosts = (Get-HostNames))
    $git = [ordered]@{ commit = $null; branch = $null; dirty = $null }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return $git }
    # Before PowerShell 7.2, a native command's stderr becomes error records even when redirected
    # to $null, and the script's ErrorActionPreference of Stop makes them terminating: git's
    # "not a git repository" would abort the run. This function's own preference keeps them quiet;
    # the exit codes below decide.
    $ErrorActionPreference = 'Continue'
    # Only the script's own checkout counts: git searches parent directories, so a source archive
    # unpacked inside another checkout would otherwise report that checkout's commit (#1445
    # review). The repository root must be git's top level (an empty prefix).
    $prefix = & git -C $repoRoot rev-parse --show-prefix 2>$null
    if ($LASTEXITCODE -ne 0 -or "$prefix".Trim()) { return $git }
    $commit = & git -C $repoRoot rev-parse HEAD 2>$null
    if ($LASTEXITCODE -ne 0) { return $git }
    $git.commit = "$commit".Trim()
    $branch = & git -C $repoRoot rev-parse --abbrev-ref HEAD 2>$null
    if ($LASTEXITCODE -eq 0) { $git.branch = Hide-NameIdentity "$branch".Trim() -User $User -Hosts $Hosts }
    # Tracked changes only: untracked scratch files do not change what was built.
    $status = & git -C $repoRoot status --porcelain --untracked-files=no 2>$null
    if ($LASTEXITCODE -eq 0) { $git.dirty = [bool]("$status".Trim()) }
    return $git
}

# The benchmark arguments (#1445 review). An argument is recorded as written only when it is a
# Google Benchmark option whose value is safe by construction ($script:AllowedArguments: numbers,
# booleans, enumerations, and the --benchmark_filter regex, which the identity pass still covers);
# a value that fails its pattern, and every other argument (--benchmark_context=..., unknown ones),
# is recorded as <name>=sha256:<hex of the value>, or sha256:<hex of the argument> when it has no
# --name=value form. Runs stay comparable on their arguments without recording paths or other
# free text. The script's own --benchmark_out keeps its file name (extra ones are refused before
# launch). The same table as ALLOWED_ARGUMENTS in tools/bench-manifest.py.
$script:NumberPattern = '[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?'
$script:BooleanPattern = '(?i:true|false|yes|no|on|off|t|f|y|n|1|0)'
$script:AllowedArguments = @{
    '--benchmark_repetitions'                = '[0-9]+'
    '--benchmark_min_time'                   = $script:NumberPattern + '[sx]?'
    '--benchmark_min_warmup_time'            = $script:NumberPattern + 's?'
    '--benchmark_display_aggregates_only'    = $script:BooleanPattern
    '--benchmark_report_aggregates_only'     = $script:BooleanPattern
    '--benchmark_enable_random_interleaving' = $script:BooleanPattern
    '--benchmark_counters_tabular'           = $script:BooleanPattern
    '--benchmark_dry_run'                    = $script:BooleanPattern
    '--benchmark_list_tests'                 = $script:BooleanPattern
    '--benchmark_time_unit'                  = 'ns|us|ms|s'
    '--benchmark_format'                     = 'console|json|csv'
    '--benchmark_out_format'                 = 'console|json|csv'
    '--benchmark_color'                      = '(?i:auto|true|false|yes|no|on|off|1|0)'
    '--v'                                    = '[0-9]+'
    '--benchmark_filter'                     = '.*'
}

function Get-RecordedArgument {
    # How one benchmark argument is recorded in the manifest (see $script:AllowedArguments).
    param([string]$Argument)
    if ($Argument.StartsWith('--benchmark_out=', [StringComparison]::Ordinal)) {
        return '--benchmark_out=' + ($Argument.Substring('--benchmark_out='.Length).Replace('\', '/').TrimEnd('/') -split '/')[-1]
    }
    $match = [regex]::Match($Argument, '\A(--[A-Za-z0-9_]+)(?:=(.*))?\z', [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $match.Success) { return 'sha256:' + (Get-TextSha256 $Argument) }
    $name = $match.Groups[1].Value
    # Case-sensitive, as Google Benchmark's flag names are (a hashtable key lookup is not).
    $pattern = if ($script:AllowedArguments.Keys -ccontains $name) { $script:AllowedArguments[$name] } else { $null }
    if (-not $match.Groups[2].Success) {
        # A bare flag: as written for a boolean option (Google Benchmark reads it as true).
        if ($pattern -ceq $script:BooleanPattern) { return $Argument }
        return 'sha256:' + (Get-TextSha256 $Argument)
    }
    $value = $match.Groups[2].Value
    if ($null -ne $pattern -and [regex]::IsMatch($value, "\A(?:$pattern)\z", [Text.RegularExpressions.RegexOptions]::Singleline)) { return $Argument }
    return "$name=sha256:" + (Get-TextSha256 $value)
}

function Get-TextSha256 {
    # SHA-256 of a string's UTF-8 bytes, or $null for no value (an untyped parameter: a [string]
    # one would turn $null into ''). text_sha256 in tools/bench-manifest.py.
    param($Value)
    if ($null -eq $Value) { return $null }
    # SHA256.Create/ComputeHash and BitConverter, not .NET 5's SHA256.HashData/Convert.ToHexString:
    # PowerShell 7.0 runs on .NET Core 3.1.
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes([string]$Value)) }
    finally { $sha.Dispose() }
    return [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
}

function Get-HostNames {
    # This machine's names for Hide-Identity: the host name, its short form and the FQDN where
    # known (host plus DNS domain, no lookup), longest first so the FQDN goes before its short name.
    $names = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in @([Environment]::MachineName, $env:COMPUTERNAME, [System.Net.Dns]::GetHostName())) { if ($name) { [void]$names.Add($name) } }
    try {
        $ip = [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties()
        if ($ip.HostName) { [void]$names.Add($ip.HostName) }
        if ($ip.HostName -and $ip.DomainName) { [void]$names.Add("$($ip.HostName).$($ip.DomainName)") }
    }
    catch { $null = $_ }
    foreach ($name in @($names)) { [void]$names.Add($name.Split('.')[0]) }
    return [string[]]@($names | Sort-Object Length -Descending)
}

function Hide-NameIdentity {
    # A preset or git branch name with each host name (FQDN before short name) and the user name
    # (3+ characters, any case) standing alone between separators -- a name's own '-', '.', '_'
    # and '/' included -- replaced by <host> / <user> (#1445 review): the preset names the output
    # files and is recorded, and the branch is recorded, so neither may carry the user or the
    # machine. Only these two names get the wider boundaries; Hide-Identity keeps its own, so an
    # allowlisted --benchmark_filter value or x86_64 is never rewritten for a user named x86. Kept
    # in step with hide_name_identity in tools/bench-manifest.py.
    param([string]$Name, [string]$User = [Environment]::UserName, [string[]]$Hosts = (Get-HostNames))
    $separated = '\s/\\"''=:,;._\-'
    $tokens = @(@($Hosts | Where-Object { $_ } | Sort-Object Length -Descending | ForEach-Object { , @($_, '<host>') }) + , @($User, '<user>'))
    foreach ($pair in $tokens) {
        if ($pair[0] -and $pair[0].Length -ge 3) {
            $Name = [regex]::Replace($Name, "(?<![^$separated])" + [regex]::Escape($pair[0]) + "(?![^$separated])", $pair[1], 'IgnoreCase')
        }
    }
    return $Name
}

function Hide-Identity {
    # Defensive last pass over every string in the manifest (#1445 review): any home-directory
    # prefix ($HOME, $env:USERPROFILE, both slash forms) becomes <home>, whatever its length --
    # trailing separators trimmed, only an empty prefix (a root) and a bare drive (C:) left out,
    # since they would hide every path; each host name
    # (FQDN, short name) and the user name, when at least 3 characters and standing alone between
    # separators (start or end, whitespace, a slash, a quote, '=', ':', ',' or ';'), become <host>
    # and <user> -- so a user named "build" leaves -DBUILD=1 alone but still hides
    # -DBUILT_BY=build and C:/Users/build. Kept in step with hide_identity in
    # tools/bench-manifest.py.
    param($Value, [string[]]$Homes, [string]$User, [string[]]$Hosts)
    if (-not $PSBoundParameters.ContainsKey('Homes')) {
        $Homes = @([Environment]::GetFolderPath('UserProfile'), $HOME, $env:HOME, $env:USERPROFILE)
    }
    if (-not $PSBoundParameters.ContainsKey('User')) { $User = [Environment]::UserName }
    if (-not $PSBoundParameters.ContainsKey('Hosts')) { $Hosts = Get-HostNames }
    if ($Value -is [System.Collections.IDictionary]) {
        $copy = [ordered]@{}
        foreach ($key in $Value.Keys) { $copy[$key] = Hide-Identity $Value[$key] -Homes $Homes -User $User -Hosts $Hosts }
        return $copy
    }
    if ($Value -is [string]) {
        $trimmed = @($Homes | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\', '/') } | Where-Object { $_ -and $_ -notmatch '\A[A-Za-z]:\z' })
        $prefixes = @($trimmed | ForEach-Object { $_; $_.Replace('\', '/'); $_.Replace('/', '\') } | Sort-Object -Unique | Sort-Object Length -Descending)
        foreach ($prefix in $prefixes) { $Value = [regex]::Replace($Value, [regex]::Escape($prefix), '<home>', 'IgnoreCase') }
        $separated = '\s/\\"''=:,;'
        $tokens = @(@($Hosts | Where-Object { $_ } | Sort-Object Length -Descending | ForEach-Object { , @($_, '<host>') }) + , @($User, '<user>'))
        foreach ($pair in $tokens) {
            if ($pair[0] -and $pair[0].Length -ge 3) {
                $Value = [regex]::Replace($Value, "(?<![^$separated])" + [regex]::Escape($pair[0]) + "(?![^$separated])", $pair[1], 'IgnoreCase')
            }
        }
        return $Value
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        # A list stays a list (written as a JSON array even with one item).
        return , [object[]]@(foreach ($item in $Value) { Hide-Identity $item -Homes $Homes -User $User -Hosts $Hosts })
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

# CMakeCache.txt entries as CMake itself reads them (cmState::ParseCacheEntry): "KEY":TYPE=VALUE,
# then KEY:TYPE=VALUE, then the untyped "KEY"=VALUE and KEY=VALUE. CMake quotes a key holding ':',
# and any other character -- '-', '.', '+' of a custom build type's CMAKE_CXX_FLAGS_<CONFIG> -- is
# part of an unquoted key. Trailing spaces, tabs and carriage returns are dropped, and a value in
# single quotes (how CMake writes one with trailing whitespace) loses them. Kept in step with
# CACHE_ENTRY_PATTERNS / parse_cmake_cache in tools/bench-manifest.py.
$script:CacheValuePattern = '(.*[^\r\t ]|[\r\t ]*)[\r\t ]*$'
$script:CacheEntryPatterns = @(
    ('^"([^"]*)":[^=]*=' + $script:CacheValuePattern)
    ('^([^=:]*):[^=]*=' + $script:CacheValuePattern)
    ('^"([^"]*)"=' + $script:CacheValuePattern)
    ('^([^=]*)=' + $script:CacheValuePattern)
)

function Read-CMakeCache {
    # The entries of a CMakeCache.txt, keyed case-sensitively (CMake's keys are); a later entry
    # wins, as in CMake.
    param([string]$Path)
    $cache = [hashtable]::new([StringComparer]::Ordinal)
    foreach ($raw in [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8).Split("`n")) {
        # One trailing carriage return is part of the line ending (cmSystemTools::GetLineFromStream).
        $line = $(if ($raw.EndsWith("`r", [StringComparison]::Ordinal)) { $raw.Substring(0, $raw.Length - 1) } else { $raw }).TrimStart(' ', "`t")
        # Blank lines, '#' comments and '//' help text are not entries (cmCacheManager::LoadCache).
        if (-not $line -or $line.StartsWith('#', [StringComparison]::Ordinal) -or $line.StartsWith('//', [StringComparison]::Ordinal)) { continue }
        foreach ($pattern in $script:CacheEntryPatterns) {
            $match = [regex]::Match($line, $pattern)
            if ($match.Success) {
                $value = $match.Groups[2].Value
                if ($value.Length -ge 2 -and $value[0] -eq "'" -and $value[-1] -eq "'") { $value = $value.Substring(1, $value.Length - 2) }
                $cache[$match.Groups[1].Value] = $value
                break
            }
        }
    }
    return $cache
}

function ConvertTo-CMakeUpper {
    # CMake's cmSystemTools::UpperCase: ASCII letters only, as the <CONFIG> suffix is built.
    param([string]$Value)
    return [regex]::Replace($Value, '[a-z]', { param($m) [string][char]([int][char]$m.Value - 32) })
}

function Find-BuildTree {
    # The binary's build tree and multi-config configuration: the nearest ancestor holding
    # CMakeCache.txt, at most $MaxLevels directories up -- build/<preset>/bin/ for a single-config
    # generator, build/<preset>/bin/<Config>/ for a multi-config one
    # (benchmarks/CMakeLists.txt). Config is that <Config> directory's name, or $null for a flat
    # bin/. $null when no cache is found. Kept in step with find_build_tree in
    # tools/bench-manifest.py.
    param([string]$Binary, [int]$MaxLevels = 4)
    $binaryDir = Split-Path -Parent ([IO.Path]::GetFullPath($Binary))
    $directory = $binaryDir
    for ($level = 0; $level -lt $MaxLevels -and $directory; $level++) {
        if (Test-Path -LiteralPath (Join-Path $directory 'CMakeCache.txt') -PathType Leaf) {
            $parts = @([IO.Path]::GetRelativePath($directory, $binaryDir).Split([char[]]@('\', '/')) | Where-Object { $_ -and $_ -ne '.' })
            $config = if ($parts.Count -eq 2 -and $parts[0] -ieq 'bin') { $parts[1] } else { $null }
            return [pscustomobject]@{ Directory = $directory; Config = $config }
        }
        $directory = Split-Path -Parent $directory
    }
    return $null
}

# The build information benchmarks/CMakeLists.txt writes next to the binary for each configuration
# (file(GENERATE)): the IPO that configuration of the TaskSmackBenchmarks target really builds with,
# including the IPO cmake/CompilerOptions.cmake turns on through a normal variable the cache does
# not show, and per configuration under multi-config generators. Read first (ipo_source
# "buildinfo"). Older build trees, without it, fall back to these CMakeCache.txt entries, best
# first. The same as BUILDINFO_NAME / IPO_SOURCES in tools/bench-manifest.py.
$script:BuildInfoName = 'TaskSmackBenchmarks.buildinfo.json'
$script:IpoSources = @('CMAKE_INTERPROCEDURAL_OPTIMIZATION', 'TASKSMACK_ENABLE_IPO')

function Read-BuildInfo {
    # The build information next to the binary, as far as it is usable: Ipo (ON or OFF), and
    # CxxFlags -- the (cxx_flags_sha256, cxx_flags_config_sha256) pair, each a SHA-256 hex string or
    # $null (no such cache entry) -- when both keys are there and well-formed. Empty without a file.
    # Kept in step with read_buildinfo in tools/bench-manifest.py.
    param([string]$Binary)
    $usable = @{}
    $path = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($Binary))) $script:BuildInfoName
    try { $info = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8) | ConvertFrom-Json -ErrorAction Stop } catch { return $usable }
    if ($info -isnot [pscustomobject]) { return $usable }
    if ($null -ne $info.PSObject.Properties['ipo'] -and $info.ipo -is [string] -and ($info.ipo -ceq 'ON' -or $info.ipo -ceq 'OFF')) { $usable.Ipo = $info.ipo }
    $keys = @('cxx_flags_sha256', 'cxx_flags_config_sha256')
    $wellFormed = @($keys | Where-Object {
            $property = $info.PSObject.Properties[$_]
            $null -ne $property -and ($null -eq $property.Value -or ($property.Value -is [string] -and $property.Value -cmatch '\A[0-9a-f]{64}\z'))
        })
    if ($wellFormed.Count -eq $keys.Count) { $usable.CxxFlags = @($info.cxx_flags_sha256, $info.cxx_flags_config_sha256) }
    return $usable
}

function Get-BuildProvenance {
    # The build tree is found by Find-BuildTree (build/<preset>); read from its CMakeCache.txt.
    # Only the compiler's file name is kept: its full path can sit under a user profile.
    $build = [ordered]@{
        build_type       = $null
        generator        = $null
        compiler         = $null
        compiler_id      = $null
        compiler_version = $null
        cxx_flags_sha256        = $null
        cxx_flags_config_sha256 = $null
        cxx_flags_source        = $null
        ipo              = $null
        ipo_source       = $null
    }
    $buildInfo = Read-BuildInfo -Binary $benchBin
    if ($buildInfo.ContainsKey('Ipo')) { $build.ipo = $buildInfo.Ipo; $build.ipo_source = 'buildinfo' }
    # The flag hashes the binary was linked with, from the build information (#1445 review): the
    # cache can have been reconfigured since.
    if ($buildInfo.ContainsKey('CxxFlags')) {
        $build.cxx_flags_sha256 = $buildInfo.CxxFlags[0]
        $build.cxx_flags_config_sha256 = $buildInfo.CxxFlags[1]
        $build.cxx_flags_source = 'buildinfo'
    }
    $tree = Find-BuildTree -Binary $benchBin
    if ($null -eq $tree) { return $build }
    $buildDir = $tree.Directory
    $cache = Read-CMakeCache -Path (Join-Path $buildDir 'CMakeCache.txt')
    # A multi-config tree has no CMAKE_BUILD_TYPE: the binary's bin/<Config>/ names it.
    $build.build_type = if ($tree.Config) { $tree.Config } elseif ($cache['CMAKE_BUILD_TYPE']) { $cache['CMAKE_BUILD_TYPE'] } else { $null }
    $build.generator = $cache['CMAKE_GENERATOR']
    if ($cache['CMAKE_CXX_COMPILER']) { $build.compiler = Split-Path -Leaf $cache['CMAKE_CXX_COMPILER'] }
    # Flags can embed absolute paths (the PGO presets' -fprofile-instr-use=${sourceDir}/...).
    # The compiler flags are hashed, not recorded: two runs can be compared on them without the
    # manifest carrying their paths (include directories, the PGO presets' profile, prefix maps).
    # SHA-256 of the value as CMake reads it from CMakeCache.txt, UTF-8, otherwise unnormalized; null
    # when the entry is absent. The configuration's entry is named the way CMake names it. Only for a
    # tree without build information (older trees); benchmarks/CMakeLists.txt hashes the same way.
    if ($null -eq $build.cxx_flags_source) {
        $build.cxx_flags_sha256 = Get-TextSha256 $cache['CMAKE_CXX_FLAGS']
        if ($build.build_type) { $build.cxx_flags_config_sha256 = Get-TextSha256 $cache["CMAKE_CXX_FLAGS_$(ConvertTo-CMakeUpper $build.build_type)"] }
        $build.cxx_flags_source = 'cache'
    }
    # Without build information (an older tree), interprocedural optimization from the cache, and
    # which entry said so ($script:IpoSources).
    if ($null -eq $build.ipo_source) {
        foreach ($key in $script:IpoSources) {
            if ($cache.ContainsKey($key)) { $build.ipo = $cache[$key]; $build.ipo_source = $key; break }
        }
    }
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
    $machine = [ordered]@{
        label         = $null
        cpu_model     = $cpu
        logical_cores = $cores
        os_name       = $osName
        os_version    = $osVersion
        arch          = $arch
    }
    $machine.label = Get-MachineLabel $machine
    return $machine
}

function Get-MachineLabel {
    # The one-line machine class, built from the other machine fields (machine_label in
    # tools/bench-manifest.py builds the same string).
    param($Machine)
    $cpu = if ($Machine.cpu_model) { $Machine.cpu_model } else { 'unknown CPU' }
    return "$cpu / $($Machine.logical_cores) logical cores / $($Machine.os_name) $($Machine.os_version)".TrimEnd()
}

# Manifest fields the identity pass leaves alone (#1445 review): validated, categorical values
# that cannot carry a user or host name but can coincide with one (a host named "Linux", a user
# named "clang"), and the flag hashes. Every other string is free-form input and is scrubbed: the
# compiler file name, the benchmark args, the git branch, the preset and result names, the CPU model.
# machine.label is rebuilt from the scrubbed CPU model and the exempt fields. Numbers and booleans
# are never touched. The same list as IDENTITY_EXEMPT in tools/bench-manifest.py.
# build.build_type is exempt only as one of CMake's standard configurations: a custom
# configuration can be named after a user or host. The same list as STANDARD_BUILD_TYPES in
# tools/bench-manifest.py.
$script:StandardBuildTypes = @('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')
$script:IdentityExempt = @(
    'schema_version', 'generator', 'created_utc', 'exit_code',
    'git.commit', 'git.dirty',
    'binary.sha256',
    'build.generator', 'build.compiler_id', 'build.compiler_version', 'build.ipo', 'build.ipo_source', 'build.cxx_flags_sha256', 'build.cxx_flags_config_sha256', 'build.cxx_flags_source',
    'benchmark.raw_repetitions', 'benchmark.report_aggregates_only',
    'machine.label', 'machine.logical_cores', 'machine.os_name', 'machine.os_version', 'machine.arch'
)

function Hide-ManifestIdentity {
    # Hide-Identity over the manifest's free-form fields only (see $script:IdentityExempt).
    param($Manifest, [string[]]$Homes, [string]$User, [string[]]$Hosts)
    $identity = @{}
    foreach ($name in 'Homes', 'User', 'Hosts') { if ($PSBoundParameters.ContainsKey($name)) { $identity[$name] = $PSBoundParameters[$name] } }
    $names = @{}
    foreach ($name in 'User', 'Hosts') { if ($identity.ContainsKey($name)) { $names[$name] = $identity[$name] } }
    $walk = {
        param($Value, [string]$Path)
        if ($Value -is [System.Collections.IDictionary]) {
            $copy = [ordered]@{}
            foreach ($key in $Value.Keys) { $copy[$key] = & $walk $Value[$key] $(if ($Path) { "$Path.$key" } else { $key }) }
            return $copy
        }
        if ($script:IdentityExempt -contains $Path -or ($Path -eq 'build.build_type' -and $script:StandardBuildTypes -ccontains $Value)) { return , $Value }
        # A custom build type is a name like a preset (ASan-benchuser, Release_benchhost): its own
        # '-', '.' and '_' bound a user or host name too.
        if ($Path -eq 'build.build_type' -and $Value -is [string]) { $Value = Hide-NameIdentity $Value @names }
        return , (Hide-Identity $Value @identity)
    }
    $result = & $walk $Manifest ''
    if ($result.Contains('machine') -and $result.machine -is [System.Collections.IDictionary]) {
        # The OS version is exempt as a whole but not free of names: a Linux kernel built with
        # CONFIG_LOCALVERSION reports 6.8.0-benchhost. A user or host name in it, between its own
        # '-', '.' and '_', becomes <user> / <host> (Hide-NameIdentity), and the label is rebuilt
        # from the result. The same on Windows, whose version is a build number, for parity.
        if ($result.machine.os_version -is [string]) {
            $result.machine.os_version = Hide-NameIdentity $result.machine.os_version @names
        }
        $result.machine.label = Get-MachineLabel $result.machine
    }
    return $result
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

function New-BenchManifest {
    # The manifest as it stands before the benchmark starts (exit_code null): see the snapshot
    # below. The arguments are recorded through Get-RecordedArgument (allowlisted options as
    # written, everything else hashed), so the manifest carries no paths from them.
    $recordedArgs = [string[]]@($benchArgs | ForEach-Object { Get-RecordedArgument $_ })
    $aggregatesOnly = Get-EffectiveReportAggregatesOnly $benchArgs
    $manifest = [ordered]@{
        schema_version = 1
        generator      = 'tools/bench.ps1'
        created_utc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        preset         = $presetComponent
        result_file    = Split-Path -Leaf $outFile
        exit_code      = $null
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
    return Hide-ManifestIdentity $manifest
}

function Save-BenchManifest {
    param($Manifest)
    $Manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestFile -Encoding utf8
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

# The preset as the file names and the manifest carry it (#1445 review): a user or host name in
# it becomes <user> / <host> (Hide-NameIdentity), and "user" / "host" in the file names, so
# neither the files nor the manifest's result_file and --benchmark_out carry it.
$presetComponent = Hide-NameIdentity $Preset
$presetStem = $presetComponent.Replace('<', '').Replace('>', '')

# Claim the result name before the benchmark starts, atomically (CreateNew fails if the file
# exists), so two runs in the same second -- concurrent ones too -- never share a name: the later
# one gets -2, -3, ... A name whose manifest is left from an earlier run is skipped as well. The
# manifest name follows the claimed result name. Kept in step with claim_output in bench.sh.
$outFile = $null
for ($suffix = 1; -not $outFile; $suffix++) {
    $stem = if ($suffix -eq 1) { "$presetStem-$timestamp" } else { "$presetStem-$timestamp-$suffix" }
    if (Test-Path -LiteralPath (Join-Path $outDir "$stem.manifest.json")) { continue }
    $candidate = Join-Path $outDir "$stem.json"
    try {
        [IO.File]::Open($candidate, [IO.FileMode]::CreateNew).Dispose()
        $outFile = $candidate
    }
    catch [System.IO.IOException] {
        if (-not (Test-Path -LiteralPath $candidate)) { throw }
    }
}
$manifestFile = [IO.Path]::ChangeExtension($outFile, '.manifest.json')

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

Write-Host "Running benchmarks (preset=$Preset) -> $outFile"
Write-Host "Binary: $benchBin"
Write-Host ""

# Provenance is captured before the benchmark starts (#1445 review): git state, build
# configuration and the binary's hash describe what was launched, even if the checkout, the build
# or the binary changes during the run. Only the exit code is added afterwards. The manifest is
# finalized before the result is redacted, so the result file stays the newest one in the output
# directory (heavy-checks.yml picks the latest perf-data/benchmark-*.json). A manifest failure is
# held until the result has been redacted, then fails the script. Kept in step with bench.sh.
$manifestError = $null
$manifest = $null
try {
    $manifest = New-BenchManifest
    Save-BenchManifest $manifest
}
catch { $manifestError = $_ }

& $benchBin @benchArgs
# $ErrorActionPreference does not cover a native command's exit code (#1423): read it explicitly.
$benchExit = $LASTEXITCODE
if ($null -eq $benchExit) { $benchExit = 0 }

if ($null -ne $manifest -and -not $manifestError) {
    try {
        $manifest.exit_code = $benchExit
        Save-BenchManifest $manifest
    }
    catch { $manifestError = $_ }
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
    $manifestNote = if ($manifestError) { " Writing the manifest failed too: $manifestError" } else { " Manifest: $manifestFile" }
    throw "Benchmark binary exited with code $benchExit; results in '$outFile' are not usable.$manifestNote"
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

if ($manifestError) {
    throw "Writing the provenance manifest '$manifestFile' failed: $manifestError"
}

Write-Host ""
Write-Host "Results written to: $outFile"
Write-Host "Provenance manifest: $manifestFile"
Write-Host "Compare two runs with Google Benchmark's compare.py:"
Write-Host "  python -m google_benchmark.compare perf-data/win-baseline.json $outFile"
