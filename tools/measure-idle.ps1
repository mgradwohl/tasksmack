#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    Measure TaskSmack's idle CPU (per thread), frame rate and memory on Windows.
.DESCRIPTION
    The Windows twin of tools/measure-idle.sh (#2492). It measures an existing build; it does not
    configure or build. One run:

    1. Launches TaskSmack.exe with TASKSMACK_TRACE_RESIZE_PERF=1, TASKSMACK_LOG_LEVEL=info, its own
       TASKSMACK_CONFIG_DIR (a fresh, empty directory unless -ConfigDir names one, so the run starts
       from default settings and never reads or rewrites the user's config.toml) and TEMP/TMP set
       to a folder inside that directory, so the app's tasksmack-debug.log is written there instead
       of over the user's own %TEMP%\tasksmack-debug.log.
    2. Waits for the main window (and for "Entering main loop" in that log), then -Warmup seconds.
    3. Samples each phase (-Phases; by default one visible phase of -Duration seconds): the process's
       and every thread's CPU time (100% = one logical CPU fully busy) and context switches
       (wake-ups per second), the private working set (Task Manager's "Memory" column), private
       bytes and working set at the start and end plus the peak private working set seen every
       -MemoryPollSeconds, the system timer resolution at each poll, dwm.exe's CPU and wake-ups, and
       how busy the rest of the machine was (#3715). A minimized phase posts SC_MINIMIZE to the
       app's own window and a restored phase SC_RESTORE (WM_SYSCOMMAND, not injected input), then
       waits -PhaseSettle seconds before sampling.
    4. Closes the app with WM_CLOSE (Process.CloseMainWindow, not injected input), or stops it if it
       has not exited 15 s later, and fails the run if it exited non-zero or had to be stopped.
    5. Prints a table, one RESULT line (and one PHASE line a phase), and writes JSON; with
       -MemoryCsv, also the memory series.

    fps and frame times come from the ResizePerf summaries the app logs every 5 s at idle, as in
    measure-idle.sh: fps is the summaries' loop intervals divided by their summed duration;
    frame p95/p99/max is update+render+post+swap per presented frame (the worst summary in the
    window); loop p95/p99 is the deliver-to-deliver interval. A summary of a window that drew no
    frame (frames=0 loopIntervals=0, as when the window is minimized) is left out of those figures
    (#3210), and they read n/a (null in the JSON) when no summary drew one. A build that does not
    log idle summaries (v0.8.0 and older, or a release build of those, which logs nothing at info
    level) gets no frame figures; the CPU and memory figures do not depend on the log.

    The UI thread is the one described as "tasksmack-ui" (Platform/ThreadName.h), or, for a build
    that does not name it, the process's first thread. The other named threads (ts-sampler-*) are
    shown by name; unnamed threads (GPU driver, SDL, the thread pool) by the module their Win32
    start address is in (igxelpgicd64.dll+0x8da460), with the thread id.

    Wake-ups, the memory figures, dwm.exe and the machine load come from one SystemProcessInformation
    snapshot at each end of a phase (tools/idle-sample.ps1); the timer resolution from
    NtQueryTimerResolution. None needs elevation. A figure that can't be read is null in the JSON
    and n/a in the table: a thread's module when the thread can't be opened (an elevated TaskSmack
    seen from a normal shell), and every snapshot figure in a 32-bit PowerShell, which then falls
    back to the per-thread CPU time alone.

    The JSON is schema tasksmack-idle-windows/2: version 1's fields, which describe the first
    phase, and a phases array with each phase's figures. A thread row adds module and wakeupsPerSec.

    Background load on the machine adds noise to Windows numbers: close what you can, and compare
    runs taken back to back.
.PARAMETER Preset
    The CMake preset whose build/<Preset>/bin/TaskSmack.exe is measured. Defaults to win-profile.
.PARAMETER Binary
    Measure this TaskSmack.exe instead (e.g. one unpacked from a release zip).
.PARAMETER Warmup
    Seconds to wait after the main loop starts, before sampling. Default 15.
.PARAMETER Duration
    Seconds to sample, as one visible phase. Default 30. Ignored when -Phases is given.
.PARAMETER Phases
    The phases to sample, in order: <kind>:<seconds>, comma-separated or as an array, with kind one
    of visible, minimized or restored (e.g. visible:120,minimized:60,restored:60). Default
    visible:<Duration>.
.PARAMETER PhaseSettle
    Seconds to wait after minimizing or restoring the window, before that phase is sampled. Default 10.
.PARAMETER MemoryPollSeconds
    How often the memory and the timer resolution are read during a phase. Default 5.
.PARAMETER MemoryCsv
    Also write the memory series here: elapsed seconds, phase, private working set, private bytes,
    working set (MB), handles and threads, one row a poll. For long memory runs.
.PARAMETER Label
    Scenario name printed with the results and used in the file names. Default idle.
.PARAMETER Window
    Passed to the app as TASKSMACK_WINDOW (e.g. 1280x720 or 1600x900,maximized): a fixed window
    geometry that is not saved on exit. Builds older than #1453 ignore it; give those a
    config.toml with [window] width/height in -ConfigDir instead.
.PARAMETER ConfigDir
    Use this directory as TASKSMACK_CONFIG_DIR instead of a fresh one (it is created if missing and
    kept afterwards). Put a config.toml in it to measure non-default settings.
.PARAMETER RedirectAppData
    Also point APPDATA, LOCALAPPDATA and USERPROFILE at -ConfigDir (or the fresh directory), for
    builds that predate TASKSMACK_CONFIG_DIR (v0.8.0 and older) and keep their config in
    %APPDATA%\TaskSmack: the app then reads and writes <dir>\AppData\Roaming\TaskSmack\config.toml
    instead of the user's. The script checks that the user's own config.toml was not modified.
.PARAMETER Json
    Write the JSON here. Default perf-data/idle-<label>-<timestamp>.json.
.EXAMPLE
    pwsh tools/measure-idle.ps1 -Preset win-release -Window 1280x720 -Duration 300
.EXAMPLE
    pwsh tools/measure-idle.ps1 -Window 1280x720 -Warmup 30 -Phases visible:120,minimized:60,restored:60
.EXAMPLE
    pwsh tools/measure-idle.ps1 -Window 1280x720 -Phases visible:3600 -MemoryPollSeconds 30 -MemoryCsv perf-data\memory.csv
.EXAMPLE
    pwsh tools/measure-idle.ps1 -Binary C:\temp\TaskSmack-0.8.0-win64\TaskSmack.exe -RedirectAppData -ConfigDir C:\temp\ts080 -Duration 300
#>

[CmdletBinding()]
param(
    [string]$Preset = 'win-profile',
    [string]$Binary,
    [ValidateRange(0, 86400)][int]$Warmup = 15,
    [ValidateRange(1, 86400)][int]$Duration = 30,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'idle',
    [string]$Window,
    [string]$ConfigDir,
    [switch]$RedirectAppData,
    [string]$Json,
    [string[]]$Phases,
    [ValidateRange(0, 600)][int]$PhaseSettle = 10,
    [ValidateRange(1, 3600)][int]$MemoryPollSeconds = 5,
    [string]$MemoryCsv
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try { [Console]::OutputEncoding = $OutputEncoding } catch { Write-Verbose "Console encoding unchanged: $_" }

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

. (Join-Path $scriptDir 'resize-perf-summary.ps1') # Get-FrameStatistic, Format-Millisecond
. (Join-Path $scriptDir 'idle-sample.ps1')         # TaskSmackIdleSampler and the phase arithmetic (#3715)

$MainLoopMarker = 'Entering main loop'                  # Core::Application::run(), info level
$AlreadyRunningExitCode = 75                            # App::ALREADY_RUNNING_EXIT_CODE (InstanceLock.h)
$MainWindowTimeoutSeconds = 60
$CloseTimeoutSeconds = 15
$WmSysCommand = 0x0112
$ScMinimize = 0xF020
$ScRestore = 0xF120

if (-not ('TaskSmackIdleNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class TaskSmackIdleNative {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")]
    static extern int GetThreadDescription(IntPtr thread, out IntPtr description);
    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr memory);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool IsZoomed(IntPtr window);
    [DllImport("user32.dll")]
    static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);

    // The client size in physical pixels: this thread is made per-monitor DPI aware first, since a
    // DPI-unaware caller gets sizes scaled down by the display scale.
    public static bool PhysicalClientRect(IntPtr window, out RECT rect) {
        IntPtr previous = SetThreadDpiAwarenessContext(new IntPtr(-4)); // PER_MONITOR_AWARE_V2
        try { return GetClientRect(window, out rect); }
        finally { if (previous != IntPtr.Zero) { SetThreadDpiAwarenessContext(previous); } }
    }

    // The thread's description (SetThreadDescription), or "" when it has none or is gone.
    public static string ThreadName(int threadId) {
        const uint THREAD_QUERY_LIMITED_INFORMATION = 0x0800;
        IntPtr thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, false, (uint)threadId);
        if (thread == IntPtr.Zero) { return ""; }
        try {
            IntPtr text;
            if (GetThreadDescription(thread, out text) < 0 || text == IntPtr.Zero) { return ""; }
            try { return Marshal.PtrToStringUni(text) ?? ""; } finally { LocalFree(text); }
        } finally { CloseHandle(thread); }
    }
}
'@
}

function Read-LogFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    # FileShare.ReadWrite: the app may still hold the log open for writing.
    $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try { return @([IO.StreamReader]::new($stream).ReadToEnd() -split "`r?`n") }
    finally { $stream.Dispose() }
}

function Select-LogWindow([string[]]$Lines, [datetime]$From, [datetime]$To) {
    # The lines whose spdlog timestamp ("[yyyy-MM-dd HH:mm:ss.fff]", local time) is in [From, To].
    # Chosen by time, not by line count, because the file sink is not flushed on every line.
    $inv = [Globalization.CultureInfo]::InvariantCulture
    foreach ($line in $Lines) {
        $m = [regex]::Match($line, '^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')
        if (-not $m.Success) { continue }
        $when = [datetime]::ParseExact($m.Groups[1].Value, 'yyyy-MM-dd HH:mm:ss.fff', $inv)
        if ($when -ge $From -and $when -le $To) { $line }
    }
}

# --- Inputs --------------------------------------------------------------------------------------
$phaseList = if ($Phases) {
    if ($PSBoundParameters.ContainsKey('Duration')) { Write-Warning '-Duration is ignored: -Phases gives each phase its length.' }
    ConvertFrom-IdlePhaseSpec $Phases
}
else { ConvertFrom-IdlePhaseSpec @("visible:$Duration") }
if ($Binary) {
    $binaryPath = (Resolve-Path -LiteralPath $Binary).Path
}
else {
    $binaryPath = Join-Path $repoRoot "build/$Preset/bin/TaskSmack.exe"
    if (-not (Test-Path -LiteralPath $binaryPath)) {
        throw "No TaskSmack.exe at $binaryPath. Build it first (cmake --build --preset $Preset), or pass -Binary."
    }
}

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$keepConfigDir = [bool]$ConfigDir
if (-not $ConfigDir) {
    $ConfigDir = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-idle-config-$([guid]::NewGuid().ToString('N'))"
}
New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
$ConfigDir = (Resolve-Path -LiteralPath $ConfigDir).Path
$appTemp = Join-Path $ConfigDir 'temp'
New-Item -ItemType Directory -Force -Path $appTemp | Out-Null
$appLog = Join-Path $appTemp 'tasksmack-debug.log'
if (-not $Json) {
    $perfDir = Join-Path $repoRoot 'perf-data'
    New-Item -ItemType Directory -Force -Path $perfDir | Out-Null
    $Json = Join-Path $perfDir "idle-$Label-$timestamp.json"
}

# The user's own config: with -RedirectAppData the app must not touch it, and this checks that.
$userConfig = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'TaskSmack\config.toml'
$userConfigStamp = if (Test-Path -LiteralPath $userConfig) { (Get-Item -LiteralPath $userConfig).LastWriteTimeUtc } else { $null }

$others = @(Get-Process -Name TaskSmack -ErrorAction SilentlyContinue)
if ($others.Count -gt 0) {
    Write-Warning "$($others.Count) other TaskSmack process(es) running (PIDs $($others.Id -join ', ')). They are left alone; their load adds noise."
}

Write-Host "=== ${Label}: warmup=${Warmup}s phases=$(($phaseList | ForEach-Object { "$($_.kind):$($_.seconds)" }) -join ',') window=$(if ($Window) { $Window } else { 'saved/default' })"
Write-Host "Binary:     $binaryPath"
Write-Host "Config dir: $ConfigDir"
Write-Host "App log:    $appLog"

# --- Launch --------------------------------------------------------------------------------------
$psi = [Diagnostics.ProcessStartInfo]::new($binaryPath)
$psi.UseShellExecute = $false
$psi.WorkingDirectory = Split-Path -Parent $binaryPath
# The app's console sink would otherwise print its whole log here; the log file has it all.
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.Environment['TASKSMACK_CONFIG_DIR'] = $ConfigDir
$psi.Environment['TASKSMACK_TRACE_RESIZE_PERF'] = '1'
$psi.Environment['TASKSMACK_LOG_LEVEL'] = 'info'
$psi.Environment['TEMP'] = $appTemp
$psi.Environment['TMP'] = $appTemp
if ($Window) { $psi.Environment['TASKSMACK_WINDOW'] = $Window }
else { [void]$psi.Environment.Remove('TASKSMACK_WINDOW') }
[void]$psi.Environment.Remove('TASKSMACK_SYNTHETIC')
if ($RedirectAppData) {
    $roaming = Join-Path $ConfigDir 'AppData\Roaming'
    $local = Join-Path $ConfigDir 'AppData\Local'
    New-Item -ItemType Directory -Force -Path $roaming, $local | Out-Null
    $psi.Environment['APPDATA'] = $roaming
    $psi.Environment['LOCALAPPDATA'] = $local
    $psi.Environment['USERPROFILE'] = $ConfigDir
}

$timerBeforeLaunch = [TaskSmackIdleSampler]::TimerResolutionMs()
$proc = [Diagnostics.Process]::Start($psi)
$null = $proc.Handle # keeps ExitCode readable after exit
# Drained and discarded, so a full pipe never blocks the app.
$proc.BeginOutputReadLine()
$proc.BeginErrorReadLine()
Write-Host "Launched TaskSmack (PID $($proc.Id))."
$failure = $null
$result = $null
try {
    $deadline = (Get-Date).AddSeconds($MainWindowTimeoutSeconds)
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 250
    }
    # A build that logs at info level says when its main loop starts; give it a few seconds more.
    $loopDeadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $loopDeadline -and -not $proc.HasExited) {
        if ((Read-LogFile $appLog) -match [regex]::Escape($MainLoopMarker)) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($proc.HasExited) {
        if ($proc.ExitCode -eq $AlreadyRunningExitCode) {
            throw "TaskSmack exited with code ${AlreadyRunningExitCode}: another TaskSmack holds the lock for $ConfigDir. Close it or use another -ConfigDir."
        }
        throw "TaskSmack exited during startup with code $($proc.ExitCode). See $appLog."
    }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { Write-Warning "No main window within $MainWindowTimeoutSeconds s; continuing." }

    if ($Warmup -gt 0) {
        Write-Host "Warming up for $Warmup s..."
        if ($proc.WaitForExit($Warmup * 1000)) { throw "TaskSmack exited during the warm-up with code $($proc.ExitCode)." }
    }

    $hwnd = $proc.MainWindowHandle
    $rect = [TaskSmackIdleNative+RECT]::new()
    $windowInfo = $null
    if ($hwnd -ne [IntPtr]::Zero -and [TaskSmackIdleNative]::PhysicalClientRect($hwnd, [ref]$rect)) {
        $windowInfo = [ordered]@{
            clientWidth  = $rect.Right - $rect.Left
            clientHeight = $rect.Bottom - $rect.Top
            dpi          = [TaskSmackIdleNative]::GetDpiForWindow($hwnd)
            maximized    = [TaskSmackIdleNative]::IsZoomed($hwnd)
        }
    }

    # The module each thread starts in, for the unnamed ones (#3715); read once, after the warm-up.
    $modules = @()
    try {
        $proc.Refresh()
        $modules = @($proc.Modules | ForEach-Object { [pscustomobject]@{ name = $_.ModuleName; base = $_.BaseAddress.ToInt64(); size = [long]$_.ModuleMemorySize } })
    }
    catch { Write-Verbose "Modules unavailable: $_" }
    $startThreads = Get-ThreadTime $proc
    $firstThreadId = ($startThreads.GetEnumerator() | Sort-Object { $_.Value.Start } | Select-Object -First 1).Key
    $threadNames = @{}
    $nameOf = {
        param([long]$tid)
        if (-not $threadNames.ContainsKey($tid)) {
            $n = [TaskSmackIdleNative]::ThreadName([int]$tid)
            $threadNames[$tid] = if ($n) { $n } elseif ($tid -eq $firstThreadId) { '(first thread)' } else { '' }
        }
        $threadNames[$tid]
    }
    $moduleOf = { param([long]$tid) Resolve-ThreadModule ([TaskSmackIdleSampler]::Win32StartAddress($tid)) $modules }

    # --- Sample ----------------------------------------------------------------------------------
    $phaseResults = [Collections.Generic.List[object]]::new()
    $memoryRows = [Collections.Generic.List[object]]::new()
    $runClock = [Diagnostics.Stopwatch]::StartNew()
    for ($index = 0; $index -lt $phaseList.Count; $index++) {
        $phase = $phaseList[$index]
        if ($phase.kind -ne 'visible' -and $hwnd -eq [IntPtr]::Zero) { throw "No main window to post $($phase.kind) to." }
        if ($phase.kind -eq 'minimized') { [void][TaskSmackIdleSampler]::PostMessageW($hwnd, $WmSysCommand, [IntPtr]$ScMinimize, [IntPtr]::Zero) }
        elseif ($phase.kind -eq 'restored') { [void][TaskSmackIdleSampler]::PostMessageW($hwnd, $WmSysCommand, [IntPtr]$ScRestore, [IntPtr]::Zero) }
        if ($index -gt 0 -and $PhaseSettle -gt 0) {
            if ($proc.WaitForExit($PhaseSettle * 1000)) { throw "TaskSmack exited while settling into $($phase.kind) with code $($proc.ExitCode)." }
        }
        $phaseResults.Add((Measure-IdlePhase -Process $proc -Kind $phase.kind -Seconds $phase.seconds -Window $hwnd -NameOf $nameOf -ModuleOf $moduleOf `
                    -FirstThreadId $firstThreadId -MemoryPollSeconds $MemoryPollSeconds -RunClock $runClock -MemoryRows $memoryRows))
    }
    $first = $phaseResults[0]

    $result = [ordered]@{
        schema          = 'tasksmack-idle-windows/2'
        label           = $Label
        binary          = $binaryPath
        binaryVersion   = (Get-Item -LiteralPath $binaryPath).VersionInfo.ProductVersion
        anchor          = $null
        warmupSeconds   = $Warmup
        # Version 1's fields: the first phase's figures
        durationSeconds = $first.durationSeconds
        cpuStartUtc     = $first.cpuStartUtc
        cpuEndUtc       = $first.cpuEndUtc
        requestedWindow = $Window
        window          = $windowInfo
        totalCpu        = $first.totalCpu
        uiThreadCpu     = $first.uiThreadCpu
        frames          = $null
        memoryStart     = $first.memoryStart
        memoryEnd       = $first.memoryEnd
        peakPrivateWorkingSetMB = $first.peakPrivateWorkingSetMB
        threads         = $first.threads
        # Version 2 (#3715)
        totalWakeupsPerSec    = $first.totalWakeupsPerSec
        uiThreadWakeupsPerSec = $first.uiThreadWakeupsPerSec
        timerResolutionMs     = $first.timerResolutionMs
        timerResolutionBeforeLaunchMs = if ($timerBeforeLaunch -gt 0) { $timerBeforeLaunch } else { $null }
        timerResolutionAfterExitMs    = $null
        phases          = $phaseResults
        memoryCsv       = $null
        machine         = [ordered]@{
            cpu          = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
            logicalCpus  = [Environment]::ProcessorCount
            gpus         = @(Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) ($($_.DriverVersion))" })
            windows      = (Get-CimInstance Win32_OperatingSystem).Version
        }
    }
}
catch {
    $failure = $_
}
finally {
    if (-not $proc.HasExited) {
        Write-Host 'Closing TaskSmack...'
        $null = $proc.CloseMainWindow()
        if (-not $proc.WaitForExit($CloseTimeoutSeconds * 1000)) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            $null = $proc.WaitForExit(30000)
            if (-not $failure) { $failure = "TaskSmack did not exit within $CloseTimeoutSeconds s of WM_CLOSE and was stopped." }
        }
        elseif (-not $failure -and $proc.ExitCode -ne 0) {
            $failure = "TaskSmack exited with code $($proc.ExitCode) after WM_CLOSE. See $appLog."
        }
    }
    if ($null -ne $userConfigStamp -and (Get-Item -LiteralPath $userConfig).LastWriteTimeUtc -ne $userConfigStamp) {
        Write-Warning "$userConfig changed during the run. Without TASKSMACK_CONFIG_DIR support the app may have used it; pass -RedirectAppData for such builds."
    }
    # Read once the app has exited, so its file sink has flushed every line.
    $logLines = Read-LogFile $appLog
    if (-not $keepConfigDir -and $proc.HasExited) {
        # The log is kept beside the JSON before the scratch config goes.
        if (Test-Path -LiteralPath $appLog) { Copy-Item -LiteralPath $appLog -Destination ([IO.Path]::ChangeExtension($Json, $null).TrimEnd('.') + '-app.log') }
        Remove-Item -LiteralPath $ConfigDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
if ($failure) { throw $failure }

# The ResizePerf summaries logged while each phase was sampled (each covers the 5 s before it).
foreach ($p in $result.phases) {
    $p.frames = Get-FrameStatistic @(Select-LogWindow -Lines $logLines -From $p.cpuStartLocal -To $p.cpuEndLocal)
    $p.Remove('cpuStartLocal')
    $p.Remove('cpuEndLocal')
}
$frames = $result.phases[0].frames
$result.frames = $frames
$result.anchor = $logLines | Where-Object { $_ -match 'ResizePerfAnchor:' } | Select-Object -First 1
$timerAfterExit = [TaskSmackIdleSampler]::TimerResolutionMs()
$result.timerResolutionAfterExitMs = if ($timerAfterExit -gt 0) { $timerAfterExit } else { $null }
if ($MemoryCsv) {
    $memoryRows | Export-Csv -LiteralPath $MemoryCsv -NoTypeInformation -Encoding utf8
    $result.memoryCsv = (Resolve-Path -LiteralPath $MemoryCsv).Path
}

$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Json -Encoding utf8

# --- Report --------------------------------------------------------------------------------------
$fmt = { param($v, $unit = '') if ($null -eq $v -or ($v -is [array] -and $v.Count -eq 0)) { 'n/a' } elseif ($v -is [array]) { ($v -join ',') + $unit } else { "$v$unit" } }
$memStart = $result.memoryStart
$memEnd = $result.memoryEnd
$peakPrivateWs = $result.peakPrivateWorkingSetMB
$threads = $result.threads
Write-Host ''
Write-Host ("{0,-34} {1,12}" -f 'Metric', 'Value')
Write-Host ("{0,-34} {1,12}" -f 'Total CPU (% of one CPU)', $result.totalCpu)
Write-Host ("{0,-34} {1,12}" -f 'UI thread CPU', (& $fmt $result.uiThreadCpu))
Write-Host ("{0,-34} {1,12}" -f 'fps', (& $fmt $frames.fps))
Write-Host ("{0,-34} {1,12}" -f 'frame p95/p99/max (ms)', (Format-Millisecond @($frames.frameP95Ms, $frames.frameP99Ms, $frames.frameMaxMs)))
Write-Host ("{0,-34} {1,12}" -f 'loop p95/p99 (ms)', (Format-Millisecond @($frames.loopP95Ms, $frames.loopP99Ms)))
Write-Host ("{0,-34} {1,12}" -f 'ResizePerf summaries', $frames.summaries)
Write-Host ("{0,-34} {1,12}" -f 'Private working set start/end MB', "$(& $fmt $memStart.privateWorkingSetMB)/$(& $fmt $memEnd.privateWorkingSetMB)")
Write-Host ("{0,-34} {1,12}" -f 'Peak private working set MB', (& $fmt $peakPrivateWs))
Write-Host ("{0,-34} {1,12}" -f 'Private bytes start/end MB', "$($memStart.privateBytesMB)/$($memEnd.privateBytesMB)")
Write-Host ("{0,-34} {1,12}" -f 'Window client (px) / DPI', $(if ($result.window) { "$($result.window.clientWidth)x$($result.window.clientHeight) @$($result.window.dpi)" } else { 'n/a' }))
Write-Host ("{0,-34} {1,12}" -f 'Wake-ups/s total / UI thread', "$(& $fmt $result.totalWakeupsPerSec)/$(& $fmt $result.uiThreadWakeupsPerSec)")
Write-Host ("{0,-34} {1,12}" -f 'System timer resolution (ms)', (& $fmt $result.timerResolutionMs))
Write-Host ("{0,-34} {1,12}" -f 'dwm CPU / wake-ups/s', $(if ($result.phases[0].dwm) { "$($result.phases[0].dwm.cpu)/$($result.phases[0].dwm.wakeupsPerSec)" } else { 'n/a' }))
Write-Host ("{0,-34} {1,12}" -f 'Rest of machine (% of all CPUs)', $(if ($result.phases[0].machineLoad) { $result.phases[0].machineLoad.restPctOfAll } else { 'n/a' }))
if ($result.phases.Count -gt 1) {
    Write-Host ''
    Write-Host ("{0,-10} {1,7} {2,7} {3,8} {4,8} {5,7} {6,10} {7,10} {8,8}" -f 'Phase', 'CPU', 'UI CPU', 'wake/s', 'UI wk/s', 'fps', 'loop p95', 'timer ms', 'rest %')
    foreach ($p in $result.phases) {
        Write-Host ("{0,-10} {1,7} {2,7} {3,8} {4,8} {5,7} {6,10} {7,10} {8,8}" -f $p.phase, $p.totalCpu, (& $fmt $p.uiThreadCpu), (& $fmt $p.totalWakeupsPerSec),
            (& $fmt $p.uiThreadWakeupsPerSec), (& $fmt $p.frames.fps), (& $fmt $p.frames.loopP95Ms), (& $fmt $p.timerResolutionMs),
            $(if ($p.machineLoad) { $p.machineLoad.restPctOfAll } else { 'n/a' }))
    }
}
Write-Host ''
Write-Host 'Threads above 0.05% (first phase):'
foreach ($t in $threads | Where-Object cpu -GT 0.05) {
    Write-Host ("  {0,7} {1,-36} {2,8} {3,10}" -f $t.tid, $(if ($t.name) { $t.name } elseif ($t.module) { $t.module } else { '' }), $t.cpu, (& $fmt $t.wakeupsPerSec ' /s'))
}
Write-Host ''
Write-Host "JSON: $Json"
if ($result.memoryCsv) { Write-Host "Memory series: $($result.memoryCsv)" }
$inv = [Globalization.CultureInfo]::InvariantCulture
# Version 1's keys first, unchanged; version 2's after them (#3715).
Write-Host ([string]::Format($inv, 'RESULT label={0} totalCpu={1} uiCpu={2} fps={3} privateWsStartMB={4} privateWsEndMB={5} duration={6} wakeups={7} uiWakeups={8} timerMs={9}',
        $Label, $result.totalCpu, (& $fmt $result.uiThreadCpu), (& $fmt $frames.fps),
        (& $fmt $memStart.privateWorkingSetMB), (& $fmt $memEnd.privateWorkingSetMB), $result.durationSeconds,
        (& $fmt $result.totalWakeupsPerSec), (& $fmt $result.uiThreadWakeupsPerSec), (& $fmt $result.timerResolutionMs)))
foreach ($p in $result.phases) {
    Write-Host ([string]::Format($inv, 'PHASE label={0} phase={1} totalCpu={2} uiCpu={3} fps={4} loopP95Ms={5} wakeups={6} uiWakeups={7} timerMs={8} duration={9}',
            $Label, $p.phase, $p.totalCpu, (& $fmt $p.uiThreadCpu), (& $fmt $p.frames.fps), (& $fmt $p.frames.loopP95Ms),
            (& $fmt $p.totalWakeupsPerSec), (& $fmt $p.uiThreadWakeupsPerSec), (& $fmt $p.timerResolutionMs), $p.durationSeconds))
}
