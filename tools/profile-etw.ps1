<#
.SYNOPSIS
    Capture a reusable ETW CPU trace for TaskSmack on Windows.
.DESCRIPTION
    Self-elevates if needed, starts a WPR CPU trace, runs either the real app or a
    focused benchmark workload, then stops tracing and writes artifacts under perf-data/.
.PARAMETER Mode
    app   - launch TaskSmack.exe and wait until it exits
    bench - run TaskSmackBenchmarks.exe with the supplied benchmark filter
.PARAMETER Preset
        Build preset that contains the binaries to run. Defaults depend on mode:
            - app   -> win-optimized for production-like timings
            - bench -> win-benchmark for non-debug-info benchmark binaries
        Use win-profile explicitly when you want a symbol-rich frame-pointer build for
        deeper follow-up analysis.
.PARAMETER SkipBuild
    Skip configure/build and use the existing binaries.
.PARAMETER BenchmarkFilter
    Google Benchmark filter used when Mode=bench.
.PARAMETER BenchmarkRepetitions
    Repetition count for benchmark-mode capture.
.PARAMETER BenchmarkMinTime
    Minimum time per benchmark repetition for benchmark-mode capture.
.EXAMPLE
    pwsh tools/profile-etw.ps1 app
.EXAMPLE
    pwsh tools/profile-etw.ps1 bench -BenchmarkFilter 'BM_SystemModel_Refresh$'
.EXAMPLE
    pwsh tools/profile-etw.ps1 app -Preset win-profile
#>
[CmdletBinding()]
param(
    [ValidateSet('app', 'bench')]
    [string]$Mode = 'app',

    [string]$Preset = '',

    [string]$Timestamp,

    [switch]$SkipBuild,

    [string]$BenchmarkFilter = 'BM_(ProcessProbe_Enumerate|ProcessModel_Refresh|SystemProbe_Sample|SystemModel_Refresh|GPUProbe_ReadCounters|GPUModel_Refresh)$',

    [int]$BenchmarkRepetitions = 5,

    [string]$BenchmarkMinTime = '0.5s'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
$perfDir = Join-Path $repoRoot 'perf-data'

if ([string]::IsNullOrWhiteSpace($Preset)) {
    $Preset = if ($Mode -eq 'app') { 'win-optimized' } else { 'win-benchmark' }
}

if (-not $Timestamp) {
    $Timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
}

$prefix = if ($Mode -eq 'app') { 'etw-app' } else { 'etw-bench' }
$tracePath = Join-Path $perfDir "$prefix-$Timestamp.etl"
$childLogPath = Join-Path $perfDir "$prefix-child-$Timestamp.log"
$launcherLogPath = Join-Path $perfDir "$prefix-launch-$Timestamp.log"
$benchJsonPath = Join-Path $perfDir "$prefix-$Timestamp.json"

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

if (-not (Test-IsAdministrator)) {
    New-Item -ItemType Directory -Path $perfDir -Force | Out-Null

    if (-not $SkipBuild) {
        Invoke-Native cmake '--preset' $Preset
        Invoke-Native cmake '--build' '--preset' $Preset
    }

    # Validate WPR availability before prompting for elevation so failures are immediate.
    $null = Ensure-Tool 'wpr'

    # Relaunch using the current PowerShell host (pwsh or powershell) for consistent behavior.
    $hostExe = (Get-Process -Id $PID).Path
    if ([string]::IsNullOrWhiteSpace($hostExe)) {
        throw 'Unable to resolve current PowerShell host executable path for elevation.'
    }

    # Child run always uses -SkipBuild: if needed, parent already built before elevation.
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Mode', $Mode, '-Preset', $Preset, '-Timestamp', $Timestamp, '-BenchmarkFilter', $BenchmarkFilter, '-BenchmarkRepetitions', "$BenchmarkRepetitions", '-BenchmarkMinTime', $BenchmarkMinTime, '-SkipBuild')

    $launchCommand = "$hostExe $($argList -join ' ')"
    "LAUNCH=$launchCommand" | Set-Content -Path $launcherLogPath -Encoding utf8

    $childProcess = Start-Process -FilePath $hostExe -Verb RunAs -ArgumentList $argList -WorkingDirectory $repoRoot -Wait -PassThru
    "EXIT_CODE=$($childProcess.ExitCode)" | Add-Content -Path $launcherLogPath -Encoding utf8

    if ($childProcess.ExitCode -ne 0) {
        throw "Elevated ETW capture child process failed with exit code $($childProcess.ExitCode). Check $launcherLogPath and $childLogPath when present."
    }

    $missingArtifacts = @()
    if (-not (Test-Path -LiteralPath $childLogPath)) {
        $missingArtifacts += $childLogPath
    }
    if (-not (Test-Path -LiteralPath $tracePath)) {
        $missingArtifacts += $tracePath
    }
    if (($Mode -eq 'bench') -and (-not (Test-Path -LiteralPath $benchJsonPath))) {
        $missingArtifacts += $benchJsonPath
    }

    if ($missingArtifacts.Count -gt 0) {
        $artifactList = $missingArtifacts -join ', '
        throw "Elevated ETW capture did not produce expected artifact(s): $artifactList. This usually means the UAC prompt was denied or the elevated child failed before trace shutdown. Check $launcherLogPath and $childLogPath when present."
    }

    Write-Host "TRACE=$tracePath"
    Write-Host "CHILD_LOG=$childLogPath"
    Write-Host "LAUNCHER_LOG=$launcherLogPath"
    Write-Host "PRESET=$Preset"
    if ($Mode -eq 'bench') {
        Write-Host "BENCH=$benchJsonPath"
    }
    return
}

New-Item -ItemType Directory -Path $perfDir -Force | Out-Null

$null = Ensure-Tool 'wpr'

if (-not $SkipBuild) {
    Invoke-Native cmake '--preset' $Preset
    Invoke-Native cmake '--build' '--preset' $Preset
}

$binaryName = if ($Mode -eq 'app') { 'TaskSmack.exe' } else { 'TaskSmackBenchmarks.exe' }
$binaryPath = Join-Path $repoRoot "build/$Preset/bin/$binaryName"
Ensure-Binary $binaryPath

$logPath = $childLogPath

Start-Transcript -Path $logPath -Force | Out-Null
try {
    Write-Host "Starting ETW capture ($Mode)"
    Write-Host "Trace: $tracePath"
    Write-Host "Preset: $Preset"

    cmd /c "wpr -cancel >nul 2>&1" | Out-Null

    Invoke-Native wpr '-start' 'CPU' '-filemode'

    if ($Mode -eq 'app') {
        $proc = Start-Process -FilePath $binaryPath -PassThru
        Write-Host "Launched app PID: $($proc.Id)"
        Write-Host 'Exercise the application, then close it to finish the trace.'
        Wait-Process -Id $proc.Id
    }
    else {
        Invoke-Native $binaryPath "--benchmark_filter=$BenchmarkFilter" "--benchmark_repetitions=$BenchmarkRepetitions" "--benchmark_min_time=$BenchmarkMinTime" '--benchmark_report_aggregates_only=true' '--benchmark_display_aggregates_only=true' "--benchmark_out=$benchJsonPath" '--benchmark_out_format=json'
        Write-Host "Benchmark JSON: $benchJsonPath"
    }

    Invoke-Native wpr '-stop' $tracePath
    Write-Host "ETW_TRACE=$tracePath"
    if ($Mode -eq 'bench') {
        Write-Host "ETW_BENCH=$benchJsonPath"
    }
}
finally {
    Stop-Transcript | Out-Null
}

Write-Host "TRACE=$tracePath"
Write-Host "CHILD_LOG=$logPath"
Write-Host "PRESET=$Preset"
if ($Mode -eq 'bench') {
    Write-Host "BENCH=$benchJsonPath"
}
