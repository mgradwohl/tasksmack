# Reading the app's ResizePerf summary lines (Core/ResizePerfTrace.h's logResizePerfTraceSummary),
# dot-sourced by measure-idle.ps1 and tested by test-resize-perf-summary.ps1 (#3210). Kept in step
# with tools/resize-perf-summary.awk, measure-idle.sh's reader. Works on PowerShell 7.0 and later.

function Get-FrameStatistic([string[]]$Lines) {
    # The ResizePerf summaries among $Lines -> fps, frame p95/p99/max, loop p95/p99, as
    # measure-idle.sh computes them: fps is the summaries' loop intervals divided by their summed
    # duration, and each percentile or max is the worst summary's.
    #
    # A summary of a window that drew nothing (frames=0, so loopIntervals=0, as when the window is
    # minimized) has no frame or loop figures of its own; before #3210 the app still printed the
    # previous drawn window's p95/p99 on it, so it is left out of those figures here, whichever
    # build wrote it. When no summary in $Lines drew a frame, the frame figures are $null (n/a), and
    # with no loop interval the loop figures and fps are too.
    $stats = [ordered]@{ summaries = 0; fps = $null; frameP95Ms = $null; frameP99Ms = $null; frameMaxMs = $null; loopP95Ms = $null; loopP99Ms = $null }
    $intervals = 0.0
    $loopMs = 0.0
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $worst = { param($current, [string]$text) $value = [double]::Parse($text, $inv); if ($null -eq $current -or $value -gt $current) { $value } else { $current } }
    foreach ($line in $Lines) {
        if ($line -notmatch 'ResizePerf\[') { continue }
        $loop = [regex]::Match($line, '\bloopIntervals=(\d+) loop avg/p95/p99/max=([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)')
        if (-not $loop.Success) { continue }
        $stats.summaries++
        $count = [double]::Parse($loop.Groups[1].Value, $inv)
        if ($count -gt 0) {
            $intervals += $count
            $loopMs += $count * [double]::Parse($loop.Groups[2].Value, $inv)
            $stats.loopP95Ms = & $worst $stats.loopP95Ms $loop.Groups[3].Value
            $stats.loopP99Ms = & $worst $stats.loopP99Ms $loop.Groups[4].Value
        }
        $frame = [regex]::Match($line, '\bframes=(\d+) .*?\bframe avg/p95/p99/max=([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)')
        if ($frame.Success -and [int64]::Parse($frame.Groups[1].Value, $inv) -gt 0) {
            $stats.frameP95Ms = & $worst $stats.frameP95Ms $frame.Groups[3].Value
            $stats.frameP99Ms = & $worst $stats.frameP99Ms $frame.Groups[4].Value
            $stats.frameMaxMs = & $worst $stats.frameMaxMs $frame.Groups[5].Value
        }
    }
    if ($loopMs -gt 0) { $stats.fps = [math]::Round($intervals * 1000.0 / $loopMs, 2) }
    return $stats
}

function Format-Millisecond([object[]]$Values) {
    # "p95/p99/max" for the report, or "n/a" when the figures are missing ($null).
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $text = [Collections.Generic.List[string]]::new()
    foreach ($value in $Values) {
        if ($null -eq $value) { return 'n/a' }
        $text.Add(([double]$value).ToString($inv))
    }
    return ($text -join '/')
}
