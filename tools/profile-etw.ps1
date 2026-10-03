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
    app   - launch TaskSmack.exe, then either wait for it to be closed manually (default) or
            run for a fixed window and close it automatically (-DurationSeconds)
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
            - app   -> win-optimized for production-like timings
            - bench -> win-benchmark for non-debug-info benchmark binaries
        Use win-profile explicitly when you want a symbol-rich frame-pointer build for
        deeper follow-up analysis.
.PARAMETER SkipBuild
    Skip configure/build and use the existing binaries.
.PARAMETER DurationSeconds
    Mode=resize/Phase=Collect: bounded recording duration (15-600 seconds; default 180).
    Mode=app only: instead of waiting indefinitely for a human to exercise and close the
    app, run it for this many seconds (idle - no interaction, so this captures background
    refresh/render cost, not click/scroll/hover load) and then close it automatically. Lets
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
$perfDir = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { Join-Path $repoRoot 'perf-data' } else { $OutputDirectory }
$wprProfilePath = Join-Path $scriptDir 'TaskSmackCPU.wprp'

if ($Mode -eq 'resize') {
    if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'resize capture requires PowerShell 7 (pwsh).' }
    . (Join-Path $scriptDir 'profile-etw-resize.ps1')
    Invoke-ResizeCapture -Phase $Phase -RunDirectory $RunDirectory -Preset $Preset -SkipBuild:$SkipBuild `
        -DurationSeconds $DurationSeconds -RepoRoot $repoRoot -Buffering $Buffering -ProviderSet $ProviderSet
    return
}

. (Join-Path $scriptDir 'profile-etw-common.ps1')

if ([string]::IsNullOrWhiteSpace($Preset)) {
    $Preset = if ($Mode -eq 'app') { 'win-optimized' } else { 'win-benchmark' }
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
$binaryPath = Join-Path $repoRoot "build/$Preset/bin/$binaryName"

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
    $result = [ordered]@{
        Binary         = $binaryPath
        Pid            = $null
        IntegrityLevel = 'Unknown'
        ExitCode       = $null
        EndReason      = $null
        StartUtc       = (Get-Date).ToUniversalTime().ToString('o')
        EndUtc         = $null
    }
    if ($Mode -eq 'app') {
        $proc = Start-Process -FilePath $binaryPath -PassThru
        $null = $proc.Handle # keep a handle so ExitCode is available after exit
        $result.Pid = $proc.Id
        $result.IntegrityLevel = Get-ProcessIntegrityLevel -ProcessId $proc.Id
        Write-Host "Launched app PID: $($proc.Id) (integrity: $($result.IntegrityLevel))"
        if ($DurationSeconds -gt 0) {
            Write-Host "Running for $DurationSeconds second(s), then closing automatically."
            Start-Sleep -Seconds $DurationSeconds
            if (-not $proc.HasExited) {
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                # Stop-Process returns before the process is gone; wait, so its status is final
                # and the trace is not stopped while it is still terminating.
                $null = $proc.WaitForExit(30000)
                $result.EndReason = 'closed by the script after -DurationSeconds (the exit code is from that forced stop)'
            }
        }
        else {
            Write-Host 'Exercise the application, then close it to finish the trace.'
            # Wait up to 4 hours; if the app crashes without exiting, this unblocks
            # so the caller can still save the recording.
            Wait-Process -Id $proc.Id -Timeout 14400 -ErrorAction SilentlyContinue
        }
        if ($proc.HasExited) {
            $result.ExitCode = $proc.ExitCode
            if (-not $result.EndReason) { $result.EndReason = 'exited' }
        }
        else {
            $result.EndReason = 'still running after the 4-hour wait'
        }
    }
    else {
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
    $result.EndUtc = (Get-Date).ToUniversalTime().ToString('o')
    return $result
}

function Assert-TargetSucceeded {
    # After the manifest is written: a benchmark run that exited nonzero, or an app still running
    # when the trace was stopped (past the 4-hour interactive wait), fails the capture -- the
    # trace ended before the workload did. (The app's exit code is not checked: -DurationSeconds
    # ends it with a forced stop.)
    param($Target)
    if ($null -eq $Target) { return }
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
        Timestamp         = $Timestamp
        Trace             = $tracePath
        Scenario          = if ($ElevatedTarget) { 'elevated-target (opt-in, -ElevatedTarget)' } else { 'normal-user target, elevated collector' }
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

function Invoke-WprCapture {
    # Starts WPR, runs $Body, and always stops WPR again if it was started.
    param([Parameter(Mandatory = $true)][scriptblock]$Body)
    $instanceName = "TaskSmack-$([guid]::NewGuid().ToString('N'))"
    $recordingStarted = $false
    try {
        Invoke-Native wpr '-start' "$wprProfilePath!TaskSmackCPU" '-filemode' '-instancename' $instanceName
        $recordingStarted = $true
        & $Body
        Invoke-Native wpr '-stop' $tracePath '-instancename' $instanceName
        $recordingStarted = $false
    }
    finally {
        if ($recordingStarted) {
            Invoke-Native wpr '-stop' $tracePath '-instancename' $instanceName
        }
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
    if (-not (Test-IsAdministrator)) { throw 'The elevated-run role must run elevated.' }
    Start-Transcript -Path $childLogPath -Force | Out-Null
    try {
        Write-Host "Starting ETW capture ($Mode) with an ELEVATED target (-ElevatedTarget)"
        Write-Host "Trace: $tracePath"
        # Not $script:elevatedTarget: PowerShell variable names are case-insensitive, so that would
        # be the -ElevatedTarget switch.
        Invoke-WprCapture { $script:capturedTarget = Invoke-ProfileTarget }
        $target = $script:capturedTarget
        $matchesForManifest = if ($Mode -eq 'bench') { Get-BenchmarkFilterMatches -BinaryPath $binaryPath -Filter $BenchmarkFilter } else { $null }
        Write-ProfileManifest -Target $target -BenchmarkMatches $matchesForManifest -CollectorElevated $true
        Write-Host "ETW_TRACE=$tracePath"
        Assert-TargetSucceeded $target
    }
    finally {
        Stop-Transcript | Out-Null
    }
    return
}

# ── Orchestrator: the normal entry point ─────────────────────────────────────────────────────
if ((Test-IsAdministrator) -and -not $ElevatedTarget) {
    throw "Run this from a normal (non-elevated) terminal. Only the WPR collector needs elevation, and this script elevates it on its own; a target launched from an elevated terminal would run elevated too, which changes what it measures (#872). To capture an elevated target deliberately, pass -ElevatedTarget."
}

if (-not $SkipBuild) {
    Invoke-Native cmake '--preset' $Preset
    Invoke-Native cmake '--build' '--preset' $Preset
}

# Validate everything before prompting for elevation so failures are immediate.
$null = Ensure-Tool 'wpr'
Ensure-Binary $wprProfilePath
Ensure-Binary $binaryPath

$benchmarkMatches = $null
if ($Mode -eq 'bench') {
    $benchmarkMatches = Get-BenchmarkFilterMatches -BinaryPath $binaryPath -Filter $BenchmarkFilter
    if ($benchmarkMatches.Severity -eq 'error') { throw $benchmarkMatches.Message }
    if ($benchmarkMatches.Severity -eq 'warning') { Write-Warning $benchmarkMatches.Message } else { Write-Host $benchmarkMatches.Message }
}

$hostExe = Get-HostExe
$commonArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSCommandPath", '-Mode', $Mode, '-Preset', $Preset, '-Timestamp', $Timestamp, '-BenchmarkFilter', $BenchmarkFilter, '-BenchmarkRepetitions', "$BenchmarkRepetitions", '-BenchmarkMinTime', $BenchmarkMinTime, '-DurationSeconds', "$DurationSeconds", '-OutputDirectory', $perfDir, '-SkipBuild')

if ($ElevatedTarget) {
    $argList = $commonArgs + @('-ElevatedTarget', '-Role', 'ElevatedRun')
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

    $collector = Start-Process -FilePath $hostExe -Verb RunAs -ArgumentList (ConvertTo-CommandLine $argList) -WorkingDirectory $repoRoot -PassThru
    $null = $collector.Handle
    $target = $null
    try {
        Wait-CollectorMarker -Path (Join-Path $controlDir 'collector-started.json') -TimeoutSeconds 120 -Collector $collector -What 'ETW collector'
        Write-Host "ETW collector started; trace: $tracePath"
        $target = Invoke-ProfileTarget
    }
    finally {
        # Always ask the collector to stop, so a failed target still leaves a saved trace.
        Set-Content -LiteralPath (Join-Path $controlDir 'stop-requested') -Value (Get-Date).ToUniversalTime().ToString('o') -Encoding utf8
        $collectorExited = $collector.WaitForExit(600000)
    }
    if (-not $collectorExited) {
        # Its exit code is not available while it runs; say so rather than read a stale value.
        "EXIT_CODE=still running" | Add-Content -Path $launcherLogPath -Encoding utf8
        throw "The elevated ETW collector did not exit within 10 minutes of the stop request; the trace may not be saved. Check $childLogPath, and close the collector window when it finishes.$(Get-CollectorErrorDetail -ControlDirectory $controlDir)"
    }
    "EXIT_CODE=$($collector.ExitCode)" | Add-Content -Path $launcherLogPath -Encoding utf8
    if ($null -ne $target) {
        Write-ProfileManifest -Target $target -BenchmarkMatches $benchmarkMatches -CollectorElevated $true
    }
    if ($collector.ExitCode -ne 0) {
        throw "Elevated ETW collector failed with exit code $($collector.ExitCode).$(Get-CollectorErrorDetail -ControlDirectory $controlDir) Check $launcherLogPath and $childLogPath."
    }
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
