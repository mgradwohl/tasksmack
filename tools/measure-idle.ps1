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
    3. Samples for -Duration seconds: the process's and every thread's CPU time (100% = one logical
       CPU fully busy), and the private working set (Task Manager's "Memory" column), private bytes
       and working set at the start and end, plus the peak private working set seen every 5 s.
    4. Closes the app with WM_CLOSE (Process.CloseMainWindow, not injected input), or stops it if it
       has not exited 15 s later, and fails the run if it exited non-zero or had to be stopped.
    5. Prints a table and one RESULT line, and writes JSON.

    fps and frame times come from the ResizePerf summaries the app logs every 5 s at idle, as in
    measure-idle.sh: fps is the summaries' loop intervals divided by their summed duration;
    frame p95/p99/max is update+render+post+swap per presented frame (the worst summary in the
    window); loop p95/p99 is the deliver-to-deliver interval. A build that does not log idle
    summaries (v0.8.0 and older, or a release build of those, which logs nothing at info level)
    gets no frame figures; the CPU and memory figures do not depend on the log.

    The UI thread is the one described as "tasksmack-ui" (Platform/ThreadName.h), or, for a build
    that does not name it, the process's first thread. The other named threads (ts-sampler-*) are
    shown by name; unnamed threads (GPU driver, SDL, the thread pool) by thread id.

    Background load on the machine adds noise to Windows numbers: close what you can, and compare
    runs taken back to back.
.PARAMETER Preset
    The CMake preset whose build/<Preset>/bin/TaskSmack.exe is measured. Defaults to win-profile.
.PARAMETER Binary
    Measure this TaskSmack.exe instead (e.g. one unpacked from a release zip).
.PARAMETER Warmup
    Seconds to wait after the main loop starts, before sampling. Default 15.
.PARAMETER Duration
    Seconds to sample. Default 30.
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
    [string]$Json
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try { [Console]::OutputEncoding = $OutputEncoding } catch { Write-Verbose "Console encoding unchanged: $_" }

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

$MainLoopMarker = 'Entering main loop'                  # Core::Application::run(), info level
$AlreadyRunningExitCode = 75                            # App::ALREADY_RUNNING_EXIT_CODE (InstanceLock.h)
$MainWindowTimeoutSeconds = 60
$CloseTimeoutSeconds = 15
$MemoryPollSeconds = 5

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

function Get-ThreadTime([System.Diagnostics.Process]$Process) {
    # tid -> @{ Ticks; Start } for every thread still readable; one that exits mid-read is skipped.
    $Process.Refresh()
    $times = @{}
    foreach ($thread in @($Process.Threads)) {
        try { $times[$thread.Id] = @{ Ticks = $thread.TotalProcessorTime.Ticks; Start = $thread.StartTime } }
        catch { Write-Verbose "Thread $($thread.Id) not readable: $_" }
    }
    return $times
}

function Get-Memory([System.Diagnostics.Process]$Process) {
    $Process.Refresh()
    $privateWs = $null
    # "Process V2" instances are <name>:<pid>, so the figure is this process's even when several
    # TaskSmack.exe run. Get-Counter is locale-dependent; on a non-English system it fails and the
    # private working set is left out (private bytes and working set are still recorded).
    try {
        $path = "\Process V2($($Process.ProcessName):$($Process.Id))\Working Set - Private"
        $privateWs = [int64](Get-Counter -Counter $path -ErrorAction Stop).CounterSamples[0].CookedValue
    }
    catch { Write-Verbose "Private working set unavailable: $_" }
    return [ordered]@{
        privateWorkingSetMB = if ($null -ne $privateWs) { [math]::Round($privateWs / 1MB, 1) } else { $null }
        privateBytesMB      = [math]::Round($Process.PrivateMemorySize64 / 1MB, 1)
        workingSetMB        = [math]::Round($Process.WorkingSet64 / 1MB, 1)
    }
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

function Get-FrameStatistic([string[]]$Lines) {
    # The ResizePerf summaries among $Lines -> fps, frame p95/p99/max, loop p95/p99, as
    # measure-idle.sh computes them.
    $stats = [ordered]@{ summaries = 0; fps = $null; frameP95Ms = 0.0; frameP99Ms = 0.0; frameMaxMs = 0.0; loopP95Ms = 0.0; loopP99Ms = 0.0 }
    $intervals = 0.0
    $loopMs = 0.0
    $inv = [Globalization.CultureInfo]::InvariantCulture
    foreach ($line in $Lines) {
        if ($line -notmatch 'ResizePerf\[') { continue }
        $frame = [regex]::Match($line, '\bframe avg/p95/p99/max=([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)')
        $loop = [regex]::Match($line, '\bloopIntervals=(\d+) loop avg/p95/p99/max=([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)')
        if (-not $loop.Success) { continue }
        $stats.summaries++
        $count = [double]::Parse($loop.Groups[1].Value, $inv)
        $intervals += $count
        $loopMs += $count * [double]::Parse($loop.Groups[2].Value, $inv)
        $stats.loopP95Ms = [math]::Max($stats.loopP95Ms, [double]::Parse($loop.Groups[3].Value, $inv))
        $stats.loopP99Ms = [math]::Max($stats.loopP99Ms, [double]::Parse($loop.Groups[4].Value, $inv))
        if ($frame.Success) {
            $stats.frameP95Ms = [math]::Max($stats.frameP95Ms, [double]::Parse($frame.Groups[2].Value, $inv))
            $stats.frameP99Ms = [math]::Max($stats.frameP99Ms, [double]::Parse($frame.Groups[3].Value, $inv))
            $stats.frameMaxMs = [math]::Max($stats.frameMaxMs, [double]::Parse($frame.Groups[4].Value, $inv))
        }
    }
    if ($loopMs -gt 0) { $stats.fps = [math]::Round($intervals * 1000.0 / $loopMs, 2) }
    return $stats
}

# --- Inputs --------------------------------------------------------------------------------------
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

Write-Host "=== ${Label}: warmup=${Warmup}s duration=${Duration}s window=$(if ($Window) { $Window } else { 'saved/default' })"
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

    # --- Sample ----------------------------------------------------------------------------------
    Write-Host "Sampling for $Duration s..."
    $memStart = Get-Memory $proc
    $peakPrivateWs = $memStart.privateWorkingSetMB
    $threadsBefore = Get-ThreadTime $proc
    $totalBefore = $proc.TotalProcessorTime.Ticks
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $cpuStartLocal = Get-Date
    while ($clock.Elapsed.TotalSeconds -lt $Duration) {
        $waitMs = [int][math]::Min($MemoryPollSeconds * 1000, ($Duration - $clock.Elapsed.TotalSeconds) * 1000)
        if ($waitMs -gt 0 -and $proc.WaitForExit($waitMs)) { throw "TaskSmack exited while sampling with code $($proc.ExitCode)." }
        $mem = Get-Memory $proc
        if ($null -ne $mem.privateWorkingSetMB -and ($null -eq $peakPrivateWs -or $mem.privateWorkingSetMB -gt $peakPrivateWs)) {
            $peakPrivateWs = $mem.privateWorkingSetMB
        }
    }
    $threadsAfter = Get-ThreadTime $proc
    $proc.Refresh()
    $totalAfter = $proc.TotalProcessorTime.Ticks
    $seconds = $clock.Elapsed.TotalSeconds
    $cpuEndLocal = Get-Date
    $memEnd = Get-Memory $proc

    $toPercent = { param([long]$ticks) [math]::Round($ticks / [TimeSpan]::TicksPerSecond / $seconds * 100.0, 2) }
    $firstThreadId = ($threadsAfter.GetEnumerator() | Sort-Object { $_.Value.Start } | Select-Object -First 1).Key
    $threads = foreach ($tid in $threadsAfter.Keys) {
        if (-not $threadsBefore.ContainsKey($tid)) { continue } # started during the sample: in the total only
        $name = [TaskSmackIdleNative]::ThreadName($tid)
        [pscustomobject]@{
            tid  = $tid
            name = if ($name) { $name } elseif ($tid -eq $firstThreadId) { '(first thread)' } else { '' }
            cpu  = & $toPercent ($threadsAfter[$tid].Ticks - $threadsBefore[$tid].Ticks)
        }
    }
    $threads = @($threads | Sort-Object cpu -Descending)
    $uiThread = $threads | Where-Object name -EQ 'tasksmack-ui' | Select-Object -First 1
    if (-not $uiThread) { $uiThread = $threads | Where-Object tid -EQ $firstThreadId | Select-Object -First 1 }


    $result = [ordered]@{
        schema          = 'tasksmack-idle-windows/1'
        label           = $Label
        binary          = $binaryPath
        binaryVersion   = (Get-Item -LiteralPath $binaryPath).VersionInfo.ProductVersion
        anchor          = $null
        warmupSeconds   = $Warmup
        durationSeconds = [math]::Round($seconds, 1)
        cpuStartUtc     = $cpuStartLocal.ToUniversalTime().ToString('o')
        cpuEndUtc       = $cpuEndLocal.ToUniversalTime().ToString('o')
        requestedWindow = $Window
        window          = $windowInfo
        totalCpu        = & $toPercent ($totalAfter - $totalBefore)
        uiThreadCpu     = if ($uiThread) { $uiThread.cpu } else { $null }
        frames          = $null
        memoryStart     = $memStart
        memoryEnd       = $memEnd
        peakPrivateWorkingSetMB = $peakPrivateWs
        threads         = $threads
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

# The ResizePerf summaries logged while CPU was sampled (each covers the 5 s before it).
$frames = Get-FrameStatistic @(Select-LogWindow -Lines $logLines -From $cpuStartLocal -To $cpuEndLocal)
$result.frames = $frames
$result.anchor = $logLines | Where-Object { $_ -match 'ResizePerfAnchor:' } | Select-Object -First 1

$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Json -Encoding utf8

# --- Report --------------------------------------------------------------------------------------
$fmt = { param($v, $unit = '') if ($null -eq $v) { 'n/a' } else { "$v$unit" } }
Write-Host ''
Write-Host ("{0,-34} {1,12}" -f 'Metric', 'Value')
Write-Host ("{0,-34} {1,12}" -f 'Total CPU (% of one CPU)', $result.totalCpu)
Write-Host ("{0,-34} {1,12}" -f 'UI thread CPU', (& $fmt $result.uiThreadCpu))
Write-Host ("{0,-34} {1,12}" -f 'fps', (& $fmt $frames.fps))
Write-Host ("{0,-34} {1,12}" -f 'frame p95/p99/max (ms)', "$($frames.frameP95Ms)/$($frames.frameP99Ms)/$($frames.frameMaxMs)")
Write-Host ("{0,-34} {1,12}" -f 'loop p95/p99 (ms)', "$($frames.loopP95Ms)/$($frames.loopP99Ms)")
Write-Host ("{0,-34} {1,12}" -f 'ResizePerf summaries', $frames.summaries)
Write-Host ("{0,-34} {1,12}" -f 'Private working set start/end MB', "$(& $fmt $memStart.privateWorkingSetMB)/$(& $fmt $memEnd.privateWorkingSetMB)")
Write-Host ("{0,-34} {1,12}" -f 'Peak private working set MB', (& $fmt $peakPrivateWs))
Write-Host ("{0,-34} {1,12}" -f 'Private bytes start/end MB', "$($memStart.privateBytesMB)/$($memEnd.privateBytesMB)")
Write-Host ("{0,-34} {1,12}" -f 'Window client (px) / DPI', $(if ($result.window) { "$($result.window.clientWidth)x$($result.window.clientHeight) @$($result.window.dpi)" } else { 'n/a' }))
Write-Host ''
Write-Host 'Threads above 0.05%:'
foreach ($t in $threads | Where-Object cpu -GT 0.05) { Write-Host ("  {0,7} {1,-22} {2,8}" -f $t.tid, $t.name, $t.cpu) }
Write-Host ''
Write-Host "JSON: $Json"
$inv = [Globalization.CultureInfo]::InvariantCulture
Write-Host ([string]::Format($inv, 'RESULT label={0} totalCpu={1} uiCpu={2} fps={3} privateWsStartMB={4} privateWsEndMB={5} duration={6}',
        $Label, $result.totalCpu, (& $fmt $result.uiThreadCpu), (& $fmt $frames.fps),
        (& $fmt $memStart.privateWorkingSetMB), (& $fmt $memEnd.privateWorkingSetMB), $result.durationSeconds))
