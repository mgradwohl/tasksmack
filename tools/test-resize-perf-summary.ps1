# Tests for tools/resize-perf-summary.ps1, measure-idle.ps1's ResizePerf summary reader (#3210): a
# summary of a window that drew no frame (frames=0 loopIntervals=0, a minimized window) is left out
# of the frame and loop figures, which are $null (n/a) when no summary drew one. Canned log lines;
# nothing is launched. Registered in CTest on Windows with PowerShell 7; can also run directly with
# pwsh -File. tests/tools/test_resize_perf_summary_awk.py covers measure-idle.sh's twin.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'resize-perf-summary.ps1')

function Assert-Equal {
    param($Expected, $Actual, [string]$Message)
    if ($Expected -is [double] -and $Actual -is [double]) {
        if ([math]::Abs($Expected - $Actual) -gt 1e-9) { throw "${Message}: expected $Expected, got $Actual" }
        return
    }
    if ($null -eq $Expected) {
        if ($null -ne $Actual) { throw "${Message}: expected null, got $Actual" }
        return
    }
    if ($Expected -cne $Actual) { throw "${Message}: expected [$Expected], got [$Actual]" }
}

function Format-Summary([int]$Frames, [string]$Frame, [int]$Loops, [string]$Loop, [string]$Reason = 'idle-progress') {
    # One summary line as Core::logResizePerfTraceSummary logs it; Frame and Loop are avg/p95/p99/max.
    $zero = '0.000/0.000/0.000/0.000'
    return "[2026-10-28 01:39:39.578] [info] ResizePerf[$Reason]: batches=5 events=5 resizeEvents=0 inputEvents=0 " +
    "maxBatchEvents=1 p0Hits=0 skippedFrames=0 maxSingleEvent=0.010 ms frames=$Frames resizeFrames=0 " +
    "frame avg/p95/p99/max=$Frame ms over100=0 over250=0 loopIntervals=$Loops loop avg/p95/p99/max=$Loop ms " +
    "drain avg/p95/p99/max=$zero ms update avg/p95/p99/max=$zero ms render avg/p95/p99/max=$zero ms " +
    "post avg/p95/p99/max=$zero ms swap avg/p95/p99/max=$zero ms"
}

function Assert-Statistic($Stats, [int]$Summaries, $Fps, $Frame, $Loop, [string]$Case) {
    Assert-Equal $Summaries $Stats.summaries "$Case summaries"
    Assert-Equal $Fps $Stats.fps "$Case fps"
    Assert-Equal $Frame[0] $Stats.frameP95Ms "$Case frame p95"
    Assert-Equal $Frame[1] $Stats.frameP99Ms "$Case frame p99"
    Assert-Equal $Frame[2] $Stats.frameMaxMs "$Case frame max"
    Assert-Equal $Loop[0] $Stats.loopP95Ms "$Case loop p95"
    Assert-Equal $Loop[1] $Stats.loopP99Ms "$Case loop p99"
}

$drawn = Format-Summary 40 '1.000/1.500/2.000/2.500' 40 '63.113/64.394/64.721/64.721'
# What a pre-#3210 build logged for the next, empty window: stale p95/p99 beside a zero avg and max.
$staleEmpty = Format-Summary 0 '0.000/9.000/9.500/0.000' 0 '0.000/64.394/64.721/0.000'
$empty = Format-Summary 0 '0.000/0.000/0.000/0.000' 0 '0.000/0.000/0.000/0.000'

# Drawn windows: the worst summary's figures, and fps over 40 * 63.113 + 100 * 20 = 4524.52 ms.
$faster = Format-Summary 100 '0.500/1.000/3.000/4.000' 100 '20.000/25.000/30.000/40.000'
Assert-Statistic (Get-FrameStatistic @($drawn, $faster)) 2 30.94 @(1.5, 3.0, 4.0) @(64.394, 64.721) 'drawn'

# An empty window, stale or not, is left out of the frame and loop figures but still counted.
$smaller = Format-Summary 10 '0.100/0.200/0.300/0.400' 10 '10.000/11.000/12.000/13.000'
Assert-Statistic (Get-FrameStatistic @($smaller, $staleEmpty, $empty, 'Entering main loop')) 3 100.0 @(0.2, 0.3, 0.4) @(11.0, 12.0) 'mixed'

# Only empty windows (a minimized run): n/a for every frame and loop figure, and fps.
$minimized = Get-FrameStatistic @($staleEmpty, $empty)
Assert-Statistic $minimized 2 $null @($null, $null, $null) @($null, $null) 'minimized'
Assert-Equal 'n/a' (Format-Millisecond @($minimized.frameP95Ms, $minimized.frameP99Ms, $minimized.frameMaxMs)) 'minimized frame text'
Assert-Equal 'n/a' (Format-Millisecond @($minimized.loopP95Ms, $minimized.loopP99Ms)) 'minimized loop text'
# The JSON keeps every field, as null.
$json = $minimized | ConvertTo-Json -Compress
Assert-Equal '{"summaries":2,"fps":null,"frameP95Ms":null,"frameP99Ms":null,"frameMaxMs":null,"loopP95Ms":null,"loopP99Ms":null}' $json 'minimized JSON'

# The first frame after a full reset has work but no deliver-to-deliver interval yet.
$first = Format-Summary 1 '5.000/5.000/5.000/5.000' 0 '0.000/0.000/0.000/0.000' 'interaction-end'
Assert-Statistic (Get-FrameStatistic @($first)) 1 $null @(5.0, 5.0, 5.0) @($null, $null) 'first frame'

# No summaries at all.
Assert-Statistic (Get-FrameStatistic @()) 0 $null @($null, $null, $null) @($null, $null) 'none'

Assert-Equal '64.394/64.721' (Format-Millisecond @(64.394, 64.721)) 'loop text'
Assert-Equal '1.5/3/4' (Format-Millisecond @(1.5, 3.0, 4.0)) 'frame text'

Write-Host 'resize-perf-summary.ps1 tests passed'
