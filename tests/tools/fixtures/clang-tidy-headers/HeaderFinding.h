#pragma once

// Fixture for tools/test-clang-tidy-headers.ps1 (#3500): a project header with exactly one
// clang-tidy finding, which the local clang-tidy scripts must report when they analyze
// HeaderFinding.cpp, as CI's Windows clang-tidy does. Not compiled into any target, and not under
// src/, so CI's own clang-tidy runs never analyze it. Keep the finding: the test fails without it.

namespace TidyFixture
{

[[nodiscard]] inline int mixedArithmetic(int a, int b, int c)
{
    return a + b * c; // readability-math-missing-parentheses
}

[[nodiscard]] int useMixedArithmetic();

} // namespace TidyFixture
