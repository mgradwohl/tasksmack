#!/usr/bin/env bash
# Headless native-Wayland checks (#2724): TaskSmack on a headless sway and a headless labwc, driven the way a
# user's pointer drives it. What the on-display tests can't do on native Wayland -- the compositor runs the
# title-bar drag, the border resize and maximize there -- is done here with a real (virtual) pointer and
# checked against the compositor's own idea of the window.
#
#   sway:  the on-display suites; a title-bar drag and a right-edge resize, checked with swaymsg -t get_tree;
#          a second output at scale 1.5, the window moved onto it, then the output unplugged (the window
#          must survive, back on the first); the size saved on a close the compositor asks for, and
#          restored on the next start
#   labwc: the Maximize/Restore checks (sway, a tiling compositor, never maximizes)
#
# Usage: tools/wayland-matrix.sh [--build-dir DIR] [--compositors sway,labwc] [--logs DIR]
# Needs: sway (swaymsg), labwc, wayland-scanner, pkg-config + wayland-client, cc, python3; the build's
# bin/TaskSmack and tests/TaskSmackAppTests. GNOME and KDE stay manual (docs/dev/wayland-test-matrix.md).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build/debug"
COMPOSITORS="sway,labwc"
LOG_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --compositors) COMPOSITORS="$2"; shift 2 ;;
        --logs) LOG_DIR="$2"; shift 2 ;;
        -h | --help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

APP="$BUILD_DIR/bin/TaskSmack"
APP_TESTS="$BUILD_DIR/tests/TaskSmackAppTests"
for needed in "$APP" "$APP_TESTS"; do
    [[ -x "$needed" ]] || { echo "Missing $needed: build the preset first" >&2; exit 2; }
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/wlm.XXXXXX")"
LOG_DIR="${LOG_DIR:-$WORK/logs}"
mkdir -p "$LOG_DIR"
# Short: a compositor's socket path must fit in 108 bytes (sockaddr_un), and a long runtime dir breaks it.
RUNTIME="$(mktemp -d /tmp/wlr.XXXXXX)"
chmod 700 "$RUNTIME"
PIDS=()
FAILURES=()

cleanup() {
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1" >&2; FAILURES+=("$1"); }

# The virtual pointer (tools/wayland/virtual-pointer.c): a headless compositor's seat has no pointer otherwise.
build_virtual_pointer() {
    local out="$WORK/vp"
    mkdir -p "$out"
    wayland-scanner client-header "$SCRIPT_DIR/wayland/wlr-virtual-pointer-unstable-v1.xml" \
        "$out/wlr-virtual-pointer-unstable-v1-client-protocol.h"
    wayland-scanner private-code "$SCRIPT_DIR/wayland/wlr-virtual-pointer-unstable-v1.xml" "$out/wlr-virtual-pointer-protocol.c"
    # shellcheck disable=SC2046 # pkg-config's flags are separate words
    cc -O2 -Wall -Wextra -I"$out" -o "$out/virtual-pointer" "$SCRIPT_DIR/wayland/virtual-pointer.c" \
        "$out/wlr-virtual-pointer-protocol.c" $(pkg-config --cflags --libs wayland-client)
    VIRTUAL_POINTER="$out/virtual-pointer"
}

# Waits up to 10 s for the compositor's Wayland socket in $1; echoes its name.
wait_for_socket() {
    local dir="$1"
    for _ in $(seq 1 100); do
        local socket
        socket="$(find "$dir" -maxdepth 1 -name 'wayland-[0-9]*' ! -name '*.lock' -printf '%f\n' 2>/dev/null | head -1)"
        if [[ -n "$socket" ]]; then
            echo "$socket"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# ---------- sway ----------

sway_rect() { # x y width height of TaskSmack's window, or nothing
    swaymsg -t get_tree | python3 -c '
import json, sys
def walk(node):
    if node.get("app_id") == "TaskSmack":
        r = node["rect"]
        print(r["x"], r["y"], r["width"], r["height"])
        sys.exit()
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        walk(child)
walk(json.load(sys.stdin))'
}

sway_window_output() { # the output TaskSmack's window is on
    swaymsg -t get_tree | python3 -c '
import json, sys
def walk(node, output):
    if node.get("type") == "output":
        output = node["name"]
    if node.get("app_id") == "TaskSmack":
        print(output)
        sys.exit()
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        walk(child, output)
walk(json.load(sys.stdin), "")'
}

start_app() { # $1: config home; leaves APP_PID
    env -u DISPLAY SDL_VIDEODRIVER=wayland TASKSMACK_SYNTHETIC=processes=60,seed=2724 XDG_CONFIG_HOME="$1" \
        "$APP" >>"$LOG_DIR/app.log" 2>&1 &
    APP_PID=$!
    PIDS+=("$APP_PID")
    for _ in $(seq 1 100); do
        [[ -n "$(sway_rect)" ]] && return 0
        sleep 0.1
    done
    return 1
}

vp() { # one virtual-pointer command; waits for its "ok"
    echo "$1" >&"${VP[1]}"
    local reply=""
    read -r -u "${VP[0]}" reply || true
    [[ "$reply" == ok ]] || echo "  (virtual pointer answered '$reply' to '$1')" >&2
}

# Presses at ($1, $2), moves by ($3, $4) in five steps, releases.
vp_drag() {
    vp "abs $1 $2 1920 1080"
    sleep 0.2
    vp "move 1 0"
    vp "move -1 0"
    sleep 0.2
    vp press
    sleep 0.2
    local _
    for _ in 1 2 3 4 5; do
        vp "move $(($3 / 5)) $(($4 / 5))"
        sleep 0.1
    done
    vp release
    sleep 0.5
}

run_sway() {
    echo "== sway (headless) =="
    local dir="$RUNTIME/sway"
    mkdir -p "$dir"
    chmod 700 "$dir"
    printf 'xwayland disable\noutput HEADLESS-1 resolution 1920x1080 position 0 0\nfor_window [app_id=".*"] floating enable\n' >"$WORK/sway.conf"
    env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$dir" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
        WLR_RENDERER=pixman sway -c "$WORK/sway.conf" >"$LOG_DIR/sway.log" 2>&1 &
    PIDS+=($!)
    local socket
    socket="$(wait_for_socket "$dir")" || { fail "sway didn't start (see $LOG_DIR/sway.log)"; return; }
    export XDG_RUNTIME_DIR="$dir" WAYLAND_DISPLAY="$socket"
    SWAYSOCK="$(find "$dir" -maxdepth 1 -name 'sway-ipc.*.sock' | head -1)"
    export SWAYSOCK

    # 1. The on-display suites, on native Wayland.
    if env -u DISPLAY SDL_VIDEODRIVER=wayland TASKSMACK_REQUIRE_DISPLAY=1 "$APP_TESTS" --gtest_filter='-UiScenarioTest.*' \
        >"$LOG_DIR/sway-app-tests.log" 2>&1; then
        pass "on-display tests"
    else
        fail "on-display tests (see $LOG_DIR/sway-app-tests.log)"
    fi
    if env -u DISPLAY SDL_VIDEODRIVER=wayland TASKSMACK_REQUIRE_DISPLAY=1 TASKSMACK_SYNTHETIC=processes=120,seed=2711 \
        "$APP_TESTS" --gtest_filter='UiScenarioTest.*' >"$LOG_DIR/sway-ui-scenarios.log" 2>&1; then
        pass "UI scenarios"
    else
        fail "UI scenarios (see $LOG_DIR/sway-ui-scenarios.log)"
    fi

    # 2. A real pointer: the compositor drags and resizes.
    coproc VP { "$VIRTUAL_POINTER" 2>>"$LOG_DIR/virtual-pointer.log"; }
    local ready
    read -r -u "${VP[0]}" ready
    [[ "$ready" == ready ]] || { fail "virtual pointer: $ready"; return; }
    local config="$WORK/config"
    start_app "$config" || { fail "TaskSmack didn't map a window (see $LOG_DIR/app.log)"; return; }
    local x y w h nx ny nw nh
    read -r x y w h <<<"$(sway_rect)"
    vp_drag $((x + w * 4 / 10)) $((y + 16)) 150 100
    read -r nx ny nw nh <<<"$(sway_rect)"
    if ((nx - x >= 145 && nx - x <= 155 && ny - y >= 95 && ny - y <= 105 && nw == w && nh == h)); then
        pass "title-bar drag moved the window by (150, 100)"
    else
        fail "title-bar drag: ($x, $y) ${w}x$h became ($nx, $ny) ${nw}x$nh, expected a (150, 100) move"
    fi
    read -r x y w h <<<"$(sway_rect)"
    vp_drag $((x + w - 2)) $((y + h / 2)) 120 0
    read -r nx ny nw nh <<<"$(sway_rect)"
    if ((nw - w >= 100 && nw - w <= 130 && nx == x)); then
        pass "right-edge resize widened it by $((nw - w)) px"
    else
        fail "right-edge resize: ${w}x$h at x $x became ${nw}x$nh at x $nx, expected about 120 px wider"
    fi

    # 3. A second output at scale 1.5: the window moves there, then the output goes away under it.
    swaymsg -q create_output
    local second
    second="$(swaymsg -t get_outputs | python3 -c 'import json,sys; print([o["name"] for o in json.load(sys.stdin) if o["name"]!="HEADLESS-1"][0])')"
    swaymsg -q "output $second resolution 1280x720 position 1920 0 scale 1.5"
    swaymsg -q "[app_id=TaskSmack] move to output $second"
    sleep 1
    if [[ "$(sway_window_output)" == "$second" ]] && kill -0 "$APP_PID" 2>/dev/null; then
        pass "moved onto a second output at scale 1.5"
    else
        fail "the window isn't on $second at scale 1.5 (on '$(sway_window_output)')"
    fi
    swaymsg -q "output $second unplug"
    sleep 1
    if kill -0 "$APP_PID" 2>/dev/null && [[ "$(sway_window_output)" == HEADLESS-1 ]]; then
        pass "survived its output being unplugged, back on HEADLESS-1"
    else
        fail "after unplugging $second: running=$(kill -0 "$APP_PID" 2>/dev/null && echo yes || echo no), on '$(sway_window_output)'"
    fi

    # 4. The size it was closed at, saved and reopened at.
    read -r x y w h <<<"$(sway_rect)"
    swaymsg -q "[app_id=TaskSmack] kill" # the compositor's close, as the user's would be
    local _
    for _ in $(seq 1 100); do
        kill -0 "$APP_PID" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$APP_PID" 2>/dev/null; then
        fail "TaskSmack didn't close when the compositor asked"
    else
        start_app "$config" || { fail "TaskSmack didn't reopen"; return; }
        read -r nx ny nw nh <<<"$(sway_rect)"
        if ((nw == w && nh == h)); then
            pass "reopened at the ${w}x$h it was closed at"
        else
            fail "closed at ${w}x$h, reopened at ${nw}x$nh"
        fi
        swaymsg -q "[app_id=TaskSmack] kill"
    fi
    echo quit >&"${VP[1]}" # it exits without an answer
    unset SWAYSOCK
}

# ---------- labwc ----------

run_labwc() {
    echo "== labwc (headless) =="
    local dir="$RUNTIME/labwc"
    mkdir -p "$dir" "$WORK/labwc"
    chmod 700 "$dir"
    printf '<?xml version="1.0"?>\n<labwc_config><core><decoration>client</decoration></core></labwc_config>\n' >"$WORK/labwc/rc.xml"
    local -a launch=(env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$dir" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1
        WLR_RENDERER=pixman labwc -C "$WORK/labwc")
    # labwc always starts Xwayland, which needs a sticky /tmp/.X11-unix; WSLg's isn't, so give labwc its own.
    if [[ -d /tmp/.X11-unix && ! -k /tmp/.X11-unix ]]; then
        unshare -rm sh -c 'mount -t tmpfs -o mode=1777 tmpfs /tmp/.X11-unix && exec "$@"' sh "${launch[@]}" >"$LOG_DIR/labwc.log" 2>&1 &
    else
        "${launch[@]}" >"$LOG_DIR/labwc.log" 2>&1 &
    fi
    PIDS+=($!)
    local socket
    socket="$(wait_for_socket "$dir")" || { fail "labwc didn't start (see $LOG_DIR/labwc.log)"; return; }
    if env -u DISPLAY XDG_RUNTIME_DIR="$dir" WAYLAND_DISPLAY="$socket" SDL_VIDEODRIVER=wayland TASKSMACK_REQUIRE_DISPLAY=1 \
        TASKSMACK_COMPOSITOR_MAXIMIZES=1 "$APP_TESTS" --gtest_filter='*TitleBarButtonsDoWhatTheySay*:*TheHitTestCallbackAnswersForTheRealWindow*' \
        >"$LOG_DIR/labwc-app-tests.log" 2>&1; then
        pass "Maximize and Restore, and the maximized window's hit test"
    else
        fail "Maximize/Restore (see $LOG_DIR/labwc-app-tests.log)"
    fi
}

build_virtual_pointer
IFS=',' read -r -a WANTED <<<"$COMPOSITORS"
for compositor in "${WANTED[@]}"; do
    case "$compositor" in
        sway) run_sway ;;
        labwc) run_labwc ;;
        *) echo "Unknown compositor: $compositor" >&2; exit 2 ;;
    esac
done

echo
if [[ ${#FAILURES[@]} -gt 0 ]]; then
    echo "${#FAILURES[@]} check(s) failed; logs in $LOG_DIR" >&2
    exit 1
fi
echo "All Wayland checks passed; logs in $LOG_DIR"
