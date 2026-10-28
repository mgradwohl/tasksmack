# The app's ResizePerf summary lines (Core/ResizePerfTrace.h's logResizePerfTraceSummary) -> one
# line: "summaries fps frameP95 frameP99 frameMax loopP95 loopP99". Read by tools/measure-idle.sh
# (awk -f) and tested by tests/tools/test_resize_perf_summary_awk.py; kept in step with
# Get-FrameStatistic in tools/resize-perf-summary.ps1, measure-idle.ps1's reader (#3210).
#
# fps is the summaries' loop intervals divided by their summed duration (0 with none); each
# percentile or max is the worst summary's. A summary of a window that drew nothing (frames=0, so
# loopIntervals=0, as when the window is minimized) has no frame or loop figures of its own; before
# #3210 the app still printed the previous drawn window's p95/p99 on it, so it is left out of those
# figures, whichever build wrote it. With no summary that drew a frame, the frame figures print as
# n/a, and with no loop interval the loop figures do too.
/ResizePerf\[/ {
    lines++
    frameCount = -1
    loopCount = 0
    for (i = 1; i <= NF; i++) {
        # frames=N precedes "frame avg/...", and loopIntervals=N precedes "loop avg/...": N weights
        # that avg, and N=0 means the figures after it are not this window's.
        if ($i ~ /^frames=/) { frameCount = substr($i, 8) + 0 }
        if ($i ~ /^loopIntervals=/) { loopCount = substr($i, 15) + 0 }
        if ($i ~ /^avg\/p95\/p99\/max=/ && i > 1 && ($(i - 1) == "frame" || $(i - 1) == "loop")) {
            split(substr($i, index($i, "=") + 1), v, "/")
            if ($(i - 1) == "frame") {
                if (frameCount != 0) {
                    if (!frames || v[2] > fp95) fp95 = v[2] + 0
                    if (!frames || v[3] > fp99) fp99 = v[3] + 0
                    if (!frames || v[4] > fmax) fmax = v[4] + 0
                    frames = 1
                }
            } else if (loopCount > 0) {
                intervals += loopCount
                loopms += v[1] * loopCount
                if (!loops || v[2] > lp95) lp95 = v[2] + 0
                if (!loops || v[3] > lp99) lp99 = v[3] + 0
                loops = 1
            }
        }
    }
}
END {
    fps = (loopms > 0) ? intervals * 1000 / loopms : 0
    printf "%d %.2f %s %s %s %s %s\n", lines, fps, ms(frames, fp95), ms(frames, fp99), ms(frames, fmax), ms(loops, lp95), ms(loops, lp99)
}
function ms(present, value) { return present ? sprintf("%.3f", value) : "n/a" }
