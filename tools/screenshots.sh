#!/usr/bin/env bash
# Regenerate the documentation screenshots (#2827): docs/images/screenshots/linux/<view>-<theme>.webp
#
# One TaskSmack launch per shot, sequentially and without injected input: each run gets its own
# TASKSMACK_CONFIG_DIR with a generated config.toml (theme, medium font, 1 s sampling, window size,
# the Command column off: command lines name the machine's users and paths),
# the startup hooks that pick the view (TASKSMACK_WINDOW, _TAB, _MACHINE_TAB, _SELECT_NAME,
# _DETAILS_TAB, _OPEN, and TASKSMACK_DEMO for the shots the startup hooks alone don't reach) and
# TASKSMACK_SCREENSHOT, which writes the frame in-app and quits (CONTRIBUTING.md, "Visual validation
# without input"). Without $DISPLAY the shots run under Xvfb. Same matrix and names as screenshots.ps1.
#
# Matrix: Overview in every theme; Processes and System in the first dark and the first light theme;
# the machine tab's CPU Cores, GPU and Network and I/O sub-tabs (cores, gpu, network; #2869), Process
# Details (overview, gpu, network, and the Overview with its Modules, Open files and Connections
# sections expanded) for one process, Services, Startup, About and Help in the first theme; and three
# frame sequences written as animated WebPs: overview-live (8 s of the Overview),
# walk-live (10 s: the Processes table, the process selected, its Process Details' Network tab) and
# themes-live (9 s: the Overview through every theme, 1.5 s each), all at 12 fps. The overview-*
# shots and themes-live get a 60 s history window and are captured after 60 s, so their charts are
# full.
#
# Requirements: a built TaskSmack; Xvfb when headless (apt install xvfb); for --webp, cwebp and
# img2webp from libwebp (apt install webp). Identity: --hostname tasksmack-demo runs the shots in a
# UTS + user namespace (unshare -r -u), so the hostname tab and System Information show that name and
# the Processes User column reads root; review every image for names before committing it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

EXE="$PROJECT_ROOT/build/release/bin/TaskSmack"
OUT="$PROJECT_ROOT/docs/images/screenshots/linux"
AFTER=8
WINDOW=1440x900
THEMES=windows-dark,ubuntu-light,dracula,nord-light,gruvbox,monochrome-light
PROCESS=""
WEBP=false
LIVE=true
DEMO_HOST=""
TIMEOUT_AFTER_CAPTURE=60
# Full shots (FULL=1): a 60 s history window, captured at the hook's maximum delay so the charts are full.
OVERVIEW_HISTORY_SECONDS=60
OVERVIEW_AFTER=60
LIVE_FPS=12
LIVE_SECONDS=8
WALK_SECONDS=10
# themes-live: each theme for this many tenths of a second, the config's theme first.
THEME_STEP_TENTHS=15
WEBP_QUALITY=80

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Regenerate the documentation screenshots, one TaskSmack launch per shot (no input is injected).

Options:
  --exe PATH        TaskSmack binary (default: build/release/bin/TaskSmack)
  --out DIR         Output directory (default: docs/images/screenshots/linux)
  --after SECONDS   Seconds after startup before each capture (default: $AFTER; the charts need 5 samples at 1 s).
                    overview-* shots and themes-live ignore it: $OVERVIEW_AFTER s with a $OVERVIEW_HISTORY_SECONDS s history window, so their charts are full
  --window WxH      Window size (default: $WINDOW)
  --themes a,b,c    Theme ids; the first is the dark theme, the first containing "light" the light one
                    (default: $THEMES)
  --process NAME    Process for the Process Details and Processes shots (default: systemd, else bash)
  --webp            Convert each PNG to WebP (cwebp -q $WEBP_QUALITY) and each sequence to an animated WebP
  --no-live         Skip the frame sequences (overview-live, walk-live, themes-live; $LIVE_FPS fps)
  --hostname NAME   Run the shots under unshare -r -u with this host name (no real host or user name)
  -h, --help        Show this help
EOF
    exit "${1:-0}"
}

ORIG_ARGS=("$@")
while [[ $# -gt 0 ]]; do
    case "$1" in
        --exe) EXE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --after) AFTER="$2"; shift 2 ;;
        --window) WINDOW="$2"; shift 2 ;;
        --themes) THEMES="$2"; shift 2 ;;
        --process) PROCESS="$2"; shift 2 ;;
        --webp) WEBP=true; shift ;;
        --no-live) LIVE=false; shift ;;
        --hostname) DEMO_HOST="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Error: Unknown argument: $1" >&2; usage 1 ;;
    esac
done

[[ -x "$EXE" ]] || { echo "Error: TaskSmack binary not found at $EXE (build the release preset or pass --exe)" >&2; exit 1; }
if $WEBP && ! command -v cwebp >/dev/null; then
    echo "Error: --webp needs cwebp and img2webp from libwebp: sudo apt install webp" >&2
    exit 1
fi
[[ -n "$PROCESS" ]] || { [[ "$(ps -o comm= -p 1 2>/dev/null)" == systemd ]] && PROCESS=systemd || PROCESS=bash; }
WIDTH="${WINDOW%x*}"
HEIGHT="${WINDOW#*x}"

# Headless: start Xvfb on a free display, large enough for the window, and stop it on exit.
XVFB_PID=""
if [[ -z "${DISPLAY:-}" ]]; then
    command -v Xvfb >/dev/null || { echo "Error: no DISPLAY and no Xvfb (sudo apt install xvfb)" >&2; exit 1; }
    display=99
    while [[ -e "/tmp/.X${display}-lock" ]]; do display=$((display + 1)); done
    Xvfb ":$display" -screen 0 "$((WIDTH + 160))x$((HEIGHT + 100))x24" -nolisten tcp >/dev/null 2>&1 &
    XVFB_PID=$!
    export DISPLAY=":$display"
    for _ in $(seq 1 50); do [[ -S "/tmp/.X11-unix/X$display" ]] && break; sleep 0.1; done
    echo "Xvfb on $DISPLAY (pid $XVFB_PID)"
fi
trap 'if [[ -n "$XVFB_PID" ]]; then kill "$XVFB_PID" 2>/dev/null || true; fi' EXIT

# --hostname: re-run this script inside a UTS + user namespace (the display and options carry over).
if [[ -n "$DEMO_HOST" && -z "${TASKSMACK_SHOTS_NAMESPACED:-}" ]]; then
    # shellcheck disable=SC2016  # the inner bash expands $1 and $@
    TASKSMACK_SHOTS_NAMESPACED=1 unshare -r -u bash -c 'hostname "$1"; shift; exec "$@"' _ "$DEMO_HOST" "$0" "${ORIG_ARGS[@]}"
    exit $?
fi

WORK="$(mktemp -d -t tasksmack-shots.XXXXXX)"
mkdir -p "$OUT" "$WORK/png"
echo "TaskSmack: $EXE"
echo "Output:    $OUT  (work: $WORK; host: $(hostname); user: $(id -un))"
IFS=',' read -r -a THEME_LIST <<< "$THEMES"
DARK="${THEME_LIST[0]}"
LIGHT=""
for t in "${THEME_LIST[@]}"; do [[ "$t" == *light* || "$t" == latte ]] && { LIGHT="$t"; break; }; done

# One shot: name theme [KEY=VALUE hooks...]; SEQUENCE=";frames=N;fps=F" is set by run_live, and
# FULL=1 gives the shot the 60 s history window and the 60 s wait.
FAILED=0
printf '%-30s %5s %8s %10s\n' name exit seconds bytes
run_shot() {
    local name="$1" theme="$2" cfg="$WORK/$1" shot="$WORK/png/$1.png"
    shift 2
    mkdir -p "$cfg"
    local after="$AFTER" history=""
    if [[ -n "${FULL:-}" ]]; then
        after="$OVERVIEW_AFTER"
        history="history_max_seconds = $OVERVIEW_HISTORY_SECONDS"
    fi
    cat > "$cfg/config.toml" <<EOF
[theme]
id = '$theme'
[font]
size = 'medium'
[sampling]
interval_ms = 1000
$history
[window]
width = $WIDTH
height = $HEIGHT
[process_columns]
command = false
EOF
    local start rc=0 ms bytes=0
    start=$(date +%s%N)
    env TASKSMACK_CONFIG_DIR="$cfg" TASKSMACK_WINDOW="$WINDOW" TASKSMACK_SCREENSHOT="$shot;after=$after${SEQUENCE:-}" "$@" \
        timeout "$((after + TIMEOUT_AFTER_CAPTURE))" "$EXE" >"$cfg/stdout.log" 2>&1 || rc=$?
    ms=$(( ($(date +%s%N) - start) / 1000000 ))
    [[ -f "$shot" ]] && bytes=$(stat -c %s "$shot")
    [[ -f "${shot%.png}-0001.png" ]] && bytes=$(stat -c %s "${shot%.png}-0001.png")
    printf '%-30s %5s %8s %10s\n' "$name" "$rc" "$((ms / 1000)).$(printf '%03d' $((ms % 1000)))" "$bytes"
    if [[ $rc -ne 0 || $bytes -eq 0 ]]; then
        FAILED=$((FAILED + 1))
        tail -n 3 "${TMPDIR:-/tmp}/tasksmack-debug.log" 2>/dev/null | sed 's/^/    /'
        return 0
    fi
    if [[ -n "${SEQUENCE:-}" ]]; then
        return 0
    elif $WEBP; then
        cwebp -quiet -q "$WEBP_QUALITY" "$shot" -o "$OUT/$name.webp"
    else
        cp "$shot" "$OUT/$name.png"
    fi
}

# A frame sequence: name theme seconds [KEY=VALUE hooks...] (TASKSMACK_DEMO among them plays while the
# frames are captured; its times count from the settle point, as the capture's `after` does), joined
# into <name>.webp when --webp and img2webp are there; otherwise the frames stay in the work dir.
run_live() {
    local name="$1" theme="$2" seconds="$3"
    shift 3
    SEQUENCE=";frames=$((seconds * LIVE_FPS));fps=$LIVE_FPS" run_shot "$name" "$theme" "$@"
    local frames=("$WORK"/png/"$name"-*.png)
    if $WEBP && command -v img2webp >/dev/null && [[ -f "${frames[0]}" ]]; then
        img2webp -loop 0 -mixed -d "$((1000 / LIVE_FPS))" -q "$WEBP_QUALITY" "${frames[@]}" -o "$OUT/$name.webp" >/dev/null
        echo "$name.webp: ${#frames[@]} frames, $(stat -c %s "$OUT/$name.webp") bytes"
    else
        echo "$name frames left in $WORK/png (img2webp -loop 0 -d $((1000 / LIVE_FPS)) ... builds the animation)"
    fi
}

for t in "${THEME_LIST[@]}"; do
    FULL=1 run_shot "overview-$t" "$t" TASKSMACK_TAB=overview
done
for t in "$DARK" "$LIGHT"; do
    [[ -n "$t" ]] || continue
    run_shot "processes-$t" "$t" TASKSMACK_TAB=Processes TASKSMACK_SELECT_NAME="$PROCESS"
    run_shot "system-$t" "$t" TASKSMACK_TAB=System
done
# The machine tab's sub-tabs (#2869), after the usual wait: the GPU and network charts need a few samples.
for sub in cores gpu network; do
    run_shot "$sub-$DARK" "$DARK" TASKSMACK_TAB=overview TASKSMACK_MACHINE_TAB="$sub"
done
for tab in overview gpu network; do
    run_shot "details-$tab-$DARK" "$DARK" TASKSMACK_SELECT_NAME="$PROCESS" TASKSMACK_DETAILS_TAB="$tab"
done
# The Overview's collapsed sections, each drawn open by a one-step demo script a second after the
# startup selection has settled.
for section in modules openfiles connections; do
    run_shot "details-$section-$DARK" "$DARK" TASKSMACK_SELECT_NAME="$PROCESS" TASKSMACK_DETAILS_TAB=overview TASKSMACK_DEMO="1:section=$section"
done
run_shot "services-$DARK" "$DARK" TASKSMACK_TAB=Services
run_shot "startup-$DARK" "$DARK" TASKSMACK_TAB=Startup
run_shot "about-$DARK" "$DARK" TASKSMACK_TAB=overview TASKSMACK_OPEN=about
run_shot "help-$DARK" "$DARK" TASKSMACK_TAB=overview TASKSMACK_OPEN=help

if $LIVE; then
    FULL=1 run_live overview-live "$DARK" "$LIVE_SECONDS" TASKSMACK_TAB=overview
    # From the Overview: the Processes table 1 s into the capture, the process selected 2 s later,
    # its Process Details' Network tab 3 s after that.
    run_live walk-live "$DARK" "$WALK_SECONDS" TASKSMACK_TAB=overview \
        TASKSMACK_DEMO="$((AFTER + 1)):tab=processes,$((AFTER + 3)):select=name:$PROCESS,$((AFTER + 6)):details=network"
    # The Overview in the first theme, then each of the others for THEME_STEP_TENTHS tenths of a second.
    steps=""
    tenths=$((OVERVIEW_AFTER * 10))
    for t in "${THEME_LIST[@]:1}"; do
        tenths=$((tenths + THEME_STEP_TENTHS))
        steps="${steps:+$steps,}$((tenths / 10)).$((tenths % 10)):theme=$t"
    done
    themes_seconds=$(( (${#THEME_LIST[@]} * THEME_STEP_TENTHS + 9) / 10 ))
    demo=()
    [[ -n "$steps" ]] && demo=(TASKSMACK_DEMO="$steps")
    FULL=1 run_live themes-live "$DARK" "$themes_seconds" TASKSMACK_TAB=overview "${demo[@]}"
fi

echo ""
echo "Wrote $(find "$OUT" -maxdepth 1 -type f | wc -l) files to $OUT ($FAILED failed); work files in $WORK"
[[ $FAILED -eq 0 ]]
