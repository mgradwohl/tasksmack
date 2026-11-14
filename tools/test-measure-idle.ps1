# Tests for tools/idle-sample.ps1, what tools/measure-idle.ps1 samples beyond CPU time (#3715): the
# -Phases parser, a thread's module from its start address, per-thread CPU and wake-ups between two
# snapshots, dwm.exe's figures, the machine load, the memory series and the timer resolutions seen.
# Recorded figures in plain objects; nothing is launched. The native helper is also run against this
# PowerShell process itself, which needs no elevation. Registered in CTest on Windows with
# PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'idle-sample.ps1')

function Assert-Equal {
    param($Expected, $Actual, [string]$Message)
    if ($null -eq $Expected) {
        if ($null -ne $Actual) { throw "${Message}: expected null, got $Actual" }
        return
    }
    if ($Expected -is [double] -or $Actual -is [double]) {
        if ([math]::Abs([double]$Expected - [double]$Actual) -gt 1e-9) { throw "${Message}: expected $Expected, got $Actual" }
        return
    }
    if ($Expected -cne $Actual) { throw "${Message}: expected [$Expected], got [$Actual]" }
}

function Assert-Throw([scriptblock]$Block, [string]$Pattern, [string]$Message) {
    try { & $Block } catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw "${Message}: threw '$($_.Exception.Message)', not /$Pattern/" }
        return
    }
    throw "${Message}: did not throw"
}

# One snapshot of a process, as TaskSmackIdleSampler.Snapshot gives it: ticks are 100 ns.
function New-Sample([long]$ProcessId, [long]$Kernel, [long]$User, [object[]]$Threads, [long]$PrivateWs = 0, [long]$PrivateBytes = 0, [long]$WorkingSet = 0, [uint32]$Handles = 0) {
    return [pscustomobject]@{
        Pid = $ProcessId; Name = 'TaskSmack.exe'; KernelTicks = $Kernel; UserTicks = $User
        PrivateWorkingSet = $PrivateWs; PrivateBytes = $PrivateBytes; WorkingSet = $WorkingSet; Handles = $Handles
        Threads = @(foreach ($t in $Threads) { [pscustomobject]@{ Tid = $t[0]; KernelTicks = $t[1]; UserTicks = $t[2]; ContextSwitches = [uint32]$t[3] } })
    }
}
$second = [TimeSpan]::TicksPerSecond

# --- Phases -----------------------------------------------------------------------------------------
$p = ConvertFrom-IdlePhaseSpec @('visible:30')
Assert-Equal 1 $p.Count 'the default: one phase'
Assert-Equal 'visible' $p[0].kind 'the default kind'
Assert-Equal 30 $p[0].seconds 'the default length'
$p = ConvertFrom-IdlePhaseSpec @('visible:120,minimized:60', ' restored:60 ')
Assert-Equal 'visible minimized restored' (($p | ForEach-Object kind) -join ' ') 'commas and an array, in order'
Assert-Equal '120 60 60' (($p | ForEach-Object seconds) -join ' ') 'their lengths'
Assert-Throw { ConvertFrom-IdlePhaseSpec @('hidden:10') } 'Invalid phase' 'an unknown kind'
Assert-Throw { ConvertFrom-IdlePhaseSpec @('visible:0') } 'Invalid phase' 'no seconds'
Assert-Throw { ConvertFrom-IdlePhaseSpec @('visible') } 'Invalid phase' 'no length'
Assert-Throw { ConvertFrom-IdlePhaseSpec @(',') } 'No phases' 'nothing at all'

# --- A thread's module ------------------------------------------------------------------------------
$modules = @(
    [pscustomobject]@{ name = 'TaskSmack.exe'; base = 0x7ff600000000; size = 0x1000000 }
    [pscustomobject]@{ name = 'igxelpgicd64.dll'; base = 0x7ffa00000000; size = 0x2000000 }
)
Assert-Equal 'igxelpgicd64.dll+0x8da460' (Resolve-ThreadModule 0x7ffa008da460 $modules) 'inside a module'
Assert-Equal 'TaskSmack.exe+0x0' (Resolve-ThreadModule 0x7ff600000000 $modules) "at a module's base"
Assert-Equal '0x7ffa02000000' (Resolve-ThreadModule 0x7ffa02000000 $modules) 'just past the last byte: no module'
Assert-Equal $null (Resolve-ThreadModule 0 $modules) "a thread that couldn't be queried"

# --- Per-thread CPU and wake-ups --------------------------------------------------------------------
# Over 10 s: the UI thread 0.3 s of CPU and 780 switches, an unnamed driver thread 0.02 s and 1240,
# a sampler that started during the phase (left out), and one that exited (left out of the total's
# switches, as its count went with it).
$before = New-Sample 42 (2 * $second) (1 * $second) @(@(100, 1000000, 1000000, 5000), @(200, 0, 0, 100), @(300, 0, 0, 50))
$after = New-Sample 42 (2 * $second + 2000000) (1 * $second + 2000000) @(@(100, 2500000, 2500000, 5780), @(200, 100000, 100000, 1340), @(400, 9000000, 0, 9))
$names = @{ 100 = 'tasksmack-ui'; 200 = ''; 400 = 'ts-sampler-sys' }
$measured = Measure-ProcessPhase $before $after 10.0 { param($tid) $names[[int]$tid] } { param($tid) if ($tid -eq 200) { 'igxelpgicd64.dll+0x8da460' } else { 'ntdll.dll+0x24990' } }
Assert-Equal 4.0 $measured.totalCpu 'the process total: 0.4 s in 10 s'
Assert-Equal 2 $measured.threads.Count 'only threads in both snapshots'
$ui = $measured.threads | Where-Object tid -EQ 100
Assert-Equal 3.0 $ui.cpu 'the UI thread: 0.3 s in 10 s'
Assert-Equal 78.0 $ui.wakeupsPerSec 'its wake-ups'
Assert-Equal $null $ui.module 'a named thread needs no module'
$driver = $measured.threads | Where-Object tid -EQ 200
Assert-Equal 0.2 $driver.cpu 'the driver thread'
Assert-Equal 124.0 $driver.wakeupsPerSec 'its wake-ups'
Assert-Equal 'igxelpgicd64.dll+0x8da460' $driver.module 'an unnamed thread gets its module'
Assert-Equal 100 $measured.threads[0].tid 'busiest first'
Assert-Equal 202.0 $measured.totalWakeupsPerSec 'the survivors: (780 + 1240) / 10'

# --- dwm.exe ----------------------------------------------------------------------------------------
# Two sessions' dwm.exe; a third only in the second snapshot (started during the phase) is left out.
$dwmBefore = @((New-Sample 7 0 0 (, @(1, 0, 0, 100))), (New-Sample 8 (1 * $second) 0 (, @(2, 0, 0, 10))))
$dwmAfter = @((New-Sample 7 (1 * $second) 0 (, @(1, 0, 0, 5100))), (New-Sample 8 (1 * $second + 500000) 0 (, @(2, 0, 0, 110))), (New-Sample 9 (5 * $second) 0 (, @(3, 0, 0, 99999))))
$dwm = Measure-DwmPhase $dwmBefore $dwmAfter 10.0
Assert-Equal 10.5 $dwm.cpu "dwm's CPU: 1.05 s in 10 s"
Assert-Equal 510.0 $dwm.wakeupsPerSec "dwm's wake-ups: 5100 in 10 s"
Assert-Equal $null (Measure-DwmPhase @() @() 10.0) 'no dwm.exe'

# --- The machine load -------------------------------------------------------------------------------
# 16 CPUs for 10 s: 1600 s in all, 1440 idle, so 10% busy; TaskSmack 4% and dwm 12% of one CPU are 1%.
$load = Measure-MachineLoad @(0, 0, 0) @((1440 * $second), (1500 * $second), (100 * $second)) 4.0 12.0 16
Assert-Equal 10.0 $load.busyPctOfAll 'busy'
Assert-Equal 9.0 $load.restPctOfAll 'the rest of the machine'
Assert-Equal 9.75 (Measure-MachineLoad @(0, 0, 0) @((1440 * $second), (1500 * $second), (100 * $second)) 4.0 $null 16).restPctOfAll 'without dwm'
Assert-Equal $null (Measure-MachineLoad $null @(1, 2, 3) 4.0 $null 16) 'no first reading'
Assert-Equal 0.0 (Measure-MachineLoad @(0, 0, 0) @((1590 * $second), (1600 * $second), 0) 400.0 $null 16).restPctOfAll 'never below zero'

# --- The memory series and the timer ----------------------------------------------------------------
$row = ConvertTo-IdleMemorySample (New-Sample 42 0 0 @(@(1, 0, 0, 0), @(2, 0, 0, 0)) -PrivateWs 50MB -PrivateBytes 75.5MB -WorkingSet 120MB -Handles 760) 65.04 'minimized'
Assert-Equal 65.0 $row.seconds 'elapsed'
Assert-Equal 'minimized' $row.phase 'phase'
Assert-Equal 50.0 $row.privateWorkingSetMB 'private working set'
Assert-Equal 75.5 $row.privateBytesMB 'private bytes'
Assert-Equal 120.0 $row.workingSetMB 'working set'
Assert-Equal 760 $row.handles 'handles'
Assert-Equal 2 $row.threads 'threads'
Assert-Equal '1 15.625' ((Get-DistinctTimerResolution @(15.625, 1, 1, -1, 15.625)) -join ' ') 'distinct, smallest first, failed reads skipped'
Assert-Equal 0 @(Get-DistinctTimerResolution @(-1)).Count 'none read'

# --- The native helper, on this process ---------------------------------------------------------------
$self = [TaskSmackIdleSampler]::Snapshot([long[]]@($PID), [string[]]@())
if ($null -ne $self) {
    $me = @($self | Where-Object Pid -EQ $PID)
    Assert-Equal 1 $me.Count 'this process in its own snapshot'
    if ($me[0].Threads.Count -lt 1) { throw 'no threads in the snapshot' }
    if ($me[0].PrivateBytes -le 0 -or $me[0].WorkingSet -le 0) { throw 'no memory figures in the snapshot' }
    $mods = @((Get-Process -Id $PID).Modules | ForEach-Object { [pscustomobject]@{ name = $_.ModuleName; base = $_.BaseAddress.ToInt64(); size = [long]$_.ModuleMemorySize } })
    $resolved = @($me[0].Threads | ForEach-Object { Resolve-ThreadModule ([TaskSmackIdleSampler]::Win32StartAddress($_.Tid)) $mods } | Where-Object { $_ -match '\.(dll|exe)\+0x' })
    if ($resolved.Count -lt 1) { throw "no thread of this process started in a module it loaded" }
}
else { Write-Host 'No snapshot (a 32-bit PowerShell): the native helper checks are skipped.' }
if ([TaskSmackIdleSampler]::TimerResolutionMs() -le 0) { throw 'no timer resolution' }
if ($null -eq [TaskSmackIdleSampler]::SystemTimes()) { throw 'no system times' }

# --- One phase of a real process -----------------------------------------------------------------
# A hidden PowerShell that only sleeps stands in for TaskSmack: no window, nothing on the desktop.
$psi = [Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path, '-NoProfile -NonInteractive -Command Start-Sleep -Seconds 60')
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$stub = [Diagnostics.Process]::Start($psi)
try {
    Start-Sleep -Seconds 2 # its threads up
    $rows = [Collections.Generic.List[object]]::new()
    $phase = Measure-IdlePhase -Process $stub -Kind 'visible' -Seconds 3 -NameOf { param($tid) '' } -ModuleOf { param($tid) 'stub.dll+0x10' } `
        -FirstThreadId 0 -MemoryPollSeconds 1 -RunClock ([Diagnostics.Stopwatch]::StartNew()) -MemoryRows $rows 6>$null
    Assert-Equal 'visible' $phase.phase 'the phase kind'
    if ([math]::Abs($phase.durationSeconds - 3.0) -gt 0.5) { throw "the phase lasted $($phase.durationSeconds) s, not about 3" }
    if ($phase.threads.Count -lt 1) { throw 'no threads measured' }
    if ($null -ne $self) {
        if ($null -eq $phase.totalWakeupsPerSec) { throw 'no wake-ups with a snapshot' }
        Assert-Equal 'stub.dll+0x10' $phase.threads[0].module "an unnamed thread's module"
        if ($null -eq $phase.machineLoad) { throw 'no machine load' }
        if ($rows.Count -lt 2) { throw "only $($rows.Count) memory rows in 3 s at 1 s" }
        if ($rows[0].privateBytesMB -le 0) { throw 'an empty memory row' }
    }
    if (@($phase.timerResolutionMs).Count -lt 1) { throw 'no timer resolution seen' }
    if ($null -eq $phase.memoryStart.privateBytesMB -or $null -eq $phase.memoryEnd.privateBytesMB) { throw 'no memory at the ends' }
    Assert-Equal $null $phase.frames 'frames are the caller''s, from the log'
    # The JSON keeps one type for the timer resolutions, a list, even with one value
    if ((ConvertTo-Json -InputObject $phase -Depth 6 -Compress) -notmatch '"timerResolutionMs":\[') { throw 'timerResolutionMs is not a list in the JSON' }
}
finally {
    if (-not $stub.HasExited) { $stub.Kill() }
    $stub.Dispose()
}

# --- measure-idle.ps1 keeps version 1's RESULT keys first ----------------------------------------------
$script = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'measure-idle.ps1')
if ($script -notmatch "'RESULT label=\{0\} totalCpu=\{1\} uiCpu=\{2\} fps=\{3\} privateWsStartMB=\{4\} privateWsEndMB=\{5\} duration=\{6\} ") {
    throw "measure-idle.ps1's RESULT line no longer starts with version 1's keys"
}

Write-Host 'test-measure-idle.ps1: all checks passed'
