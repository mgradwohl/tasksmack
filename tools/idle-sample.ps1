# What tools/measure-idle.ps1 samples beyond CPU time (#3715), and the arithmetic on it: dot-source it.
#
# None of it needs elevation:
#   - a SystemProcessInformation snapshot (NtQuerySystemInformation class 5) of TaskSmack and dwm.exe:
#     each thread's CPU time and context switches (wake-ups), and the process's private working set,
#     private bytes, working set and handles;
#   - each thread's Win32 start address (NtQueryInformationThread, ThreadQuerySetWin32StartAddress),
#     mapped to the module it is in, so an unnamed thread reads igxelpgicd64.dll+0x8da460 rather than a
#     bare id. A thread that can't be opened (an elevated TaskSmack seen from a normal shell) has none;
#   - the system timer resolution (NtQueryTimerResolution), which a process raising it (timeBeginPeriod)
#     changes for the whole machine;
#   - GetSystemTimes, for how busy the rest of the machine was.
# Anything that fails reads $null, and the figures that need it are left out.
#
# The functions after the native helper take plain objects, so tools/test-measure-idle.ps1 runs them
# on recorded data; nothing here launches anything.

if (-not ('TaskSmackIdleSampler' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public sealed class TaskSmackThreadSample {
    public long Tid; public long KernelTicks; public long UserTicks; public uint ContextSwitches;
}
public sealed class TaskSmackProcessSample {
    public long Pid; public string Name = "";
    public long KernelTicks; public long UserTicks;
    public long PrivateWorkingSet; public long PrivateBytes; public long WorkingSet; public uint Handles;
    public List<TaskSmackThreadSample> Threads = new List<TaskSmackThreadSample>();
}
public static class TaskSmackIdleSampler {
    [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int infoClass, IntPtr buffer, int length, out int returned);
    [DllImport("ntdll.dll")] static extern int NtQueryTimerResolution(out uint maximum, out uint minimum, out uint current);
    [DllImport("ntdll.dll")] static extern int NtQueryInformationThread(IntPtr thread, int infoClass, out IntPtr info, int length, IntPtr returned);
    [DllImport("kernel32.dll")] static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);

    const int SystemProcessInformation = 5;
    const int ThreadQuerySetWin32StartAddress = 9;
    const int StatusInfoLengthMismatch = unchecked((int)0xC0000004);

    // The current resolution in ms, or -1 when it can't be read.
    public static double TimerResolutionMs() {
        uint maximum, minimum, current;
        return NtQueryTimerResolution(out maximum, out minimum, out current) == 0 ? current / 10000.0 : -1;
    }

    // idle, kernel (which includes idle) and user time, in 100-ns units; null when the call fails.
    public static long[] SystemTimes() {
        long idle, kernel, user;
        return GetSystemTimes(out idle, out kernel, out user) ? new long[] { idle, kernel, user } : null;
    }

    // The thread's Win32 start address, or 0 when the thread can't be opened or queried.
    public static long Win32StartAddress(long threadId) {
        const uint THREAD_QUERY_INFORMATION = 0x0040;
        IntPtr thread = OpenThread(THREAD_QUERY_INFORMATION, false, (uint)threadId);
        if (thread == IntPtr.Zero) { return 0; }
        try {
            IntPtr address;
            return NtQueryInformationThread(thread, ThreadQuerySetWin32StartAddress, out address, IntPtr.Size, IntPtr.Zero) == 0 ? address.ToInt64() : 0;
        } finally { CloseHandle(thread); }
    }

    // The processes whose id is in processIds or whose image name (any case) is in names. The offsets
    // are SYSTEM_PROCESS_INFORMATION's and SYSTEM_THREAD_INFORMATION's on 64-bit Windows; null in a
    // 32-bit process or when the query fails.
    public static List<TaskSmackProcessSample> Snapshot(long[] processIds, string[] names) {
        if (IntPtr.Size != 8) { return null; }
        int size = 4 << 20;
        IntPtr buffer = IntPtr.Zero;
        try {
            for (;;) {
                buffer = Marshal.AllocHGlobal(size);
                int returned;
                int status = NtQuerySystemInformation(SystemProcessInformation, buffer, size, out returned);
                if (status == StatusInfoLengthMismatch) {
                    Marshal.FreeHGlobal(buffer); buffer = IntPtr.Zero;
                    size = Math.Max(size * 2, returned + 65536);
                    continue;
                }
                if (status != 0) { return null; }
                break;
            }
            var result = new List<TaskSmackProcessSample>();
            long offset = 0;
            for (;;) {
                IntPtr entry = new IntPtr(buffer.ToInt64() + offset);
                uint next = (uint)Marshal.ReadInt32(entry, 0);
                uint threadCount = (uint)Marshal.ReadInt32(entry, 4);
                long processId = Marshal.ReadIntPtr(entry, 80).ToInt64();
                ushort nameBytes = (ushort)Marshal.ReadInt16(entry, 56);
                IntPtr nameBuffer = Marshal.ReadIntPtr(entry, 64);
                string name = nameBuffer == IntPtr.Zero ? "" : Marshal.PtrToStringUni(nameBuffer, nameBytes / 2);
                bool wanted = Array.IndexOf(processIds, processId) >= 0;
                foreach (string n in names ?? new string[0]) { if (string.Equals(n, name, StringComparison.OrdinalIgnoreCase)) { wanted = true; } }
                if (wanted) {
                    var sample = new TaskSmackProcessSample {
                        Pid = processId, Name = name,
                        PrivateWorkingSet = Marshal.ReadInt64(entry, 8),
                        UserTicks = Marshal.ReadInt64(entry, 40),
                        KernelTicks = Marshal.ReadInt64(entry, 48),
                        Handles = (uint)Marshal.ReadInt32(entry, 96),
                        WorkingSet = Marshal.ReadInt64(entry, 144),
                        PrivateBytes = Marshal.ReadInt64(entry, 200),
                    };
                    for (int i = 0; i < threadCount; i++) {
                        IntPtr thread = new IntPtr(entry.ToInt64() + 256 + i * 80);
                        sample.Threads.Add(new TaskSmackThreadSample {
                            KernelTicks = Marshal.ReadInt64(thread, 0),
                            UserTicks = Marshal.ReadInt64(thread, 8),
                            Tid = Marshal.ReadIntPtr(thread, 48).ToInt64(),
                            ContextSwitches = (uint)Marshal.ReadInt32(thread, 64),
                        });
                    }
                    result.Add(sample);
                }
                if (next == 0) { break; }
                offset += next;
            }
            return result;
        } finally { if (buffer != IntPtr.Zero) { Marshal.FreeHGlobal(buffer); } }
    }
}
'@
}

$IdlePhaseKinds = @('visible', 'minimized', 'restored')

function ConvertFrom-IdlePhaseSpec {
    # "visible:120,minimized:60" (or several such strings) -> [{ kind; seconds }], in order.
    param([string[]]$Spec)
    $phases = foreach ($part in @($Spec | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        $m = [regex]::Match($part, '^(?<kind>[a-z]+):(?<seconds>\d+)$')
        if (-not $m.Success -or $m.Groups['kind'].Value -notin $IdlePhaseKinds -or [int]$m.Groups['seconds'].Value -lt 1) {
            throw "Invalid phase '$part': expected <kind>:<seconds>, with kind one of $($IdlePhaseKinds -join ', ') and seconds at least 1."
        }
        [pscustomobject]@{ kind = $m.Groups['kind'].Value; seconds = [int]$m.Groups['seconds'].Value }
    }
    if (-not $phases) { throw 'No phases given.' }
    return @($phases)
}

function Resolve-ThreadModule {
    # "<module>+0x<offset>" for an address inside one of $Modules ({ name; base; size }), "0x<address>"
    # outside them all, $null for no address (0: the thread couldn't be queried).
    param([long]$Address, [object[]]$Modules)
    if ($Address -le 0) { return $null }
    foreach ($module in $Modules) {
        if ($Address -ge $module.base -and $Address -lt ($module.base + $module.size)) {
            return '{0}+0x{1:x}' -f $module.name, ($Address - $module.base)
        }
    }
    return '0x{0:x}' -f $Address
}

function Measure-ProcessPhase {
    # Per-thread CPU (% of one CPU) and wake-ups per second between two snapshots of one process
    # (TaskSmackProcessSample, or objects shaped like one), for the threads in both. $NameOf and
    # $ModuleOf map a thread id to its name and module ($null when there is none).
    param($Before, $After, [double]$Seconds, [scriptblock]$NameOf, [scriptblock]$ModuleOf)
    $earlier = @{}
    foreach ($t in $Before.Threads) { $earlier[[long]$t.Tid] = $t }
    $toPercent = { param([long]$ticks) [math]::Round($ticks / [TimeSpan]::TicksPerSecond / $Seconds * 100.0, 3) }
    $threads = foreach ($t in $After.Threads) {
        $b = $earlier[[long]$t.Tid]
        if ($null -eq $b) { continue } # started during the phase: in the total only
        $name = & $NameOf ([long]$t.Tid)
        [pscustomobject]@{
            tid           = [long]$t.Tid
            name          = $name
            module        = if ($name) { $null } else { & $ModuleOf ([long]$t.Tid) }
            cpu           = & $toPercent (($t.KernelTicks + $t.UserTicks) - ($b.KernelTicks + $b.UserTicks))
            wakeupsPerSec = [math]::Round(([long]$t.ContextSwitches - [long]$b.ContextSwitches) / $Seconds, 2)
        }
    }
    # A thread that exited during the phase took its switches with it: the total is the survivors'.
    $survivorSwitches = 0L
    foreach ($t in $After.Threads) { if ($earlier.ContainsKey([long]$t.Tid)) { $survivorSwitches += [long]$t.ContextSwitches - [long]$earlier[[long]$t.Tid].ContextSwitches } }
    return [pscustomobject]@{
        totalCpu           = & $toPercent (($After.KernelTicks + $After.UserTicks) - ($Before.KernelTicks + $Before.UserTicks))
        totalWakeupsPerSec = [math]::Round($survivorSwitches / $Seconds, 1)
        threads            = @($threads | Sort-Object cpu -Descending)
    }
}

function Measure-DwmPhase {
    # dwm.exe's CPU (% of one CPU) and wake-ups per second, summed over its instances (one per
    # session) found in both snapshots; $null when there is none.
    param([object[]]$Before, [object[]]$After, [double]$Seconds)
    $cpuTicks = 0L; $switches = 0L; $found = $false
    foreach ($a in $After) {
        $b = $Before | Where-Object { $_.Pid -eq $a.Pid } | Select-Object -First 1
        if ($null -eq $b) { continue }
        $found = $true
        $cpuTicks += ($a.KernelTicks + $a.UserTicks) - ($b.KernelTicks + $b.UserTicks)
        $earlier = @{}
        foreach ($t in $b.Threads) { $earlier[[long]$t.Tid] = $t }
        foreach ($t in $a.Threads) { if ($earlier.ContainsKey([long]$t.Tid)) { $switches += [long]$t.ContextSwitches - [long]$earlier[[long]$t.Tid].ContextSwitches } }
    }
    if (-not $found) { return $null }
    return [pscustomobject]@{
        cpu           = [math]::Round($cpuTicks / [TimeSpan]::TicksPerSecond / $Seconds * 100.0, 3)
        wakeupsPerSec = [math]::Round($switches / $Seconds, 1)
    }
}

function Measure-MachineLoad {
    # How busy the whole machine was (% of all CPUs) from two GetSystemTimes readings ({ idle, kernel,
    # user }, kernel including idle), and the rest of it once TaskSmack's and dwm's CPU (% of one CPU)
    # are taken out: a disturbed run shows up here. $null without both readings.
    param([long[]]$Before, [long[]]$After, [double]$TaskSmackCpu, $DwmCpu, [int]$LogicalCpus)
    if ($null -eq $Before -or $null -eq $After) { return $null }
    $idle = $After[0] - $Before[0]
    $total = ($After[1] - $Before[1]) + ($After[2] - $Before[2])
    if ($total -le 0) { return $null }
    $busy = [math]::Round(($total - $idle) / [double]$total * 100.0, 2)
    $dwm = if ($null -ne $DwmCpu) { [double]$DwmCpu } else { 0.0 }
    return [pscustomobject]@{
        busyPctOfAll   = $busy
        restPctOfAll   = [math]::Round([math]::Max(0.0, $busy - ($TaskSmackCpu + $dwm) / $LogicalCpus), 2)
    }
}

function ConvertTo-IdleMemorySample {
    # One row of the memory series from a process snapshot.
    param($Sample, [double]$Seconds, [string]$Phase)
    return [pscustomobject]@{
        seconds             = [math]::Round($Seconds, 1)
        phase               = $Phase
        privateWorkingSetMB = [math]::Round($Sample.PrivateWorkingSet / 1MB, 2)
        privateBytesMB      = [math]::Round($Sample.PrivateBytes / 1MB, 2)
        workingSetMB        = [math]::Round($Sample.WorkingSet / 1MB, 2)
        handles             = [int]$Sample.Handles
        threads             = @($Sample.Threads).Count
    }
}

function Get-DistinctTimerResolution {
    # The distinct timer resolutions seen (ms), smallest first; readings that failed (negative) skipped.
    param([double[]]$Readings)
    return @($Readings | Where-Object { $_ -gt 0 } | Sort-Object -Unique)
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

function Get-Memory([System.Diagnostics.Process]$Process, $Sample = $null) {
    # From the process's SystemProcessInformation entry when there is one (#3715): any locale, and
    # the same snapshot the thread figures come from.
    if ($null -ne $Sample) {
        return [ordered]@{
            privateWorkingSetMB = [math]::Round($Sample.PrivateWorkingSet / 1MB, 1)
            privateBytesMB      = [math]::Round($Sample.PrivateBytes / 1MB, 1)
            workingSetMB        = [math]::Round($Sample.WorkingSet / 1MB, 1)
        }
    }
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

function Get-ProcessSnapshot([int]$ProcessId, [switch]$WithDwm) {
    # TaskSmack's entry and, with -WithDwm, dwm.exe's: @{ app; dwm }, or $null when no snapshot.
    [string[]]$names = @()
    if ($WithDwm) { $names = @('dwm.exe') }
    $all = [TaskSmackIdleSampler]::Snapshot([long[]]@($ProcessId), $names)
    if ($null -eq $all) { return $null }
    return @{ app = ($all | Where-Object Pid -EQ $ProcessId | Select-Object -First 1); dwm = @($all | Where-Object { $_.Name -ieq 'dwm.exe' }) }
}

function Measure-IdlePhase {
    # Samples one phase of $Process for $Seconds: CPU time (process and threads), wake-ups, memory at
    # each end and every $MemoryPollSeconds (each poll is also a row in $MemoryRows, timed by
    # $RunClock), the timer resolution at each poll, dwm.exe and the machine load. $NameOf and
    # $ModuleOf map a thread id to its name and module. Posts nothing: the caller minimizes or
    # restores the window first. The frames are left $null for the caller to fill from the log;
    # cpuStartLocal and cpuEndLocal are the phase's local times for that, which the caller removes.
    param(
        [System.Diagnostics.Process]$Process,
        [string]$Kind,
        [int]$Seconds,
        [IntPtr]$Window = [IntPtr]::Zero,
        [scriptblock]$NameOf,
        [scriptblock]$ModuleOf,
        $FirstThreadId,
        [int]$MemoryPollSeconds = 5,
        [System.Diagnostics.Stopwatch]$RunClock,
        [System.Collections.Generic.List[object]]$MemoryRows
    )
    Write-Host "Sampling $($Kind) for $($Seconds) s..."
    $snapBefore = Get-ProcessSnapshot $Process.Id -WithDwm
    if ($null -eq $snapBefore) {
        Write-Warning 'No SystemProcessInformation snapshot (a 32-bit PowerShell?): wake-ups, dwm and the machine load are n/a.'
    }
    $sysBefore = [TaskSmackIdleSampler]::SystemTimes()
    $memStart = Get-Memory $Process $(if ($snapBefore) { $snapBefore.app })
    $peakPrivateWs = $memStart.privateWorkingSetMB
    $threadsBefore = Get-ThreadTime $Process
    $Process.Refresh()
    $totalBefore = $Process.TotalProcessorTime.Ticks
    $timerReadings = [Collections.Generic.List[double]]::new()
    $timerReadings.Add([TaskSmackIdleSampler]::TimerResolutionMs())
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $cpuStartLocal = Get-Date
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        $waitMs = [int][math]::Min($MemoryPollSeconds * 1000, ($Seconds - $clock.Elapsed.TotalSeconds) * 1000)
        if ($waitMs -gt 0 -and $Process.WaitForExit($waitMs)) { throw "The process exited while sampling with code $($Process.ExitCode)." }
        $timerReadings.Add([TaskSmackIdleSampler]::TimerResolutionMs())
        $poll = Get-ProcessSnapshot $Process.Id
        $mem = Get-Memory $Process $(if ($poll) { $poll.app })
        if ($null -ne $mem.privateWorkingSetMB -and ($null -eq $peakPrivateWs -or $mem.privateWorkingSetMB -gt $peakPrivateWs)) {
            $peakPrivateWs = $mem.privateWorkingSetMB
        }
        if ($poll -and $poll.app) { $MemoryRows.Add((ConvertTo-IdleMemorySample $poll.app $RunClock.Elapsed.TotalSeconds $Kind)) }
    }
    $snapAfter = Get-ProcessSnapshot $Process.Id -WithDwm
    $sysAfter = [TaskSmackIdleSampler]::SystemTimes()
    $threadsAfter = Get-ThreadTime $Process
    $Process.Refresh()
    $totalAfter = $Process.TotalProcessorTime.Ticks
    $elapsed = $clock.Elapsed.TotalSeconds # not $seconds: PowerShell would assign the [int] parameter
    $cpuEndLocal = Get-Date
    $memEnd = Get-Memory $Process $(if ($snapAfter) { $snapAfter.app })

    $toPercent = { param([long]$ticks) [math]::Round($ticks / [TimeSpan]::TicksPerSecond / $elapsed * 100.0, 2) }
    $totalCpu = & $toPercent ($totalAfter - $totalBefore)
    if ($snapBefore -and $snapAfter -and $snapBefore.app -and $snapAfter.app) {
        $measured = Measure-ProcessPhase $snapBefore.app $snapAfter.app $elapsed $NameOf $ModuleOf
        $threads = @($measured.threads | ForEach-Object { $_.cpu = [math]::Round($_.cpu, 2); $_ })
        $totalWakeups = $measured.totalWakeupsPerSec
        $dwm = Measure-DwmPhase $snapBefore.dwm $snapAfter.dwm $elapsed
    }
    else {
        # Version 1's per-thread CPU, from each thread's processor time
        $rows = foreach ($tid in $threadsAfter.Keys) {
            if (-not $threadsBefore.ContainsKey($tid)) { continue }
            [pscustomobject]@{ tid = $tid; name = & $NameOf ([long]$tid); module = $null
                cpu = & $toPercent ($threadsAfter[$tid].Ticks - $threadsBefore[$tid].Ticks); wakeupsPerSec = $null }
        }
        $threads = @($rows | Sort-Object cpu -Descending)
        $totalWakeups = $null
        $dwm = $null
    }
    $uiThread = $threads | Where-Object name -EQ 'tasksmack-ui' | Select-Object -First 1
    if (-not $uiThread) { $uiThread = $threads | Where-Object tid -EQ $FirstThreadId | Select-Object -First 1 }
    return [ordered]@{
        phase                   = $Kind
        durationSeconds         = [math]::Round($elapsed, 1)
        cpuStartUtc             = $cpuStartLocal.ToUniversalTime().ToString('o')
        cpuEndUtc               = $cpuEndLocal.ToUniversalTime().ToString('o')
        cpuStartLocal           = $cpuStartLocal
        cpuEndLocal             = $cpuEndLocal
        minimized               = if ($Window -ne [IntPtr]::Zero) { [TaskSmackIdleSampler]::IsIconic($Window) } else { $null }
        totalCpu                = $totalCpu
        uiThreadCpu             = if ($uiThread) { $uiThread.cpu } else { $null }
        totalWakeupsPerSec      = $totalWakeups
        uiThreadWakeupsPerSec   = if ($uiThread) { $uiThread.wakeupsPerSec } else { $null }
        timerResolutionMs       = @(Get-DistinctTimerResolution $timerReadings.ToArray())
        dwm                     = $dwm
        machineLoad             = Measure-MachineLoad $sysBefore $sysAfter $totalCpu $(if ($dwm) { $dwm.cpu }) ([Environment]::ProcessorCount)
        frames                  = $null
        memoryStart             = $memStart
        memoryEnd               = $memEnd
        peakPrivateWorkingSetMB = $peakPrivateWs
        threads                 = $threads
    }
}
