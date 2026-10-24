#!/usr/bin/env bash
# Compare TaskSmack's UI between two git refs (#2712): builds each in a scratch worktree (debug preset,
# -j 4, one build at a time), runs the UI scenario suite (UiScenarioTest, #2711) in capture mode, and
# writes the screenshot pairs, a pixel diff per step with the share of pixels that changed, and each
# run's warnings, errors and ImGui assertions into one directory with an index.html
# (tools/ui-compare-report.py). For signing off a UI dependency bump (ImGui, ImPlot, SDL, FreeType):
#
#   tools/ui-compare.sh origin/main renovate/imgui-1.94
#
# Both refs must have the scenario suite (tests/App/test_UiScenariosOnDisplay.cpp). The scenarios run on
# a synthetic machine, so the processes and charts match between runs; System Information still reads
# this machine, and live values (CPU %, times) move a little between runs, so a small change on every
# step is normal. Look at the diffs, not just the numbers.
#
# Dependencies: each worktree reuses this checkout's FetchContent sources (.cache/fetchcontent, or
# --seed) read-only, through FETCHCONTENT_SOURCE_DIR_<NAME>, for every dependency whose GIT_TAG in that
# ref matches the seed's checkout; a dependency the ref pins differently, such as the one being bumped,
# is downloaded into that worktree's own cache as a normal configure would. The scenarios run under Xvfb
# (apt install xvfb) with WAYLAND_DISPLAY unset, as in CI, even when a desktop is up: on a real display
# the pointer's own moves would mix with the scenarios' injected ones; --use-display runs them there.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

OUT=""
SEED="$PROJECT_ROOT/.cache/fetchcontent"
WORK=""
KEEP=false
USE_DISPLAY=false
JOBS=4

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] <ref-a> <ref-b>

Build both refs, run the UI scenarios in capture mode on each, and write a side-by-side report.

Options:
  --out DIR     Report directory (default: ui-compare-<a>-vs-<b> under the current directory)
  --seed DIR    FetchContent sources to reuse (default: $SEED)
  --work DIR    Where the scratch worktrees go (default: a new directory under \${TMPDIR:-/tmp})
  --keep        Keep the worktrees and their builds afterwards
  --use-display Run the scenarios on the current display instead of Xvfb (keep the pointer off the window)
  -h, --help    Show this help
EOF
}

REFS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    --keep) KEEP=true; shift ;;
    --use-display) USE_DISPLAY=true; shift ;;
    -h | --help) usage; exit 0 ;;
    -*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    *) REFS+=("$1"); shift ;;
    esac
done
if [[ ${#REFS[@]} -ne 2 ]]; then
    usage >&2
    exit 2
fi
if [[ "$USE_DISPLAY" == false ]] && ! command -v xvfb-run >/dev/null; then
    echo "xvfb-run not found (apt install xvfb), or pass --use-display" >&2
    exit 1
fi

slug() { tr -c 'A-Za-z0-9._-' '-' <<<"$1" | sed 's/-*$//'; }
[[ -n "$OUT" ]] || OUT="$PWD/ui-compare-$(slug "${REFS[0]}")-vs-$(slug "${REFS[1]}")"
[[ -n "$WORK" ]] || WORK="$(mktemp -d "${TMPDIR:-/tmp}/tasksmack-ui-compare.XXXXXX")"
mkdir -p "$OUT" "$WORK"
OUT="$(cd "$OUT" && pwd)"

WORKTREES=()
cleanup() {
    if [[ "$KEEP" == false ]]; then
        for tree in "${WORKTREES[@]}"; do
            git -C "$PROJECT_ROOT" worktree remove --force "$tree" 2>/dev/null || true
        done
        rm -rf "$WORK"
    else
        echo "Kept the worktrees under $WORK"
    fi
}
trap cleanup EXIT

# "<name> <sha>" for each FetchContent_Declare() with a GIT_TAG under <tree>: the name lower-cased, as
# FetchContent names its <name>-src directory.
declared_pins() {
    local tree="$1"
    # shellcheck disable=SC2016 # the $1 and $2 are awk's fields
    find "$tree" -path "$tree/build" -prune -o -path "$tree/.cache" -prune -o \
        \( -name CMakeLists.txt -o -name '*.cmake' \) -print0 |
        xargs -0 awk '
            /FetchContent_Declare\(/ { declaring = 1; name = ""; next }
            declaring && name == "" && NF { name = tolower($1); next }
            declaring && $1 == "GIT_TAG" { print name, $2; declaring = 0 }
            declaring && /^[[:space:]]*\)/ { declaring = 0 }'
}

# -DFETCHCONTENT_SOURCE_DIR_<NAME>=<seed> for each dependency <tree> pins at the commit the seed has out.
seed_arguments() {
    local tree="$1" name sha seeded head
    declared_pins "$tree" | sort -u | while read -r name sha; do
        seeded="$SEED/$name-src"
        [[ -d "$seeded/.git" ]] || continue
        head="$(git -C "$seeded" rev-parse HEAD 2>/dev/null || true)"
        if [[ -n "$head" && "$head" == "$sha"* ]]; then
            echo "-DFETCHCONTENT_SOURCE_DIR_$(tr '[:lower:]-' '[:upper:]_' <<<"$name")=$seeded"
        else
            echo "  $name: ${sha:0:12} isn't the seed's checkout; downloaded for this ref" >&2
        fi
    done
}

run_side() {
    local side="$1" ref="$2"
    local tree="$WORK/$side"
    echo "== $side: $ref"
    git -C "$PROJECT_ROOT" worktree add --force --detach "$tree" "$ref" >/dev/null
    WORKTREES+=("$tree")
    if [[ ! -f "$tree/tests/App/test_UiScenariosOnDisplay.cpp" ]]; then
        echo "$ref has no UI scenario suite (tests/App/test_UiScenariosOnDisplay.cpp, #2711)" >&2
        exit 1
    fi
    local seeds=()
    mapfile -t seeds < <(seed_arguments "$tree")
    (cd "$tree" && cmake --preset debug "${seeds[@]}" >"$OUT/$side-configure.log" 2>&1) ||
        { echo "Configuring $ref failed: $OUT/$side-configure.log" >&2; exit 1; }
    (cd "$tree" && cmake --build --preset debug -j "$JOBS" --target TaskSmackAppTests >"$OUT/$side-build.log" 2>&1) ||
        { echo "Building $ref failed: $OUT/$side-build.log" >&2; exit 1; }
    rm -rf "${OUT:?}/$side"
    mkdir -p "$OUT/$side"
    local runner=()
    if [[ "$USE_DISPLAY" == false ]]; then
        runner=(xvfb-run -a env -u WAYLAND_DISPLAY)
    fi
    # -V: the suite's whole log, warnings included, whether it passes or not.
    if ! (cd "$tree" && TASKSMACK_UI_CAPTURE_DIR="$OUT/$side" "${runner[@]}" ctest --preset debug -R '^UiScenarioTest$' -V \
        >"$OUT/$side/run.log" 2>&1); then
        echo "  the scenarios failed on $ref (see $OUT/$side/run.log); the report shows what was captured" >&2
    fi
    echo "  $(find "$OUT/$side" -name '*.png' | wc -l) frames"
}

run_side a "${REFS[0]}"
run_side b "${REFS[1]}"
python3 "$SCRIPT_DIR/ui-compare-report.py" "$OUT" "${REFS[0]}" "${REFS[1]}"
