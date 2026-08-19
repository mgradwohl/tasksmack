#!/bin/bash -eu

# ClusterFuzzLite supplies compiler and linker flags as argument lists.
# shellcheck disable=SC2086
"$CXX" $CXXFLAGS \
    -std=c++23 \
    -Isrc \
    tests/fuzz/fuzz_proc_parsing.cpp \
    $LIB_FUZZING_ENGINE \
    -o "$OUT/fuzz_proc_parsing"
