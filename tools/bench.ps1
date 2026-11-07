#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    Run TaskSmack benchmarks with consistent settings on Windows.
.DESCRIPTION
    Runs build/<Preset>/bin/TaskSmackBenchmarks.exe with 10 repetitions and a 0.5 s minimum time
    per repetition, and writes JSON output at perf-data/<preset>-<timestamp>.json (-2, -3, ...
    appended when a run in the same second already wrote that name; a user or host name in the
    preset becomes "user" / "host" in the file names), holding every repetition plus the
    mean/median/stddev/cv aggregates, and a provenance sidecar at
    perf-data/<preset>-<timestamp>.manifest.json (git state, binary SHA-256, build config,
    benchmark args, anonymized machine class; see CONTRIBUTING.md "Benchmark Output"). On success
    the last line is RESULT=<result file path>, for scripts to read.

    Exits with the benchmark binary's own exit code if it fails or crashes, as tools/bench.sh does;
    any partial output is still redacted (or deleted when it cannot be) and the manifest records
    the exit code. An extra --benchmark_out or --benchmark_out_format argument is refused with exit
    code 2 before the benchmark starts.

    On a hybrid CPU the run is pinned to the performance cores by default (-Core). The system CPU
    load is sampled for 500 ms just before and just after the run and recorded in the manifest;
    the TASKSMACK_BENCH_LOAD_SAMPLE_MS environment variable changes the sample length (the script
    tests shorten it).

    Compare a run against a baseline with tools/check-benchmark-regression.py, or pass -Baseline
    and the script does it, measuring any flagged benchmark across launches (#3448): see -Baseline.
.PARAMETER Baseline
    A baseline result to check this run against once it succeeds (#3448). The run is checked with
    tools/check-benchmark-regression.py at its default thresholds plus any -CheckArgs. When it flags
    regressions, the flagged benchmarks alone run again in -Launches more separate launches of the
    binary (each a bench.ps1 run of its own with the same preset, binary, -Core and arguments, and a
    --benchmark_filter from tools/bench-across-launches.py plan), each one's median across all the
    launches' medians replaces its row in a copy of the run (bench-across-launches.py combine), and
    that copy is checked again at the same thresholds and coverage. This is the maintainer's decision
    on #3448 (2026-11-05), as heavy-checks.yml does on Linux (#3476): not best-of-N, and no threshold
    changes, so a benchmark slow in most launches still fails. The launches, the flagged names and the
    combined run go to <result>.across-launches\ next to the result. The script then exits with the
    check's exit code: 0 passed, 1 regressions or low coverage, 2 an unusable input. A coverage loss
    alone has nothing to re-measure and fails as it is. A further launch whose manifest records a
    different core class from the full run's (say, its pinning failed) is exit 2, with nothing
    combined. Needs Python (-Python), which runs as python -I -X utf8.
.PARAMETER Launches
    With -Baseline: how many more launches the flagged benchmarks get, 5 in all with the full run.
    Defaults to 4, as heavy-checks.yml.
.PARAMETER BudgetSeconds
    With -Baseline: the time the further launches may take. tools/bench-across-launches.py plan
    estimates them from the full run's own timings and refuses past it, and no launch starts once it
    is spent; either way the flags stand as regressions. Defaults to 600, as heavy-checks.yml.
.PARAMETER CheckArgs
    With -Baseline: more arguments for both checks. From PowerShell pass an array,
    & tools/bench.ps1 -Baseline <file> -CheckArgs @('--threshold', '40'). "pwsh -File" can't bind an
    array: there -CheckArgs takes one token, so write the option and its value as one,
    -CheckArgs '--threshold=40' (any further token goes to the benchmark binary). The script sets
    --baseline, --current, --current-manifest and --regressions-out itself, so they are refused, and
    so is any abbreviation of them (the checker's argparse accepts unique prefixes, e.g.
    --current-man), with or without =value. The script's own options also come after -CheckArgs
    on the checker's command line, so its values win either way.
.PARAMETER Python
    With -Baseline: the Python interpreter for the check. Defaults to the checkout's
    .venv\Scripts\python.exe when there is one, then python3 or python on PATH (not the Microsoft
    Store alias).
.PARAMETER Preset
    The CMake preset whose build holds the benchmark binary. Defaults to win-benchmark.
.PARAMETER BenchmarkBinary
    Overrides build/<Preset>/bin/TaskSmackBenchmarks.exe (the script tests point it at a stub).
    Defaults to the TASKSMACK_BENCH_BIN environment variable when it is set, as in bench.sh. A
    relative path or bare name is relative to the PowerShell location, resolved once to an
    absolute path for the check, the hash, the manifest and the launch, and never looked up on
    PATH.
.PARAMETER OutputDirectory
    Overrides perf-data/. Defaults to the TASKSMACK_BENCH_OUT_DIR environment variable when it is
    set, as in bench.sh.
.PARAMETER Core
    Which cores the benchmark runs on (#3163). "performance" pins the benchmark process, and every
    process it starts, to the logical CPUs of the highest EfficiencyClass, leaving out logical CPU 0
    (it takes most interrupts) and its core while that still leaves at least four. Never fewer than
    four (#3413): the *_Concurrent_PublicationWait benchmarks run up to four threads that spin on
    each other, and on one CPU they barely progress; with fewer than four of the highest class the
    run is not pinned and a warning says so. "any" leaves placement to the scheduler.
    Defaults to "performance" on a hybrid CPU (cores of more than one EfficiencyClass), where a
    whole run can otherwise land on efficiency cores and come out 2-3x slower, and to "any"
    elsewhere; on a CPU with one class "performance" changes nothing. The classes are read with
    GetLogicalProcessorInformationEx; where that fails the run is not pinned and a warning says
    so. The manifest records the choice, the affinity mask and the system CPU load just before
    and after the run.
.PARAMETER ExtraArgs
    Arguments passed to the benchmark binary (e.g. --benchmark_filter=Foo). A "--" separator before
    them works when the script is called from PowerShell (& tools/bench.ps1 win-benchmark -- ...),
    but not through "pwsh -File", whose own parameter binding rejects a bare "--" and splits any
    argument that starts with "-" at its first ":" as if it were -name:value (#3547), so
    "pwsh -File" can't pass --benchmark_filter=BM_X/iterations:1000; call the script from PowerShell
    with & for that.
.PARAMETER ExtraArgsFile
    Internal (#3547): a UTF-8 JSON array of strings that replaces ExtraArgs, which -Baseline's further
    launches use to hand their arguments to a bench.ps1 of their own exactly as they are. Can't be
    combined with ExtraArgs or -Baseline.
.EXAMPLE
    pwsh tools/bench.ps1
.EXAMPLE
    pwsh tools/bench.ps1 win-benchmark --benchmark_filter=ProcessModel
.EXAMPLE
    pwsh tools/bench.ps1 win-benchmark -Core any --benchmark_filter=ProcessModel
.EXAMPLE
    python tools/check-benchmark-regression.py --baseline perf-data/win-baseline.json --current perf-data/win-benchmark-<timestamp>.json
.EXAMPLE
    pwsh tools/bench.ps1 win-benchmark -Baseline perf-data/win-baseline.json
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Preset = "win-benchmark",

    # The same environment overrides as bench.sh (#2254); an explicit parameter wins.
    [string]$BenchmarkBinary = $env:TASKSMACK_BENCH_BIN,

    [string]$OutputDirectory = $env:TASKSMACK_BENCH_OUT_DIR,

    # No default here: unset means "performance on a hybrid CPU, any elsewhere" (Select-BenchCore).
    [ValidateSet('performance', 'any')]
    [string]$Core,

    # Check the run against a baseline, measuring flagged benchmarks across launches (#3448).
    [string]$Baseline,

    [ValidateRange(1, 100)]
    [int]$Launches = 4,

    [ValidateRange(0, [double]::MaxValue)]
    [double]$BudgetSeconds = 600,

    [string[]]$CheckArgs = @(),

    [string]$Python,

    # The benchmark arguments of a -Baseline further launch (#3547): see Invoke-AcrossLaunches' caller.
    [Parameter(DontShow)]
    [string]$ExtraArgsFile,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
# The script's own output is UTF-8 (#1648). git's output is decoded as UTF-8 by Invoke-GitText,
# whatever this setting (#1745).
# Without a console (a detached process) setting it throws; the default is kept then.
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try { [Console]::OutputEncoding = $OutputEncoding } catch { Write-Verbose "Console encoding unchanged: $_" }
# Native exit codes are read from $LASTEXITCODE (the benchmark's and git's). A session or profile
# that enables native-command error promotion would otherwise make a failing benchmark throw before
# its exit code is captured, skipping the manifest and the redaction of its partial output (#1423).
$PSNativeCommandUseErrorActionPreference = $false

$benchScriptPath = $MyInvocation.MyCommand.Path
$scriptDir = Split-Path -Parent $benchScriptPath
$repoRoot = Split-Path -Parent $scriptDir
# Read-CMakeCache and ConvertTo-CMakeUpper, shared with profile-etw-common.ps1 (#1479).
. (Join-Path $scriptDir 'cmake-cache.ps1')

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

# A -Baseline further launch's arguments, read from the file its parent wrote (#3547): through
# "pwsh -File" they would be split at their first ":" (see ExtraArgs). System.Text.Json, not
# ConvertFrom-Json, which would turn a date-shaped string into a DateTime.
if ($ExtraArgsFile) {
    if ($ExtraArgs.Count -gt 0 -or $Baseline) {
        Write-Error -Message '-ExtraArgsFile cannot be combined with benchmark arguments or -Baseline.' -ErrorAction Continue
        exit 2
    }
    try {
        $ExtraArgs = [System.Text.Json.JsonSerializer]::Deserialize([IO.File]::ReadAllText($ExtraArgsFile, [Text.UTF8Encoding]::new($false)), [string[]], [System.Text.Json.JsonSerializerOptions]::new())
    }
    catch {
        Write-Error -Message "-ExtraArgsFile '$ExtraArgsFile' is not a JSON array of strings: $_" -ErrorAction Continue
        exit 2
    }
    if ($null -eq $ExtraArgs) { $ExtraArgs = @() }
}

# The script owns the output file: Google Benchmark takes the last --benchmark_out(_format), so an
# extra one would write somewhere the redaction and the manifest never look.
# Exit code 2, a usage error, as in bench.sh (#2254).
foreach ($arg in $ExtraArgs) {
    if ($arg -match '^--benchmark_out(_format)?(=|$)') {
        Write-Error -Message "'$arg' is not allowed: bench.ps1 sets the benchmark output file and format itself. Use -OutputDirectory (or TASKSMACK_BENCH_OUT_DIR) to choose where results are written." -ErrorAction Continue
        exit 2
    }
}

function Find-BenchPython {
    # The interpreter for -Baseline's checks: -Python, else the checkout's .venv, else python3 or
    # python on PATH, leaving out the Microsoft Store alias (it opens the Store instead of running).
    # $null when there is none.
    param([string]$Requested, [string]$RepoRoot)
    if ($Requested) {
        $path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Requested)
        return $(if (Test-Path -LiteralPath $path -PathType Leaf) { $path } else { $null })
    }
    $venv = Join-Path $RepoRoot '.venv\Scripts\python.exe'
    if (Test-Path -LiteralPath $venv -PathType Leaf) { return $venv }
    $found = @(Get-Command python3, python -CommandType Application -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1 -ExpandProperty Source)
    return $(if ($found.Count -gt 0) { $found[0] } else { $null })
}

function Get-ReservedCheckOption {
    # The checker option bench.ps1 sets itself that $Argument would set: the option, its =value form,
    # or an abbreviation of either. tools/check-benchmark-regression.py's argparse accepts any unique
    # prefix of a long option (no allow_abbrev=False), so --current-man=x is --current-manifest.
    # Every option it could abbreviate is named, joined with "or"; $null when it sets none of them.
    param([string]$Argument)
    $name = $Argument.Split('=', 2)[0]
    if (-not $name.StartsWith('--') -or $name.Length -le 2) { return $null }
    $hits = @('--baseline', '--current', '--current-manifest', '--regressions-out' | Where-Object { $_.StartsWith($name, [StringComparison]::Ordinal) })
    return $(if ($hits.Count -gt 0) { $hits -join ' or ' } else { $null })
}

# -Baseline is checked before the run, so a mistake fails now and not after a 25-minute run. Exit
# code 2, a usage error.
$pythonExe = $null
if ($Baseline) {
    $Baseline = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Baseline)
    if (-not (Test-Path -LiteralPath $Baseline -PathType Leaf)) {
        Write-Error -Message "Baseline not found: $Baseline" -ErrorAction Continue
        exit 2
    }
    $pythonExe = Find-BenchPython -Requested $Python -RepoRoot $repoRoot
    if (-not $pythonExe) {
        Write-Error -Message "-Baseline needs Python: none found$(if ($Python) { " at '$Python'" } else { ' (.venv, python3 or python on PATH)' }). Pass -Python <path>." -ErrorAction Continue
        exit 2
    }
    foreach ($arg in $CheckArgs) {
        $owned = Get-ReservedCheckOption $arg
        if ($owned) {
            Write-Error -Message "'$arg' is not allowed in -CheckArgs: the checker reads it as $owned, which bench.ps1 sets for each check itself." -ErrorAction Continue
            exit 2
        }
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

function Invoke-GitText {
    # git's exit code and standard output, decoded as UTF-8 whatever the console's code page
    # (#1745). PowerShell decodes a native command's output with [Console]::OutputEncoding, which
    # follows the console the script inherited: the script sets it to UTF-8 above, but not without
    # a console, and a caller that loads only these functions (the tests) never sets it. Under a
    # 437 or 1252 console, a non-ASCII branch name was read back as mojibake. git's stderr ("not a
    # git repository") is read and dropped; the exit code decides.
    param([string]$Git, [string[]]$Arguments)
    $start = [System.Diagnostics.ProcessStartInfo]::new($Git)
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $process = [System.Diagnostics.Process]::Start($start)
    try {
        # Both pipes are drained together, so a full stderr pipe cannot stall git.
        $errorText = $process.StandardError.ReadToEndAsync()
        $output = $process.StandardOutput.ReadToEnd()
        $null = $errorText.GetAwaiter().GetResult()
        $process.WaitForExit()
        return [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $output }
    }
    finally { $process.Dispose() }
}

function Get-GitProvenance {
    # The checkout's commit, branch and dirty flag; a user or host name in the branch is hidden
    # (Hide-NameIdentity).
    param([string]$User = [Environment]::UserName, [string[]]$Hosts = (Get-HostNames))
    $git = [ordered]@{ commit = $null; branch = $null; dirty = $null }
    $gitCommand = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $gitCommand) { return $git }
    $gitPath = $gitCommand.Source
    # Only the script's own checkout counts: git searches parent directories, so a source archive
    # unpacked inside another checkout would otherwise report that checkout's commit (#1445
    # review). The repository root must be git's top level (an empty prefix).
    $prefix = Invoke-GitText $gitPath @('-C', $repoRoot, 'rev-parse', '--show-prefix')
    if ($prefix.ExitCode -ne 0 -or $prefix.Output.Trim()) { return $git }
    $commit = Invoke-GitText $gitPath @('-C', $repoRoot, 'rev-parse', 'HEAD')
    if ($commit.ExitCode -ne 0) { return $git }
    $git.commit = $commit.Output.Trim()
    $branch = Invoke-GitText $gitPath @('-C', $repoRoot, 'rev-parse', '--abbrev-ref', 'HEAD')
    if ($branch.ExitCode -eq 0) { $git.branch = Hide-NameIdentity $branch.Output.Trim() -User $User -Hosts $Hosts }
    # Tracked changes only: untracked scratch files do not change what was built.
    $status = Invoke-GitText $gitPath @('-C', $repoRoot, 'status', '--porcelain', '--untracked-files=no')
    if ($status.ExitCode -eq 0) { $git.dirty = [bool]($status.Output.Trim()) }
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
    # The build information next to the binary, as far as it is usable: Config (a non-empty
    # configuration name without control characters), Ipo (ON or OFF), and
    # CxxFlags -- the (cxx_flags_sha256, cxx_flags_config_sha256) pair, each a SHA-256 hex string or
    # $null (no such cache entry) -- when both keys are there and well-formed. Empty without a file.
    # Kept in step with read_buildinfo in tools/bench-manifest.py.
    param([string]$Binary)
    $usable = @{}
    $path = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($Binary))) $script:BuildInfoName
    try { $info = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8) | ConvertFrom-Json -ErrorAction Stop } catch { return $usable }
    if ($info -isnot [pscustomobject]) { return $usable }
    if ($null -ne $info.PSObject.Properties['config'] -and $info.config -is [string] -and $info.config -cmatch '\A[^\x00-\x1f]+\z') { $usable.Config = $info.config }
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
    # The configuration the binary was linked as, from the build information (#1445 review): a
    # single-config tree reconfigured from Release to Debug without a rebuild still holds the
    # Release binary. build_type, ipo and the flag hashes then all describe the same binary.
    if ($buildInfo.ContainsKey('Config')) { $build.build_type = $buildInfo.Config }
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
    # Without build information: a multi-config tree has no CMAKE_BUILD_TYPE, and the binary's
    # bin/<Config>/ names it.
    if (-not $build.build_type) { $build.build_type = if ($tree.Config) { $tree.Config } elseif ($cache['CMAKE_BUILD_TYPE']) { $cache['CMAKE_BUILD_TYPE'] } else { $null } }
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

# Core placement and system load (#3163). On a hybrid CPU a whole run can land on efficiency cores
# and come out 2-3x slower on every benchmark, pure arithmetic included; pinned to one performance
# core the same comparison agreed within about 10%. The two Win32 calls are compiled on first use,
# not when the script starts, and only on Windows.
$script:BenchNativeSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace TaskSmackBench
{
    public static class Native
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetLogicalProcessorInformationEx(int relationship, IntPtr buffer, ref uint length);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetSystemTimes(out long idle, out long kernel, out long user);

        // The raw SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX records for RelationProcessorCore (0), one per
        // physical core; ConvertFrom-ProcessorCoreBuffer reads them.
        public static byte[] ProcessorCores()
        {
            const int relationProcessorCore = 0;
            const int errorInsufficientBuffer = 122;
            uint length = 0;
            if (GetLogicalProcessorInformationEx(relationProcessorCore, IntPtr.Zero, ref length)) return new byte[0];
            int error = Marshal.GetLastWin32Error();
            if (error != errorInsufficientBuffer) throw new Win32Exception(error);
            IntPtr buffer = Marshal.AllocHGlobal((int)length);
            try
            {
                if (!GetLogicalProcessorInformationEx(relationProcessorCore, buffer, ref length)) throw new Win32Exception(Marshal.GetLastWin32Error());
                byte[] bytes = new byte[length];
                Marshal.Copy(buffer, bytes, 0, (int)length);
                return bytes;
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }

        // Idle, kernel (idle included) and user time of all CPUs, in 100 ns units.
        public static long[] SystemTimes()
        {
            long idle, kernel, user;
            if (!GetSystemTimes(out idle, out kernel, out user)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return new long[] { idle, kernel, user };
        }
    }
}
'@

# How long each system-load sample lasts, just before and just after the run. The script tests
# shorten it with TASKSMACK_BENCH_LOAD_SAMPLE_MS.
$script:LoadSampleMilliseconds = 500
# The fewest logical CPUs -Core performance pins a run to (#3413): the most threads one benchmark
# keeps running at once, the *_Concurrent_PublicationWait benchmarks' measuring thread, writer and
# two extra readers (benchmarks/bench_ModelLimits.cpp), all of which spin. Kept in step with
# bench-manifest.py's MIN_PINNED_CPUS.
$script:MinPinnedCpus = 4

function Initialize-BenchNative {
    if (-not ('TaskSmackBench.Native' -as [type])) { Add-Type -TypeDefinition $script:BenchNativeSource }
}

function ConvertFrom-ProcessorCoreBuffer {
    # The physical cores in a GetLogicalProcessorInformationEx(RelationProcessorCore) buffer: each
    # one's EfficiencyClass (higher is faster), processor group and logical processor numbers in
    # that group. A record is SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX: Relationship (4 bytes), Size
    # (4), then PROCESSOR_RELATIONSHIP: Flags (1), EfficiencyClass (1), Reserved (20), GroupCount
    # (2), and GROUP_AFFINITY entries from byte 32 (a pointer-sized Mask, then the WORD Group). A
    # core never spans groups, so its first entry holds all of it. Records of other kinds are
    # skipped by their Size. Throws on a truncated or malformed buffer.
    param([byte[]]$Buffer, [int]$PointerSize = [IntPtr]::Size)
    $cores = [System.Collections.Generic.List[object]]::new()
    $offset = 0
    while ($offset -lt $Buffer.Length) {
        if ($offset + 8 -gt $Buffer.Length) { throw "Truncated processor information at byte $offset" }
        $relationship = [BitConverter]::ToInt32($Buffer, $offset)
        $size = [BitConverter]::ToInt32($Buffer, $offset + 4)
        if ($size -lt 8 -or $offset + $size -gt $Buffer.Length) { throw "Malformed processor information at byte $offset (size $size)" }
        if ($relationship -eq 0) {
            if ($size -lt 32 + $PointerSize + 2) { throw "Processor core record at byte $offset is too short (size $size)" }
            $maskAt = $offset + 32
            if ([BitConverter]::ToUInt16($Buffer, $offset + 30) -ge 1) {
                $mask = if ($PointerSize -eq 8) { [BitConverter]::ToUInt64($Buffer, $maskAt) } else { [uint64][BitConverter]::ToUInt32($Buffer, $maskAt) }
                $processors = [int[]]@(for ($bit = 0; $bit -lt $PointerSize * 8; $bit++) { if (($mask -band ([uint64]1 -shl $bit)) -ne 0) { $bit } })
                $cores.Add([pscustomobject]@{
                        EfficiencyClass = [int]$Buffer[$offset + 9]
                        Group           = [int][BitConverter]::ToUInt16($Buffer, $maskAt + $PointerSize)
                        Processors      = $processors
                    })
            }
        }
        $offset += $size
    }
    return , $cores.ToArray()
}

function Get-ProcessorCores {
    # This machine's physical cores (ConvertFrom-ProcessorCoreBuffer), or $null where they cannot
    # be read: outside Windows, or when the call or the compile fails.
    if (-not $IsWindows) { return $null }
    try {
        Initialize-BenchNative
        return , (ConvertFrom-ProcessorCoreBuffer ([TaskSmackBench.Native]::ProcessorCores()))
    }
    catch {
        Write-Verbose "Reading the processor cores failed: $_"
        return $null
    }
}

function ConvertFrom-AffinityPointer {
    # A Process.ProcessorAffinity value as an unsigned mask.
    param([IntPtr]$Pointer)
    $mask = [BitConverter]::ToUInt64([BitConverter]::GetBytes($Pointer.ToInt64()), 0)
    if ([IntPtr]::Size -eq 4) { $mask = $mask -band [uint64][uint32]::MaxValue }
    return $mask
}

function ConvertTo-AffinityPointer {
    param([uint64]$Mask)
    return [IntPtr][BitConverter]::ToInt64([BitConverter]::GetBytes($Mask), 0)
}

function Get-ProcessAffinityMask {
    # The logical CPUs this process may run on (its processor group's mask), or $null.
    try { return ConvertFrom-AffinityPointer ([System.Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity) }
    catch {
        Write-Verbose "Reading the process affinity failed: $_"
        return $null
    }
}

function Format-AffinityMask {
    param($Mask)
    if ($null -eq $Mask) { return $null }
    return '0x{0:x}' -f [uint64]$Mask
}

function Select-BenchCore {
    # Where the run goes (#3163), from the physical cores (ConvertFrom-ProcessorCoreBuffer's
    # shape, or $null when they could not be read), the -Core choice ('' when not given) and the
    # CPUs this process may use. Returns:
    #  - Core: the mode asked for, or the default: "performance" on a hybrid CPU (more than one
    #    EfficiencyClass), "any" otherwise;
    #  - CoreClass: the EfficiencyClass of the pinned CPUs, or "any" when the run is not pinned;
    #  - Hybrid: $true / $false, or $null when the cores are unknown;
    #  - LogicalCpus and Mask: the logical CPUs to pin to (ascending) and their affinity mask, or
    #    $null for none;
    #  - Warning: why "performance" could not pin, or $null.
    # "performance" takes the logical CPUs of the highest EfficiencyClass in processor group 0
    # (Process.ProcessorAffinity sets the process's primary group, which is group 0 on any machine
    # with 64 logical CPUs or fewer): those on cores that do not hold logical CPU 0, which takes
    # most interrupts (an SMT sibling of CPU 0 shares its core), else every one but CPU 0, else all
    # of them, the first of those with at least $script:MinPinnedCpus. A set rather than one CPU
    # (#3413): it still keeps the run off the efficiency cores (#3163), and a multi-threaded
    # benchmark gets a CPU per thread. With fewer than that the run is not pinned. On a CPU of one
    # class it changes nothing.
    param([AllowNull()][object[]]$Cores, [AllowEmptyString()][string]$Requested, [AllowNull()]$AllowedMask)
    $known = $null -ne $Cores -and $Cores.Count -gt 0
    $hybrid = if ($known) { @($Cores | ForEach-Object { $_.EfficiencyClass } | Sort-Object -Unique).Count -gt 1 } else { $null }
    $mode = if ($Requested) { $Requested } elseif ($hybrid) { 'performance' } else { 'any' }
    $choice = [pscustomobject]@{ Core = $mode; CoreClass = 'any'; Hybrid = $hybrid; LogicalCpus = $null; Mask = $null; Warning = $null }
    if ($mode -ne 'performance') {
        if (-not $known -and -not $Requested) { $choice.Warning = 'The processor cores could not be read, so the run is not pinned (-Core any). On a hybrid CPU it may land on efficiency cores.' }
        return $choice
    }
    if (-not $known) {
        $choice.Warning = 'The processor cores could not be read, so -Core performance does nothing and the run is not pinned.'
        return $choice
    }
    if (-not $hybrid) { return $choice }
    $allowed = if ($null -eq $AllowedMask) { [uint64]::MaxValue } else { [uint64]$AllowedMask }
    $usable = { param([int]$Cpu) $Cpu -lt 64 -and ($allowed -band ([uint64]1 -shl $Cpu)) -ne 0 }
    $top = [int]($Cores | Measure-Object -Property EfficiencyClass -Maximum).Maximum
    $topCores = @($Cores | Where-Object { $_.EfficiencyClass -eq $top -and $_.Group -eq 0 })
    $apart = @($topCores | Where-Object { $_.Processors -notcontains 0 } | ForEach-Object { $_.Processors } | Where-Object { & $usable $_ })
    $notZero = @($topCores | ForEach-Object { $_.Processors } | Where-Object { $_ -ne 0 -and (& $usable $_) })
    $any = @($topCores | ForEach-Object { $_.Processors } | Where-Object { & $usable $_ })
    if (-not $any.Count) {
        $choice.Warning = "No logical CPU of EfficiencyClass $top is in this process's affinity (processor group 0), so the run is not pinned."
        return $choice
    }
    # Each tier holds the one before it, so the last is the largest.
    $candidates = @(if ($apart.Count -ge $script:MinPinnedCpus) { $apart } elseif ($notZero.Count -ge $script:MinPinnedCpus) { $notZero } else { $any })
    if ($candidates.Count -lt $script:MinPinnedCpus) {
        $choice.Warning = "Only $($candidates.Count) logical CPU(s) of EfficiencyClass $top are in this process's affinity, fewer than the $($script:MinPinnedCpus) threads the concurrent benchmarks run at once, so the run is not pinned."
        return $choice
    }
    $cpus = [int[]]@($candidates | Sort-Object -Unique)
    $mask = [uint64]0
    foreach ($cpu in $cpus) { $mask = $mask -bor ([uint64]1 -shl $cpu) }
    $choice.CoreClass = $top
    $choice.LogicalCpus = $cpus
    $choice.Mask = $mask
    return $choice
}

function Get-CpuLoadPercent {
    # The busy share of all CPUs between two GetSystemTimes samples (idle, kernel, user; kernel
    # time includes idle time), 0-100 to one decimal, or $null when no time passed.
    param([long[]]$Before, [long[]]$After)
    $idle = $After[0] - $Before[0]
    $total = ($After[1] - $Before[1]) + ($After[2] - $Before[2])
    if ($total -le 0) { return $null }
    return [math]::Round([math]::Min(100.0, [math]::Max(0.0, 100.0 * ($total - $idle) / $total)), 1)
}

function Measure-SystemCpuLoad {
    # The system CPU load over the next $Milliseconds, or $null outside Windows or on a failure.
    param([int]$Milliseconds = $script:LoadSampleMilliseconds)
    if (-not $IsWindows) { return $null }
    try {
        Initialize-BenchNative
        $before = [TaskSmackBench.Native]::SystemTimes()
        Start-Sleep -Milliseconds $Milliseconds
        return Get-CpuLoadPercent $before ([TaskSmackBench.Native]::SystemTimes())
    }
    catch {
        Write-Verbose "Sampling the system CPU load failed: $_"
        return $null
    }
}

function Get-PlacementDescription {
    # The console line for a run's placement.
    param($Choice, $Mask)
    $where = if ($null -ne $Choice.LogicalCpus) { "logical CPUs $($Choice.LogicalCpus -join ',') (EfficiencyClass $($Choice.CoreClass))" }
    elseif ($Choice.Hybrid -eq $false) { 'any (not a hybrid CPU)' }
    else { 'any (not pinned)' }
    $affinity = if ($null -ne $Mask) { ", affinity $(Format-AffinityMask $Mask)" } else { '' }
    return "Cores: -Core $($Choice.Core) -> $where$affinity"
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
    'machine.label', 'machine.logical_cores', 'machine.os_name', 'machine.os_version', 'machine.arch',
    'placement.core', 'placement.core_class', 'placement.affinity_mask'
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
        # Where the run went (#3163, #3413): the -Core mode, the pinned CPUs' EfficiencyClass ("any"
        # when not pinned), whether the CPU is hybrid, the logical CPUs and the affinity mask it ran with.
        placement      = [ordered]@{
            core          = $coreChoice.Core
            core_class    = $coreChoice.CoreClass
            hybrid        = $coreChoice.Hybrid
            logical_cpus  = $coreChoice.LogicalCpus
            affinity_mask = Format-AffinityMask $runMask
        }
        # The system CPU load just before and just after the run, each over sample_ms: pinning does
        # not help a run that shares the machine with a build (#3163). Filled in around the launch.
        system_load    = [ordered]@{
            sample_ms      = $loadSampleMs
            before_percent = $null
            after_percent  = $null
        }
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

function Get-ManifestCoreClass {
    # The core class a bench.ps1 manifest records, as text: the pinned EfficiencyClass, "any" when not
    # pinned, or "(none)" when it records no placement. Throws when the manifest does not parse.
    param([string]$Path)
    $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $placement = if ($manifest.PSObject.Properties['placement']) { $manifest.placement } else { $null }
    if ($null -eq $placement -or -not $placement.PSObject.Properties['core_class'] -or $null -eq $placement.core_class) { return '(none)' }
    return [string]$placement.core_class
}

function Invoke-AcrossLaunches {
    # -Baseline (#3448): check $First against $Baseline and, when it flags regressions, measure the
    # flagged benchmarks across launches and check the median across them, as heavy-checks.yml's
    # "Re-measure flagged benchmarks across launches" does on Linux (#3476), with the same
    # tools/bench-across-launches.py. $Launch runs one further launch: it gets the filter and a
    # directory of its own and returns @{ ExitCode; Result }. Returns the exit code the script ends
    # with: the last check's (0 passed, 1 regressions or low coverage, 2 unusable input), 1 when the
    # flags stand (over budget, or nothing to re-measure), or a failed launch's own exit code.
    param(
        [string]$Python,
        [string]$ToolsDirectory,
        [string]$Baseline,
        [string]$First,
        [string]$FirstManifest,
        [string]$WorkDirectory,
        [int]$Launches,
        [double]$BudgetSeconds,
        [string[]]$CheckArgs,
        [scriptblock]$Launch
    )
    $checker = Join-Path $ToolsDirectory 'check-benchmark-regression.py'
    $helper = Join-Path $ToolsDirectory 'bench-across-launches.py'
    New-Item -ItemType Directory -Path $WorkDirectory -Force | Out-Null
    $regressions = Join-Path $WorkDirectory 'regressions.json'
    $nativeExit = { if ($null -eq $LASTEXITCODE) { 2 } else { $LASTEXITCODE } }
    # Python in isolated UTF-8 mode, as CONTRIBUTING.md asks of wrappers: a benchmark name outside the
    # console code page would otherwise fail to print through the pipe PowerShell reads.
    $pythonArgs = @('-I', '-X', 'utf8')
    # The full run's core class, which every further launch must share (#3163): one that ran elsewhere
    # (say, its pinning failed and it ran unpinned) would mix cores in one median.
    $firstClass = if ($FirstManifest -and (Test-Path -LiteralPath $FirstManifest -PathType Leaf)) { Get-ManifestCoreClass $FirstManifest } else { $null }

    Write-Host ""
    Write-Host "Checking against $Baseline"
    # -CheckArgs first: the script's own options come after them and win (argparse keeps the last).
    & $Python @pythonArgs $checker @CheckArgs --baseline $Baseline --current $First --regressions-out $regressions | Out-Host
    $checked = & $nativeExit
    # Only flagged regressions are re-measured; a pass, or a usage error, stands.
    if ($checked -ne 1) { return $checked }

    # The budget runs from here. plan refuses (exit 1) when there is nothing to re-measure (a coverage
    # loss alone) or the estimate is over budget, and says which.
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Write-Host ""
    $planOut = @(& $Python @pythonArgs $helper plan --current $First --regressions $regressions --launches $Launches --budget-seconds $BudgetSeconds)
    $planned = & $nativeExit
    if ($planned -ne 0) {
        if ($planned -eq 1) { Write-Host "The flags stand as regressions." }
        return $planned
    }
    $filter = [string]$planOut[-1]

    $results = [Collections.Generic.List[string]]::new()
    $total = $Launches + 1
    for ($index = 2; $index -le $total; $index++) {
        if ($clock.Elapsed.TotalSeconds -ge $BudgetSeconds) {
            Write-Host "Out of time: launch $index of $total not started, past the $($BudgetSeconds)s budget. The flags stand as regressions."
            return 1
        }
        $directory = Join-Path $WorkDirectory "launch-$index"
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        Write-Host ""
        Write-Host "Launch $index of ${total}: --benchmark_filter=$filter"
        $launched = & $Launch $filter $directory
        if ($launched.ExitCode -ne 0) {
            Write-Error -Message "Launch $index failed (exit $($launched.ExitCode))." -ErrorAction Continue
            return $launched.ExitCode
        }
        if (-not $launched.Result -or -not (Test-Path -LiteralPath $launched.Result -PathType Leaf)) {
            Write-Error -Message "Launch $index's result not found (RESULT=$($launched.Result))." -ErrorAction Continue
            return 2
        }
        if ($null -ne $firstClass) {
            $launchManifest = [IO.Path]::ChangeExtension($launched.Result, '.manifest.json')
            $launchClass = if (Test-Path -LiteralPath $launchManifest -PathType Leaf) { Get-ManifestCoreClass $launchManifest } else { '(no manifest)' }
            if ($launchClass -cne $firstClass) {
                Write-Error -Message "Launch $index ran on core class $launchClass, not the full run's $firstClass ($launchManifest), so its times can't share a median with the full run's. Nothing is combined." -ErrorAction Continue
                return 2
            }
        }
        $results.Add($launched.Result)
    }

    # A flagged benchmark with no usable median in some launch fails here (exit 2), not quietly.
    $combined = Join-Path $WorkDirectory 'combined.json'
    Write-Host ""
    & $Python @pythonArgs $helper combine --first $First --regressions $regressions --baseline $Baseline --output $combined @results | Out-Host
    $combinedExit = & $nativeExit
    if ($combinedExit -ne 0) { return $combinedExit }

    # The same check on the combined run, with the full run's manifest (the core-class check).
    $manifestArgs = if ($FirstManifest -and (Test-Path -LiteralPath $FirstManifest -PathType Leaf)) { @('--current-manifest', $FirstManifest) } else { @() }
    Write-Host ""
    Write-Host "Checking the median across $total launches against $Baseline"
    & $Python @pythonArgs $checker @CheckArgs --baseline $Baseline --current $combined @manifestArgs | Out-Host
    return (& $nativeExit)
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

# Core placement (#3163, #3413): on a hybrid CPU the run is pinned to the performance cores by
# default. This process is pinned only around the launch, so the benchmark and anything it starts
# inherit the affinity while the script's own work (git, hashing, the manifest) is not confined to
# them, and its affinity is put back afterwards (it may be an interactive session that ran
# `& bench.ps1`). A failure to pin is a warning and an unpinned run, recorded as such.
$loadSampleMs = $script:LoadSampleMilliseconds
if ($env:TASKSMACK_BENCH_LOAD_SAMPLE_MS -match '^[0-9]+$') { $loadSampleMs = [int]$env:TASKSMACK_BENCH_LOAD_SAMPLE_MS }
$processMask = Get-ProcessAffinityMask
$coreChoice = Select-BenchCore -Cores (Get-ProcessorCores) -Requested $Core -AllowedMask $processMask
$runMask = if ($null -ne $coreChoice.Mask) { $coreChoice.Mask } else { $processMask }

# Provenance is captured before the benchmark starts (#1445 review): git state, build
# configuration and the binary's hash describe what was launched, even if the checkout, the build
# or the binary changes during the run. Only the exit code and the load after the run are added
# afterwards. The manifest is finalized before the result is redacted. A manifest failure is held
# until the result has been redacted, then fails the script. Kept in step with bench.sh.
$manifestError = $null
$manifest = $null
try {
    $manifest = New-BenchManifest
    # Sampled last before the launch (#3163).
    $manifest.system_load.before_percent = Measure-SystemCpuLoad $loadSampleMs
}
catch { $manifestError = $_ }

$savedAffinity = $null
try {
    if ($null -ne $coreChoice.Mask) {
        try {
            $self = [System.Diagnostics.Process]::GetCurrentProcess()
            $original = $self.ProcessorAffinity
            $self.ProcessorAffinity = ConvertTo-AffinityPointer $coreChoice.Mask
            $savedAffinity = $original
        }
        catch {
            $coreChoice.Warning = "Pinning to logical CPUs $($coreChoice.LogicalCpus -join ',') failed, so the run is not pinned: $_"
            $coreChoice.CoreClass = 'any'
            $coreChoice.LogicalCpus = $null
            $coreChoice.Mask = $null
            $runMask = $processMask
            if ($null -ne $manifest) {
                $manifest.placement.core_class = 'any'
                $manifest.placement.logical_cpus = $null
                $manifest.placement.affinity_mask = Format-AffinityMask $runMask
            }
        }
    }
    if ($coreChoice.Warning) { Write-Warning $coreChoice.Warning }
    Write-Host (Get-PlacementDescription $coreChoice $runMask)
    Write-Host ""
    if ($null -ne $manifest -and -not $manifestError) {
        try { Save-BenchManifest $manifest }
        catch { $manifestError = $_ }
    }

    & $benchBin @benchArgs
    # $ErrorActionPreference does not cover a native command's exit code (#1423): read it explicitly.
    $benchExit = $LASTEXITCODE
    if ($null -eq $benchExit) { $benchExit = 0 }
}
finally {
    if ($null -ne $savedAffinity) { [System.Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity = $savedAffinity }
}

if ($null -ne $manifest -and -not $manifestError) {
    try {
        $manifest.exit_code = $benchExit
        $manifest.system_load.after_percent = Measure-SystemCpuLoad $loadSampleMs
        Save-BenchManifest $manifest
    }
    catch { $manifestError = $_ }
}
if ($null -ne $manifest -and $null -ne $manifest.system_load.before_percent) {
    Write-Host "System CPU load: $($manifest.system_load.before_percent)% before the run, $($manifest.system_load.after_percent)% after"
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
    # The benchmark's own exit code, as bench.sh exits with (#2254), not the 1 a throw gives.
    Write-Error -Message "Benchmark binary exited with code $benchExit; results in '$outFile' are not usable.$manifestNote" -ErrorAction Continue
    exit $benchExit
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

# -Baseline (#3448): the check, with any flagged benchmark measured across launches. Each further
# launch is this script again in a process of its own, with the same preset, binary, -Core and
# arguments plus the plan's --benchmark_filter (Google Benchmark takes the last one), so it is pinned,
# sampled, redacted and given a manifest the same way. Its output is shown and its RESULT= line read.
# The benchmark arguments go to it in a temporary file (-ExtraArgsFile, #3547), not on its command
# line: "pwsh -File" splits an argument that starts with "-" at its first ":" as -name:value, and
# most flagged names hold one (BM_X/iterations:1000). The preset, binary, directory and core stay on
# the command line: none of them can start with "-" (a "--" preset was moved into ExtraArgs above,
# the binary and directory are absolute paths, and -Core is validated). A process of its own, not
# & bench.ps1 in this one: the launch pins, sets the console encoding and exits or throws as a
# top-level run does, and none of that may reach this run or the launches after it.
$checkExit = 0
if ($Baseline) {
    $pwshExe = (Get-Process -Id $PID).Path
    $launchOne = {
        param([string]$Filter, [string]$Directory)
        $argsFile = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-bench-args-$([guid]::NewGuid().ToString('N')).json"
        try {
            [IO.File]::WriteAllText($argsFile, [System.Text.Json.JsonSerializer]::Serialize([string[]](@($ExtraArgs) + @("--benchmark_filter=$Filter")), [string[]], [System.Text.Json.JsonSerializerOptions]::new()), [Text.UTF8Encoding]::new($false))
            $childArgs = @('-NoProfile', '-File', $benchScriptPath, $Preset, '-BenchmarkBinary', $benchBin, '-OutputDirectory', $Directory, '-ExtraArgsFile', $argsFile)
            if ($Core) { $childArgs += @('-Core', $Core) }
            $lines = @(& $pwshExe @childArgs | ForEach-Object { Write-Host $_; $_ })
            $code = if ($null -eq $LASTEXITCODE) { 2 } else { $LASTEXITCODE }
        }
        finally {
            Remove-Item -LiteralPath $argsFile -Force -ErrorAction SilentlyContinue
        }
        $resultLine = @($lines | Where-Object { "$_" -like 'RESULT=*' } | Select-Object -Last 1)
        [pscustomobject]@{ ExitCode = $code; Result = $(if ($resultLine.Count -gt 0) { "$($resultLine[0])".Substring('RESULT='.Length) } else { $null }) }
    }
    $checkExit = Invoke-AcrossLaunches -Python $pythonExe -ToolsDirectory $scriptDir -Baseline $Baseline -First $outFile `
        -FirstManifest $manifestFile -WorkDirectory ([IO.Path]::ChangeExtension($outFile, '.across-launches')) `
        -Launches $Launches -BudgetSeconds $BudgetSeconds -CheckArgs $CheckArgs -Launch $launchOne
}

Write-Host ""
Write-Host "Results written to: $outFile"
Write-Host "Provenance manifest: $manifestFile"
if ($Baseline) {
    Write-Host "Checked against ${Baseline}: $(if ($checkExit -eq 0) { 'passed' } else { "failed (exit $checkExit)" })"
}
else {
    Write-Host "Compare against a baseline (median per benchmark, regressions over the threshold fail):"
    Write-Host "  python tools/check-benchmark-regression.py --baseline perf-data/win-baseline.json --current $outFile"
    Write-Host "  (or pass -Baseline, which measures any flagged benchmark across launches, #3448)"
}
# Machine-readable result path, printed only on success; kept in step with bench.sh (#2249). With
# -Baseline the run succeeded either way, and the exit code is the check's.
Write-Host "RESULT=$outFile"
exit $checkExit
