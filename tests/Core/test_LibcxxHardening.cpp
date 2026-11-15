/// @file test_LibcxxHardening.cpp
/// @brief The tests run with the libc++ checks TaskSmack ships with (#3900): _LIBCPP_HARDENING_MODE_FAST, as
/// tasksmack_apply_libcxx_hardening() (cmake/CompilerOptions.cmake) gives the app and TaskSmackTests alike, so an
/// out-of-bounds operator[] traps in a test before it can trap in the shipped, root-running TaskSmack.

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#if defined(__linux__) && defined(_LIBCPP_VERSION)

namespace
{

TEST(LibcxxHardeningTest, TheTestsHaveTheAppsHardeningMode)
{
    EXPECT_EQ(_LIBCPP_HARDENING_MODE, _LIBCPP_HARDENING_MODE_FAST);
}

TEST(LibcxxHardeningDeathTest, AnIndexPastTheEndTraps)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::vector<int> values(2, 0);
    // Volatile, so the compiler can't see the index is out of range and refuse to build, or drop the read.
    volatile std::size_t past = values.size();
    EXPECT_DEATH_IF_SUPPORTED(static_cast<void>(values[past]), "");
}

} // namespace

#endif
