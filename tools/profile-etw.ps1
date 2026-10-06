<#
.SYNOPSIS
    Capture a reusable ETW CPU trace for TaskSmack on Windows.
.DESCRIPTION
    Starts a WPR CPU trace, runs either the real app or a focused benchmark workload, then stops
    tracing and writes artifacts under perf-data/.

    Only the WPR collector is elevated. Run this from a normal (non-elevated) terminal: it
    launches an elevated collector child (one UAC prompt) that starts and stops WPR, and runs the
    target itself at ordinary-user integrity. Elevating the target changes what it can see and do
    -- process access, handle and EStats data, and so the workload being measured -- so an
    elevated target is an A/B confound, not a permissions detail (#872). The target's measured
    integrity level is recorded in the run manifest. -ElevatedTarget opts into the old
    fully-elevated capture as a separately labelled scenario.

    Uses tools/TaskSmackCPU.wprp (a custom WPR profile with larger buffers than the
    built-in "CPU" profile) rather than "wpr -start CPU" directly: every capture taken with
    the plain built-in profile on a typical dev machine (16 logical cores, system-wide
    sampling) reported hundreds of thousands of dropped events ("This trace has dropped N
    events. Please record this trace again."), which silently degrades every downstream
    analysis. The custom profile cuts that by roughly an order of magnitude or more (still
    subject to run-to-run system-load variance, so occasional drops are expected, just far
    fewer) - see TaskSmackCPU.wprp's own comment for where its buffer sizing came from.
.PARAMETER Mode
    app   - launch TaskSmack.exe, let it warm up (see -WarmupSeconds), start the trace, then
            either wait for it to be closed manually (default) or record for a fixed window
            and close it automatically (-DurationSeconds). The capture fails, without printing
            TRACE=, if the app exits during warm-up, exits before a -DurationSeconds window ends,
            or is closed with a nonzero exit code (#1186).
    bench - run TaskSmackBenchmarks.exe with the supplied benchmark filter
    resize - separated normal-user app / elevated collector diagnostic capture (#882).
.PARAMETER Phase
    resize mode only: Prepare snapshots binaries, Collect records ETW in a separate elevated
    terminal, App launches normally with stdout/stderr, Save asks a -Buffering Ring collector to
    write its buffer now, Check verifies overlap and exports loss diagnostics.
.PARAMETER RunDirectory
    resize mode only: unique directory shared by all four phases; must not exist at Prepare.
.PARAMETER Preset
    Build preset that contains the binaries to run. Defaults depend on mode:
        - app   -> win-release, the preset release builds ship (.github/workflows/release.yml),
                   so the profile measures the binary users run (#1186)
        - bench -> win-benchmark for non-debug-info benchmark binaries
    The preset and the compile flags its build tree was configured with are logged at the start
    of the run and recorded in the manifest. Pass win-profile for a symbol-rich frame-pointer
    build for deeper follow-up attribution, or win-optimized (LTO, -march=x86-64-v3,
    -fomit-frame-pointer) to profile that opt-in build -- it is not what ships.
.PARAMETER WarmupSeconds
    app mode: after the app's main window appears (waiting up to -MainWindowTimeoutSeconds), wait
    this many more seconds before starting the trace, so startup (font, theme, first process
    enumeration) is not in the profile. Default 5. The app exiting during warm-up fails the run.
.PARAMETER IncludeStartup
    app mode: start the trace before launching the app, so startup is recorded deliberately (no
    warm-up).
.PARAMETER MainWindowTimeoutSeconds
    app mode: how long warm-up waits for the main window before warning and continuing. Default 60.
.PARAMETER SkipTrace
    app mode: dry run of the launch/warm-up/monitor logic without WPR, the elevated collector or a
    UAC prompt. Nothing is recorded; the manifest is written and the run fails or succeeds as a
    capture would, printing DRY_RUN=ok instead of TRACE= on success. Used by
    tools/test-profile-etw.ps1.
.PARAMETER SkipBuild
    Skip configure/build and use the existing binaries.
.PARAMETER DurationSeconds
    Mode=resize/Phase=Collect: bounded recording duration (15-600 seconds; default 180).
    Mode=app only: instead of waiting indefinitely for a human to exercise and close the
    app, record for this many seconds after warm-up (idle - no interaction, so this captures
    background refresh/render cost, not click/scroll/hover load), stop the trace and then close
    the app automatically. The app must still be running at the end of the window. Lets
    an app-mode capture run unattended/scripted. Omit (0) to keep the default interactive
    behavior (wait for manual close, useful when you actually want to exercise the UI).
.PARAMETER BenchmarkFilter
    Google Benchmark filter used when Mode=bench. Defaults to a curated set covering every
    probe/model refresh path (Process/System/GPU/Storage/Disk) plus the PDH per-process GPU
    path and core History container operations - deliberately excludes MemoryGrowth-style
    benchmarks (measure allocation, not CPU hotspots) and the pure-algorithmic Format/Numeric
    micro-benchmarks (better suited to bench.ps1's regression tracking than ETW correlation).
    Pass '.*' to profile the entire benchmark suite instead.

    The filter is checked before capturing (#874), as tools/profile-perf.sh does on Linux: a
    filter matching no benchmark fails, and one matching several warns, because each benchmark
    gets the same minimum time, so a cheap one is looped more and gets as many samples as an
    expensive one. For hotspot attribution, match exactly one benchmark. The matched names are
    recorded in the run manifest.
.PARAMETER BenchmarkRepetitions
    Repetition count for benchmark-mode capture.
.PARAMETER BenchmarkMinTime
    Minimum time per benchmark repetition for benchmark-mode capture.
.PARAMETER ElevatedTarget
    app/bench modes: run the target elevated too, as the whole capture used to (#872). For
    comparing against an elevated run deliberately; the artifacts are named etw-<mode>-elevated-*
    and the manifest records the target's measured integrity, so the two cannot be confused.
.EXAMPLE
    pwsh tools/profile-etw.ps1 app
.EXAMPLE
    pwsh tools/profile-etw.ps1 app -DurationSeconds 45
.EXAMPLE
    pwsh tools/profile-etw.ps1 bench -BenchmarkFilter 'BM_SystemModel_Refresh$'
.EXAMPLE
    pwsh tools/profile-etw.ps1 bench -BenchmarkFilter '.*'
.EXAMPLE
    pwsh tools/profile-etw.ps1 app -Preset win-profile
.EXAMPLE
    pwsh tools/profile-etw.ps1 app -DurationSeconds 45 -WarmupSeconds 15
.EXAMPLE
    pwsh tools/profile-etw.ps1 app -DurationSeconds 20 -IncludeStartup
.EXAMPLE
    pwsh tools/profile-etw.ps1 app -DurationSeconds 45 -ElevatedTarget
.EXAMPLE
    pwsh tools/profile-etw.ps1 resize -Phase Prepare -RunDirectory .\perf-data\resize-001 -Preset win-profile
#>
[CmdletBinding()]
param(
    [ValidateSet('app', 'bench', 'resize')]
    [string]$Mode = 'app',

    [string]$Preset = '',

    [string]$Timestamp,

    [ValidateSet('Prepare', 'Collect', 'App', 'Save', 'Check')]
    [string]$Phase = 'Prepare',

    # resize mode only: File records straight to disk for a fixed window; Ring records into
    # WPR's in-memory buffer until Save is requested, so a rare stall can be captured after it
    # is observed instead of having to occur inside a fixed window.
    [ValidateSet('File', 'Ring')]
    [string]$Buffering = 'File',

    # resize mode only: Focused uses tools/TaskSmackResize.wprp, which keeps the scheduler and
    # sampling data attribution needs without the provider volume that made the earlier set
    # account for ~48% of a measured stall's own CPU (#912). Verbose is the wider legacy set.
    [ValidateSet('Focused', 'Verbose')]
    [string]$ProviderSet = 'Focused',

    [string]$RunDirectory,

    [switch]$SkipBuild,

    [int]$DurationSeconds = 0,

    [ValidateRange(0, 3600)]
    [int]$WarmupSeconds = 5,

    [switch]$IncludeStartup,

    [ValidateRange(0, 3600)]
    [int]$MainWindowTimeoutSeconds = 60,

    [switch]$SkipTrace,

    # Internal (tests): run this executable instead of the preset's TaskSmack.exe, with a hidden
    # window.
    [string]$TargetPath,

    [string]$BenchmarkFilter = 'BM_(ProcessProbe_Enumerate|ProcessModel_Refresh|SystemProbe_Sample|SystemModel_Refresh|GPUProbe_ReadCounters|GPUModel_Refresh|GPUModel_ProcessGpuCounters|PDHGPUProbe_ReadProcessGPUCounters|DiskProbe_Read|StorageModel_Sample|History_(Push|RandomAccess|SequentialAccess|CopyTo))$',

    [int]$BenchmarkRepetitions = 5,

    [string]$BenchmarkMinTime = '0.5s',

    [switch]$ElevatedTarget,

    # Internal: how this instance was launched. Orchestrator is the normal-user entry point;
    # Collector is its elevated WPR child; ElevatedRun is the -ElevatedTarget elevated child.
    [ValidateSet('Orchestrator', 'Collector', 'ElevatedRun')]
    [string]$Role = 'Orchestrator',

    # Internal: Collector role only, the directory the orchestrator and collector signal through.
    [string]$ControlDirectory,

    # app/bench modes: where the trace, logs, manifest and benchmark JSON are written. Defaults to
    # perf-data/ in the repository.
    [string]$OutputDirectory,

    # Internal (tests): overrides how long the collector waits for the stop request.
    [int]$CollectorTimeoutSeconds = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
$wprProfilePath = Join-Path $scriptDir 'TaskSmackCPU.wprp'

if ($Mode -eq 'resize') {
    if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'resize capture requires PowerShell 7 (pwsh).' }
    . (Join-Path $scriptDir 'profile-etw-resize.ps1')
    Invoke-ResizeCapture -Phase $Phase -RunDirectory $RunDirectory -Preset $Preset -SkipBuild:$SkipBuild `
        -DurationSeconds $DurationSeconds -RepoRoot $repoRoot -Buffering $Buffering -ProviderSet $ProviderSet
    return
}

. (Join-Path $scriptDir 'profile-etw-common.ps1')
# Absolute before anything is created or passed on (see Resolve-CaptureOutputDirectory).
$perfDir = Resolve-CaptureOutputDirectory -OutputDirectory $OutputDirectory -RepoRoot $repoRoot

if ([string]::IsNullOrWhiteSpace($Preset)) {
    # win-release is what .github/workflows/release.yml builds and ships (#1186).
    $Preset = if ($Mode -eq 'app') { 'win-release' } else { 'win-benchmark' }
}
if ($SkipTrace -and $Mode -ne 'app') { throw '-SkipTrace is supported in app mode only.' }
if ($Mode -ne 'app' -and ($IncludeStartup -or $PSBoundParameters.ContainsKey('WarmupSeconds'))) {
    throw '-WarmupSeconds and -IncludeStartup apply to app mode only.'
}

if (-not $Timestamp) {
    $Timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
}

# An elevated-target capture is a different scenario, so its artifacts are named apart (#872).
$prefix = if ($Mode -eq 'app') { 'etw-app' } else { 'etw-bench' }
if ($ElevatedTarget) { $prefix = "$prefix-elevated" }
$tracePath = Join-Path $perfDir "$prefix-$Timestamp.etl"
$childLogPath = Join-Path $perfDir "$prefix-child-$Timestamp.log"
$launcherLogPath = Join-Path $perfDir "$prefix-launch-$Timestamp.log"
$benchJsonPath = Join-Path $perfDir "$prefix-$Timestamp.json"
$manifestPath = Join-Path $perfDir "$prefix-$Timestamp.manifest.json"

$binaryName = if ($Mode -eq 'app') { 'TaskSmack.exe' } else { 'TaskSmackBenchmarks.exe' }
$buildDir = Join-Path $repoRoot "build/$Preset"
$binaryPath = if ($TargetPath) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($TargetPath) } else { Join-Path $buildDir "bin/$binaryName" }

# The collector waits at most this long for the stop request: a fixed-duration app run plus
# slack, or else the 4-hour interactive allowance plus slack (a benchmark run can legitimately
# take long with large -BenchmarkRepetitions/-BenchmarkMinTime). Reaching it fails the capture.
$collectorTimeoutSeconds = if ($CollectorTimeoutSeconds -gt 0) { $CollectorTimeoutSeconds }
    elseif ($Mode -eq 'app' -and $DurationSeconds -gt 0) { $DurationSeconds + 600 }
    else { 14400 + 600 }

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Exe,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Exe $($Arguments -join ' ')"
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Ensure-Tool {
    param([Parameter(Mandatory = $true)][string]$Command)
    $cmd = Get-Command $Command -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "Required command not found on PATH: $Command"
    }
    return $cmd.Source
}

function Ensure-Binary {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Required binary not found: $Path"
    }
}

function Get-HostExe {
    # Relaunch using the current PowerShell host (pwsh or powershell) for consistent behavior.
    $hostExe = (Get-Process -Id $PID).Path
    if ([string]::IsNullOrWhiteSpace($hostExe)) {
        throw 'Unable to resolve current PowerShell host executable path for elevation.'
    }
    return $hostExe
}

function Invoke-ProfileTarget {
    # Runs the app or benchmark workload from the current process's token and measures the
    # integrity level it actually got, rather than inferring it from the launcher (#872).
    # StartTrace/StopTrace start and stop the recording: around the whole benchmark run, and in
    # app mode after the warm-up (Invoke-AppCapture, #1186).
    param(
        [Parameter(Mandatory = $true)][scriptblock]$StartTrace,
        [Parameter(Mandatory = $true)][scriptblock]$StopTrace
    )
    if ($Mode -eq 'app') {
        $windowStyle = if ($TargetPath) { 'Hidden' } else { 'Normal' }
        return Invoke-AppCapture -BinaryPath $binaryPath -StartTrace $StartTrace -StopTrace $StopTrace `
            -DurationSeconds $DurationSeconds -WarmupSeconds $WarmupSeconds -IncludeStartup:$IncludeStartup `
            -MainWindowTimeoutSeconds $MainWindowTimeoutSeconds -WindowStyle $windowStyle
    }
    $result = [ordered]@{
        Binary         = $binaryPath
        Pid            = $null
        IntegrityLevel = 'Unknown'
        ExitCode       = $null
        EndReason      = $null
        StartUtc       = (Get-Date).ToUniversalTime().ToString('o')
        EndUtc         = $null
    }
    & $StartTrace | Out-Host
    try {
        $benchArgs = @("--benchmark_filter=$BenchmarkFilter", "--benchmark_repetitions=$BenchmarkRepetitions", "--benchmark_min_time=$BenchmarkMinTime", '--benchmark_report_aggregates_only=true', '--benchmark_display_aggregates_only=true', "--benchmark_out=$benchJsonPath", '--benchmark_out_format=json')
        $proc = Start-Process -FilePath $binaryPath -ArgumentList (ConvertTo-CommandLine $benchArgs) -NoNewWindow -PassThru
        $null = $proc.Handle
        $result.Pid = $proc.Id
        $result.IntegrityLevel = Get-ProcessIntegrityLevel -ProcessId $proc.Id
        Write-Host "Launched benchmarks PID: $($proc.Id) (integrity: $($result.IntegrityLevel))"
        $proc.WaitForExit()
        $result.ExitCode = $proc.ExitCode
        $result.EndReason = 'exited'
        # A failure is returned, not thrown, so the caller still writes the manifest (PID,
        # integrity, exit code) once the trace is saved, and fails after that (Assert-TargetSucceeded).
        if ($proc.ExitCode -eq 0) { Write-Host "Benchmark JSON: $benchJsonPath" }
    }
    finally {
        & $StopTrace | Out-Host
    }
    $result.EndUtc = (Get-Date).ToUniversalTime().ToString('o')
    return $result
}

function Assert-TargetSucceeded {
    # After the manifest is written: an app run that did not cover what was asked (Invoke-AppCapture's
    # .Failure: it exited during warm-up or before the -DurationSeconds window ended, or was closed
    # with a nonzero exit code), a benchmark run that exited nonzero, or an app still running
    # when the trace was stopped (see Get-UnfinishedTargetReason), fails the capture. (After a
    # full -DurationSeconds window the app's exit code is not checked: the script's forced stop
    # sets it.)
    param($Target)
    if ($null -eq $Target) { return }
    if ($Target.Contains('Failure') -and $Target.Failure) {
        throw "Capture failed: $($Target.Failure) The manifest ($manifestPath) and any partial trace are kept for inspection; no trace is reported."
    }
    if ($Mode -eq 'bench' -and $Target.ExitCode -ne 0) {
        throw "Benchmark binary failed with exit code $($Target.ExitCode): $binaryPath. The trace and manifest ($manifestPath) are kept for inspection."
    }
    if ($null -eq $Target.ExitCode) {
        throw "The target (PID $($Target.Pid)) was still running when the trace was stopped ($($Target.EndReason)), so the trace is truncated. The trace and manifest ($manifestPath) are kept for inspection."
    }
}

function Write-ProfileManifest {
    param($Target, $BenchmarkMatches, [bool]$CollectorElevated)
    $manifest = [ordered]@{
        Mode              = $Mode
        Preset            = $Preset
        BuildFlags        = Get-PresetBuildFlags -BuildDirectory $buildDir
        Timestamp         = $Timestamp
        Trace             = if ($SkipTrace) { $null } else { $tracePath }
        Scenario          = if ($SkipTrace) { 'dry run (-SkipTrace): nothing recorded' } elseif ($ElevatedTarget) { 'elevated-target (opt-in, -ElevatedTarget)' } else { 'normal-user target, elevated collector' }
        CollectorElevated = $CollectorElevated
        Target            = $Target
    }
    if ($Mode -eq 'bench') {
        $manifest.BenchmarkFilter = $BenchmarkFilter
        # [string[]] so a single match is still written as a JSON list, not unrolled to a string.
        $manifest.BenchmarkMatches = [string[]]@(if ($BenchmarkMatches) { $BenchmarkMatches.Names })
        $manifest.BenchmarkMatchSeverity = if ($BenchmarkMatches) { $BenchmarkMatches.Severity } else { 'unchecked' }
        $manifest.BenchmarkJson = $benchJsonPath
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
}

function Start-WprRecording {
    # A unique instance name, so an existing recording is never touched.
    $script:wprInstanceName = "TaskSmack-$([guid]::NewGuid().ToString('N'))"
    Invoke-Native wpr '-start' "$wprProfilePath!TaskSmackCPU" '-filemode' '-instancename' $script:wprInstanceName
}

function Stop-WprRecording {
    Invoke-Native wpr '-stop' $tracePath '-instancename' $script:wprInstanceName
}

function Invoke-WprCapture {
    # Starts WPR, runs $Body, and always stops WPR again if it was started.
    param([Parameter(Mandatory = $true)][scriptblock]$Body)
    $recordingStarted = $false
    try {
        Start-WprRecording
        $recordingStarted = $true
        & $Body
        $recordingStarted = $false
        Stop-WprRecording
    }
    finally {
        if ($recordingStarted) { Stop-WprRecording }
    }
}

New-Item -ItemType Directory -Path $perfDir -Force | Out-Null

# ── Collector role: the elevated child that only starts and stops WPR (#872) ────────────────
# It runs no binary and no command from the control directory; it only waits for the
# orchestrator's stop request.
if ($Role -eq 'Collector') {
    if ([string]::IsNullOrWhiteSpace($ControlDirectory) -or -not (Test-Path -LiteralPath $ControlDirectory)) {
        throw "Collector control directory not found: $ControlDirectory"
    }
    $startedMarker = Join-Path $ControlDirectory 'collector-started.json'
    $stopMarker = Join-Path $ControlDirectory 'stop-requested'
    $doneMarker = Join-Path $ControlDirectory 'collector-done.json'
    # This window closes when the collector exits, so a failure is also written where the
    # orchestrator reports it from (Wait-CollectorMarker).
    $errorMarker = Join-Path $ControlDirectory 'collector-error.txt'
    if (-not (Test-IsAdministrator)) {
        $message = 'The collector role must run elevated, but its token is not an administrator token. Run profile-etw.ps1 from a normal terminal of an administrator account (not a restricted token such as runas /trustlevel), so the UAC prompt can elevate it.'
        Set-Content -LiteralPath $errorMarker -Value $message -Encoding utf8
        throw $message
    }
    Start-Transcript -Path $childLogPath -Force | Out-Null
    try {
        Write-Host "Starting ETW collector ($Mode); trace: $tracePath"
        $script:collectorTimedOut = $false
        Invoke-WprCapture {
            @{ StartUtc = (Get-Date).ToUniversalTime().ToString('o') } | ConvertTo-Json | Set-Content -LiteralPath $startedMarker -Encoding utf8
            $deadline = (Get-Date).AddSeconds($collectorTimeoutSeconds)
            while (-not (Test-Path -LiteralPath $stopMarker)) {
                if ((Get-Date) -gt $deadline) {
                    $script:collectorTimedOut = $true
                    break
                }
                Start-Sleep -Milliseconds 250
            }
        }
        # The trace is saved either way, but one cut off before the target finished is incomplete
        # and must not be reported as a successful capture.
        if ($script:collectorTimedOut) {
            throw "No stop request within $collectorTimeoutSeconds s; the trace was saved but is truncated, so the capture failed."
        }
        @{ StopUtc = (Get-Date).ToUniversalTime().ToString('o'); Trace = $tracePath } | ConvertTo-Json | Set-Content -LiteralPath $doneMarker -Encoding utf8
        Write-Host "ETW_TRACE=$tracePath"
    }
    catch {
        Set-Content -LiteralPath $errorMarker -Value ($_ | Out-String) -Encoding utf8 -ErrorAction SilentlyContinue
        throw
    }
    finally {
        Stop-Transcript | Out-Null
    }
    return
}

# ── ElevatedRun role: the opt-in fully-elevated capture (-ElevatedTarget) ────────────────────
if ($Role -eq 'ElevatedRun') {
    # Without the switch the artifacts and manifest would be named and labelled as a normal-user
    # capture while the target runs elevated.
    if (-not $ElevatedTarget) { throw 'The elevated-run role requires -ElevatedTarget, so an elevated capture is never labelled as a normal-user one.' }
    if (-not (Test-IsAdministrator)) { throw 'The elevated-run role must run elevated.' }
    Start-Transcript -Path $childLogPath -Force | Out-Null
    try {
        Write-Host "Starting ETW capture ($Mode) with an ELEVATED target (-ElevatedTarget)"
        Write-Host "Trace: $tracePath"
        # Not $script:elevatedTarget: PowerShell variable names are case-insensitive, so that would
        # be the -ElevatedTarget switch.
        $target = Invoke-ProfileTarget -StartTrace { Start-WprRecording } -StopTrace { Stop-WprRecording }
        $matchesForManifest = if ($Mode -eq 'bench') { Get-BenchmarkFilterMatches -BinaryPath $binaryPath -Filter $BenchmarkFilter } else { $null }
        Write-ProfileManifest -Target $target -BenchmarkMatches $matchesForManifest -CollectorElevated $true
        # A failed run (e.g. the app crashed) throws here, before the trace is reported.
        Assert-TargetSucceeded $target
        Write-Host "ETW_TRACE=$tracePath"
    }
    finally {
        Stop-Transcript | Out-Null
    }
    return
}

# ── Orchestrator: the normal entry point ─────────────────────────────────────────────────────
if ((Test-IsAdministrator) -and -not $ElevatedTarget -and -not $SkipTrace) {
    throw "Run this from a normal (non-elevated) terminal. Only the WPR collector needs elevation, and this script elevates it on its own; a target launched from an elevated terminal would run elevated too, which changes what it measures (#872). To capture an elevated target deliberately, pass -ElevatedTarget."
}

if (-not $SkipBuild) {
    Invoke-Native cmake '--preset' $Preset
    Invoke-Native cmake '--build' '--preset' $Preset
}

# Say which build is measured: the preset and the flags its tree was configured with (#1186).
Write-Host (Format-PresetBuildFlags -Preset $Preset -Flags (Get-PresetBuildFlags -BuildDirectory $buildDir))
Write-Host "Binary: $binaryPath"
if ($Mode -eq 'app') {
    if ($IncludeStartup) { Write-Host 'Startup: recorded (-IncludeStartup); the trace starts before the app is launched.' }
    else { Write-Host "Startup: excluded; the trace starts once the main window exists plus $WarmupSeconds s of warm-up." }
}

# Validate everything before prompting for elevation so failures are immediate.
if (-not $SkipTrace) { $null = Ensure-Tool 'wpr' }
Ensure-Binary $wprProfilePath
Ensure-Binary $binaryPath

# ── Dry run (-SkipTrace): the app lifecycle and its failure checks, with nothing recorded ────
if ($SkipTrace) {
    Write-Warning '-SkipTrace: dry run. No WPR session is started and nothing is recorded.'
    $target = Invoke-ProfileTarget -StartTrace { Write-Host 'DRY RUN: the trace would start now.' } -StopTrace { Write-Host 'DRY RUN: the trace would stop now.' }
    Write-ProfileManifest -Target $target -BenchmarkMatches $null -CollectorElevated $false
    Assert-TargetSucceeded $target
    Write-Host 'DRY_RUN=ok'
    Write-Host "MANIFEST=$manifestPath"
    Write-Host "TARGET_INTEGRITY=$($target.IntegrityLevel)"
    Write-Host "PRESET=$Preset"
    return
}

$benchmarkMatches = $null
if ($Mode -eq 'bench') {
    $benchmarkMatches = Get-BenchmarkFilterMatches -BinaryPath $binaryPath -Filter $BenchmarkFilter
    if ($benchmarkMatches.Severity -eq 'error') { throw $benchmarkMatches.Message }
    if ($benchmarkMatches.Severity -eq 'warning') { Write-Warning $benchmarkMatches.Message } else { Write-Host $benchmarkMatches.Message }
}

$hostExe = Get-HostExe
$commonArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSCommandPath", '-Mode', $Mode, '-Preset', $Preset, '-Timestamp', $Timestamp, '-BenchmarkFilter', $BenchmarkFilter, '-BenchmarkRepetitions', "$BenchmarkRepetitions", '-BenchmarkMinTime', $BenchmarkMinTime, '-DurationSeconds', "$DurationSeconds", '-OutputDirectory', $perfDir, '-SkipBuild')

if ($ElevatedTarget) {
    # The elevated child runs the app itself, so it needs the warm-up settings too.
    $argList = $commonArgs + @('-ElevatedTarget', '-Role', 'ElevatedRun')
    if ($Mode -eq 'app') {
        $argList += @('-WarmupSeconds', "$WarmupSeconds", '-MainWindowTimeoutSeconds', "$MainWindowTimeoutSeconds")
        if ($IncludeStartup) { $argList += '-IncludeStartup' }
    }
    "LAUNCH=$hostExe $($argList -join ' ')" | Set-Content -Path $launcherLogPath -Encoding utf8
    if (Test-IsAdministrator) {
        & $hostExe @argList
        $childExit = $LASTEXITCODE
    }
    else {
        $childProcess = Start-Process -FilePath $hostExe -Verb RunAs -ArgumentList (ConvertTo-CommandLine $argList) -WorkingDirectory $repoRoot -Wait -PassThru
        $childExit = $childProcess.ExitCode
    }
    "EXIT_CODE=$childExit" | Add-Content -Path $launcherLogPath -Encoding utf8
    if ($childExit -ne 0) {
        throw "Elevated ETW capture child process failed with exit code $childExit. Check $launcherLogPath and $childLogPath when present."
    }
}
else {
    $controlDir = Join-Path $perfDir "$prefix-control-$Timestamp"
    # Never reuse one: a failed run leaves its directory behind, and its stale
    # collector-started/stop-requested markers would let the target start before this run's WPR
    # session does, or stop the collector at once. Same rule as the resize run directory.
    if (Test-Path -LiteralPath $controlDir) {
        throw "Capture control directory already exists: $controlDir. It is left by an earlier run with -Timestamp $Timestamp; pass a different -Timestamp, or delete the directory once that run is no longer needed."
    }
    New-Item -ItemType Directory -Path $controlDir | Out-Null
    $argList = $commonArgs + @('-Role', 'Collector', '-ControlDirectory', $controlDir)
    "LAUNCH=$hostExe $($argList -join ' ')" | Set-Content -Path $launcherLogPath -Encoding utf8

    # The collector is started by Invoke-ProfileTarget: for an app capture only after the app has
    # warmed up (#1186), and stopped as soon as the capture ends -- also when the app crashes.
    $script:collector = $null
    $script:collectorStopRequested = $false
    $script:collectorExited = $true
    $startCollector = {
        if ($Mode -eq 'app') { Write-Host 'Starting the elevated ETW collector; approve the UAC prompt to begin recording.' }
        $script:collector = Start-Process -FilePath $hostExe -Verb RunAs -ArgumentList (ConvertTo-CommandLine $argList) -WorkingDirectory $repoRoot -PassThru
        $null = $script:collector.Handle
        Wait-CollectorMarker -Path (Join-Path $controlDir 'collector-started.json') -TimeoutSeconds 120 -Collector $script:collector -What 'ETW collector'
        Write-Host "ETW collector started; trace: $tracePath"
    }
    $stopCollector = {
        # Once only; and only if it was launched.
        if ($null -eq $script:collector -or $script:collectorStopRequested) { return }
        $script:collectorStopRequested = $true
        Set-Content -LiteralPath (Join-Path $controlDir 'stop-requested') -Value (Get-Date).ToUniversalTime().ToString('o') -Encoding utf8
        $script:collectorExited = $script:collector.WaitForExit(600000)
    }
    $target = $null
    $pendingError = $null
    try {
        $target = Invoke-ProfileTarget -StartTrace $startCollector -StopTrace $stopCollector
    }
    catch {
        # Held until the collector's status is known, so a collector failure is reported with it.
        $pendingError = $_
    }
    finally {
        # Always ask a launched collector to stop, so a failed target still leaves a saved trace.
        & $stopCollector
    }
    $collector = $script:collector
    $collectorExited = $script:collectorExited
    $collectorFailure = $null
    if ($null -eq $collector) {
        # Never launched: the app failed during warm-up, before recording would have started.
        "EXIT_CODE=collector not started" | Add-Content -Path $launcherLogPath -Encoding utf8
        if ($null -ne $target) {
            Write-ProfileManifest -Target $target -BenchmarkMatches $benchmarkMatches -CollectorElevated $false
        }
    }
    elseif (-not $collectorExited) {
        # Its exit code is not available while it runs; say so rather than read a stale value.
        "EXIT_CODE=still running" | Add-Content -Path $launcherLogPath -Encoding utf8
        $collectorFailure = "The elevated ETW collector did not exit within 10 minutes of the stop request; the trace may not be saved. Check $childLogPath, and close the collector window when it finishes.$(Get-CollectorErrorDetail -ControlDirectory $controlDir)"
    }
    else {
        "EXIT_CODE=$($collector.ExitCode)" | Add-Content -Path $launcherLogPath -Encoding utf8
        if ($null -ne $target) {
            Write-ProfileManifest -Target $target -BenchmarkMatches $benchmarkMatches -CollectorElevated $true
        }
        if ($collector.ExitCode -ne 0) {
            $collectorFailure = "Elevated ETW collector failed with exit code $($collector.ExitCode).$(Get-CollectorErrorDetail -ControlDirectory $controlDir) Check $launcherLogPath and $childLogPath."
        }
    }
    Assert-CollectorOutcome -PendingError $pendingError -CollectorFailure $collectorFailure
    Remove-Item -LiteralPath $controlDir -Recurse -Force -ErrorAction SilentlyContinue
    Assert-TargetSucceeded $target
}

$missingArtifacts = @()
foreach ($artifact in @($childLogPath, $tracePath, $manifestPath)) {
    if (-not (Test-Path -LiteralPath $artifact)) { $missingArtifacts += $artifact }
}
if (($Mode -eq 'bench') -and (-not (Test-Path -LiteralPath $benchJsonPath))) {
    $missingArtifacts += $benchJsonPath
}
if ($missingArtifacts.Count -gt 0) {
    $artifactList = $missingArtifacts -join ', '
    throw "ETW capture did not produce expected artifact(s): $artifactList. This usually means the UAC prompt was denied or the elevated child failed before trace shutdown. Check $launcherLogPath and $childLogPath when present."
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
Write-Host "TRACE=$tracePath"
Write-Host "MANIFEST=$manifestPath"
Write-Host "TARGET_INTEGRITY=$($manifest.Target.IntegrityLevel)"
Write-Host "CHILD_LOG=$childLogPath"
Write-Host "LAUNCHER_LOG=$launcherLogPath"
Write-Host "PRESET=$Preset"
if ($Mode -eq 'bench') {
    Write-Host "BENCH=$benchJsonPath"
}
