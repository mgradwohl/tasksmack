#!/usr/bin/env bash
# release-smoke-check.sh -- Launch a packaged Linux TaskSmack twice and check that it quits cleanly.
#
# Usage:
#   release-smoke-check.sh <label> <TaskSmack binary> <log prefix> <config-v0.8.0.toml>
#
# release.yml's smoke check of a packaged Linux binary: validate-linux-package runs it on the
# installed .deb and the extracted .tar.gz before anything is published, and
# verify-published-release on the .tar.gz downloaded from the release (#2721). It launches with
# SDL's "offscreen" video driver, so it needs no X server, only Mesa's llvmpipe (libgl1-mesa-dri,
# libegl1, libegl-mesa0).
#
# A bounded "is it still running" check alone doesn't prove assets loaded -- src/UI/AssetPath.cpp
# and Theme.cpp both fall back gracefully on missing fonts/themes, so a broken installed/extracted
# asset layout would still pass a plain liveness check. Assert the actual log lines that only appear
# once the icon font and at least one theme have loaded from the real assets directory.
#
# Each run quits by itself rather than being killed (#2720): TASKSMACK_SCREENSHOT (#2827) writes one
# frame ALIVE_SECONDS after startup and closes the window through the same close request as the
# title bar's Close button, so every layer detaches and ShellLayer::onDetach() saves config.toml
# into the run's own TASKSMACK_CONFIG_DIR. The run must exit 0 (one that wrote no frame exits
# non-zero) within QUIT_TIMEOUT_SECONDS, log no [error] or [critical] line, and leave a config.toml
# with its [theme] table: with no input nothing writes the file before the shutdown's save, so its
# being there proves that save ran.
#
# Each binary runs twice (#2726): from an empty config dir, then from a copy of the config.toml
# v0.8.0 saved (tests/App/fixtures/config-v0.8.0.toml). The second run must save the file in today's
# format (config_version) and keep the settings changed in v0.8.0's UI when the fixture was made
# (tests/App/fixtures/README.md), each of which differs from the default. validate-linux-upgrade
# checks the same settings; keep the two lists in step.

set -uo pipefail
if [[ $# -ne 4 ]]; then
  echo "Usage: $0 <label> <TaskSmack binary> <log prefix> <config-v0.8.0.toml>" >&2
  exit 2
fi
label="$1"
bin="$2"
log_prefix="$3"
fixture="$4"
ALIVE_SECONDS=5
QUIT_TIMEOUT_SECONDS=60
# Section, key and the value as TaskSmack writes it.
FIXTURE_SETTINGS=(
  "theme id 'dracula'"
  "font size 'large'"
  "sampling interval_ms 2000"
  "sampling history_max_seconds 600"
  "ui show_privilege_notice false"
  "process_columns threads true"
  "process_columns user false"
)

# The value of <key> in [<section>] of a config.toml, as written.
toml_value() {
  awk -v want="$2" -v key="$3" '
    /^\[/ { section = substr($0, 2, length($0) - 2); next }
    section == want && $1 == key && $2 == "=" { sub(/^[^=]*= /, ""); print; exit }
  ' "$1"
}

# launch <run label> <log> [<config.toml to start from>]
launch() {
  local run="$1" log="$2" seed="${3:-}"
  local work config_dir shot config status
  work=$(mktemp -d)
  config_dir="$work/config"
  shot="$work/smoke.png"
  config="$config_dir/config.toml"
  if [[ -n "$seed" ]]; then
    mkdir -p "$config_dir"
    cp "$seed" "$config"
  fi
  SDL_VIDEODRIVER=offscreen TASKSMACK_LOG_LEVEL=debug TASKSMACK_CONFIG_DIR="$config_dir" \
    TASKSMACK_SCREENSHOT="$shot;after=$ALIVE_SECONDS" timeout "$QUIT_TIMEOUT_SECONDS" "$bin" > "$log" 2>&1
  status=$?
  cat "$log"
  if [[ "$status" -eq 124 ]]; then
    echo "$run: TaskSmack didn't quit by itself within $QUIT_TIMEOUT_SECONDS s (TASKSMACK_SCREENSHOT asks it to after $ALIVE_SECONDS s); the timeout killed it." >&2
    exit 1
  fi
  if [[ "$status" -ne 0 ]]; then
    echo "$run: TaskSmack exited with status $status; a clean quit after its screenshot exits 0." >&2
    exit 1
  fi
  if [[ ! -s "$shot" ]]; then
    echo "$run: TaskSmack exited 0 but wrote no screenshot at $shot." >&2
    exit 1
  fi
  if grep -E '\[(critical|error)\]' "$log" >&2; then
    echo "$run: the log has the [critical]/[error] line(s) above." >&2
    exit 1
  fi
  if [[ ! -f "$config" ]]; then
    echo "$run: no config.toml in $config_dir after the quit; the settings weren't saved on exit." >&2
    exit 1
  fi
  echo "=== $run config.toml ==="
  cat "$config"
  echo
  echo "=== end of config.toml ==="
  if ! grep -A1 -x '\[theme\]' "$config" | grep -Eqx "id = ('[^']+'|\"[^\"]+\")"; then
    echo "$run: config.toml in $config_dir has no [theme] table with an id; the save wrote something else." >&2
    exit 1
  fi
  if [[ -n "$seed" ]]; then
    # v0.8.0 wrote no config_version, so its being there proves the shutdown's save ran.
    if ! grep -Eqx 'config_version = [0-9]+' "$config"; then
      echo "$run: config.toml has no config_version; the shutdown's save didn't rewrite v0.8.0's file." >&2
      exit 1
    fi
    local entry section key expected actual lost=0
    for entry in "${FIXTURE_SETTINGS[@]}"; do
      read -r section key expected <<<"$entry"
      actual=$(toml_value "$config" "$section" "$key")
      if [[ "$actual" != "$expected" ]]; then
        echo "$run: [$section] $key is ${actual:-missing} after the save, not v0.8.0's $expected." >&2
        lost=1
      fi
    done
    if [[ "$lost" -ne 0 ]]; then
      exit 1
    fi
  fi
  if ! grep -q "Assets directory found:" "$log"; then
    echo "$run: assets directory was not found (see the 'Assets directory not found' fallback warning above, if present); installed/extracted asset layout is broken." >&2
    exit 1
  fi
  if grep -q "Icon font not found" "$log"; then
    echo "$run: icon font failed to load even though an assets directory was found." >&2
    exit 1
  fi
  if ! grep -q "Loaded theme:" "$log"; then
    echo "$run: no themes loaded; theme directory is missing or empty." >&2
    exit 1
  fi
}

if [[ ! -s "$fixture" ]]; then
  echo "$label: the v0.8.0 config fixture $fixture is missing." >&2
  exit 1
fi
launch "$label (fresh config)" "$log_prefix-fresh.log"
launch "$label (v0.8.0's config)" "$log_prefix-v080.log" "$fixture"
echo "$label: assets (icon font + themes) resolved correctly; TaskSmack quit by itself after $ALIVE_SECONDS s with config.toml saved, from a fresh config and from v0.8.0's, whose settings it kept."
