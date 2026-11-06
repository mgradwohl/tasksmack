// Fixture for tools/test-clang-tidy-headers.ps1 (#3500): clean itself, so any finding reported
// for this translation unit comes from HeaderFinding.h.

#include "HeaderFinding.h"

// The test's compile databases define this; without it the command was split wrongly.
#ifndef TIDY_FIXTURE_DEFINE
#error "TIDY_FIXTURE_DEFINE is not defined: the compile command was not parsed as written"
#endif

namespace TidyFixture
{

int useMixedArithmetic()
{
    return mixedArithmetic(1, 2, 3);
}

} // namespace TidyFixture
