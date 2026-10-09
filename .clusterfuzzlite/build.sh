#!/bin/bash -eu
# Builds every libFuzzer target under tests/fuzz/ into $OUT. ClusterFuzzLite runs every fuzzer
# binary it finds there, starting each from $OUT/<target>_seed_corpus.zip when there is one.
#
# Also runs locally, from the repo root, to reproduce CI (see CONTRIBUTING.md "Fuzzing").

OUT="${OUT:?OUT must name the output directory}"
mkdir -p "$OUT"

export FUZZ_DEPS_DIR="${FUZZ_DEPS_DIR:-${WORK:-$PWD/build}/fuzz-deps}"
.clusterfuzzlite/fetch-deps.sh

# ClusterFuzzLite supplies compiler and linker flags as argument lists.
# shellcheck disable=SC2086
build_fuzzer() {
    local name="$1"
    shift
    "$CXX" $CXXFLAGS \
        -std=c++23 \
        -Isrc \
        -isystem "$FUZZ_DEPS_DIR/tomlplusplus/include" \
        -isystem "$FUZZ_DEPS_DIR/spdlog/include" \
        -isystem "$FUZZ_DEPS_DIR/imgui" \
        -isystem "$FUZZ_DEPS_DIR/implot" \
        -DSPDLOG_USE_STD_FORMAT \
        "tests/fuzz/$name.cpp" "$@" \
        $LIB_FUZZING_ENGINE \
        -o "$OUT/$name"
}

# Zips the given seed files, flat, into the target's seed corpus.
seed_corpus() {
    local name="$1"
    shift
    rm -f "$OUT/${name}_seed_corpus.zip"
    zip -q -j "$OUT/${name}_seed_corpus.zip" "$@"
}

build_fuzzer fuzz_proc_parsing

# The SMBIOS table parser (#1513): header-only.
build_fuzzer fuzz_smbios

# The /proc/[pid]/maps module parser (#802): header-only, seeded with a real-looking maps file.
build_fuzzer fuzz_proc_maps
seed_corpus fuzz_proc_maps tests/fuzz/corpus/fuzz_proc_maps/*

# The /proc/[pid]/status security, attr/current and cgroup parsers (#1526): header-only, seeded with
# a real-looking status file, label and cgroup line.
build_fuzzer fuzz_proc_status_security
seed_corpus fuzz_proc_status_security tests/fuzz/corpus/fuzz_proc_status_security/*

# The Commit & paging parsers (#1516): header-only, seeded with real-looking /proc/meminfo and /proc/swaps.
build_fuzzer fuzz_commit_paging
seed_corpus fuzz_commit_paging tests/fuzz/corpus/fuzz_commit_paging/*

# The Storage parsers (#1517): header-only, seeded with a real-looking /proc/self/mountinfo and udev file.
build_fuzzer fuzz_mountinfo
seed_corpus fuzz_mountinfo tests/fuzz/corpus/fuzz_mountinfo/*

# The EDID parser (#1519): header-only, seeded with a real-looking 128-byte base block.
build_fuzzer fuzz_edid
seed_corpus fuzz_edid tests/fuzz/corpus/fuzz_edid/*

build_fuzzer fuzz_theme_loader src/UI/ThemeLoader.cpp
# Every built-in theme, hand-written seeds for the colour forms they don't use, and the minimised
# toml++ crash inputs (#1387, #1388, #1389) as regression seeds.
seed_corpus fuzz_theme_loader assets/themes/*.toml tests/fuzz/corpus/fuzz_theme_loader/* tests/fuzz/corpus/toml-regressions/*

# ThemeStub stands in for Theme.cpp (ImGui/ImPlot runtime), exactly as in the unit-test build, and
# ConfigDirOverrideStub for Core/ConfigDirOverride.cpp, which reads TASKSMACK_CONFIG_DIR through SDL.
build_fuzzer fuzz_user_config src/App/UserConfig.cpp tests/Mocks/ThemeStub.cpp tests/fuzz/ConfigDirOverrideStub.cpp
# config.toml as UserConfig::save() writes it (defaults, and every optional key set), plus legacy keys
# and out-of-range values, and the toml++ regression seeds.
seed_corpus fuzz_user_config tests/fuzz/corpus/fuzz_user_config/* tests/fuzz/corpus/toml-regressions/*
