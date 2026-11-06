// Fixture for tools/test-clang-tidy-headers.ps1 (#3500): clean itself, so any finding reported
// for this translation unit comes from HeaderFinding.h.

#include "HeaderFinding.h"

namespace TidyFixture
{

int useMixedArithmetic()
{
    return mixedArithmetic(1, 2, 3);
}

} // namespace TidyFixture
