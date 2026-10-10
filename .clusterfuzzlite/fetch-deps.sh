#!/bin/bash -eu
# Fetches the header-only dependencies the fuzz targets compile against, at the commits
# cmake/Dependencies.cmake pins (so a Renovate bump there moves the fuzz build with it), into
# $FUZZ_DEPS_DIR/<name>, with the same cmake/patches/ the CMake build applies. A dependency
# already checked out at its pin isn't fetched again, but still gets its patches.
# Run from the repo root. The Dockerfile runs it at image build; build.sh runs it again, which is
# a no-op there and fetches on a local run.

FUZZ_DEPS_DIR="${FUZZ_DEPS_DIR:?FUZZ_DEPS_DIR must name the dependency directory}"
mkdir -p "$FUZZ_DEPS_DIR"

pinned_sha() {
    # The GIT_TAG on the line after the dependency's GIT_REPOSITORY in cmake/Dependencies.cmake.
    local sha
    sha="$(grep -A1 "GIT_REPOSITORY $1\$" cmake/Dependencies.cmake | grep -oE 'GIT_TAG +[0-9a-f]{40}' | grep -oE '[0-9a-f]{40}' || true)"
    if [[ -z "$sha" ]]; then
        echo "error: no pinned GIT_TAG for $1 in cmake/Dependencies.cmake" >&2
        return 1
    fi
    echo "$sha"
}

# Applies each patch unless it is already in place (it reverses cleanly), as
# cmake/patches/ApplyPatch.cmake does for the CMake build. A patch that neither reverses nor
# applies gets one retry on a pristine checkout (an older version of it may be applied); if it
# still doesn't apply (the pin moved under it), the build fails rather than fuzzing an unpatched
# dependency. The reset drops any other local change, so a dependency gets at most one patch.
apply_patches() {
    local dir="$1" p
    shift
    for p in "$@"; do
        git -C "$dir" apply --check --reverse --whitespace=nowarn "$p" 2>/dev/null && continue
        git -C "$dir" apply --whitespace=nowarn "$p" 2>/dev/null && continue
        # Neither: most likely an older version of the patch is applied (a reused $FUZZ_DEPS_DIR).
        # Reset the dependency's own checkout to pristine and try once more, as ApplyPatch.cmake does.
        echo "fetch-deps: resetting $(basename "$dir") to pristine before applying $(basename "$p")" >&2
        git -C "$dir" checkout -q -- .
        git -C "$dir" clean -fdq
        git -C "$dir" apply --whitespace=nowarn "$p"
    done
}

# fetch_dep <name> <url> [absolute patch path...]
fetch_dep() {
    local name="$1" url="$2" sha dir
    shift 2
    sha="$(pinned_sha "$url")"
    dir="$FUZZ_DEPS_DIR/$name"
    # Patches don't move HEAD, so a tree already at the pin may still be unpatched: patch either way.
    if [[ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" != "$sha" ]]; then
        rm -rf "$dir"
        git init -q "$dir"
        git -C "$dir" fetch -q --depth 1 "$url" "$sha"
        git -C "$dir" checkout -q FETCH_HEAD
    fi
    apply_patches "$dir" "$@"
}

PATCHES_DIR="$PWD/cmake/patches"

fetch_dep tomlplusplus https://github.com/marzer/tomlplusplus.git "$PATCHES_DIR/tomlplusplus-parse-value-assertions.patch"
fetch_dep spdlog https://github.com/gabime/spdlog.git
fetch_dep imgui https://github.com/ocornut/imgui.git
fetch_dep implot https://github.com/epezent/implot.git
