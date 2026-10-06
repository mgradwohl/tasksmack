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

build_fuzzer fuzz_theme_loader src/UI/ThemeLoader.cpp
# Every built-in theme, plus hand-written seeds for the colour forms they don't use.
seed_corpus fuzz_theme_loader assets/themes/*.toml tests/fuzz/corpus/fuzz_theme_loader/*

# ThemeStub stands in for Theme.cpp (ImGui/ImPlot runtime), exactly as in the unit-test build.
build_fuzzer fuzz_user_config src/App/UserConfig.cpp tests/Mocks/ThemeStub.cpp
# config.toml as UserConfig::save() writes it (defaults, and every optional key set), plus legacy keys
# and out-of-range values.
seed_corpus fuzz_user_config tests/fuzz/corpus/fuzz_user_config/*
