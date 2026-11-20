/// @file test_LibcxxHardening.cpp
/// @brief The tests run with the standard-library checks TaskSmack ships with: libc++'s _LIBCPP_HARDENING_MODE_FAST
/// on Linux (#3900) and the MSVC STL's _MSVC_STL_HARDENING on Windows (#4341), as tasksmack_apply_stl_hardening()
/// (cmake/CompilerOptions.cmake) gives the app and TaskSmackTests alike. So an out-of-bounds operator[] traps in a
/// test before it can trap in the shipped TaskSmack, which runs as root on Linux and elevated on Windows.

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

#elif defined(_WIN32) && defined(_MSVC_STL_VERSION)

#include <optional>
#include <span>
#include <string_view>

namespace
{

TEST(MsvcStlHardeningTest, TheTestsHaveTheAppsHardeningMode)
{
    // The define the app's objects (TaskSmackApp) and the elevated images get (#4341): without it the shipped,
    // non-Debug build has _ITERATOR_DEBUG_LEVEL 0 and no bounds checks at all.
    EXPECT_EQ(_MSVC_STL_HARDENING, 1);
}

// Only where iterator debugging is off, the configuration that ships: in Debug (_ITERATOR_DEBUG_LEVEL 2) the
// same out-of-bounds read reaches the CRT's debug report first, which would open an assertion dialog, and tests
// must never pop up UI. CI's Windows release build runs these.
#if _ITERATOR_DEBUG_LEVEL == 0

TEST(MsvcStlHardeningDeathTest, AnIndexPastTheEndOfAVectorTraps)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::vector<int> values(2, 0);
    // Volatile, so the compiler can't see the index is out of range and refuse to build, or drop the read.
    volatile std::size_t past = values.size();
    EXPECT_DEATH_IF_SUPPORTED(static_cast<void>(values[past]), "");
}

TEST(MsvcStlHardeningDeathTest, AnIndexPastTheEndOfASpanOrStringViewTraps)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::vector<int> values(2, 0);
    const std::span<const int> span(values);
    const std::string_view text = "ab";
    volatile std::size_t past = 2;
    EXPECT_DEATH_IF_SUPPORTED(static_cast<void>(span[past]), "");
    EXPECT_DEATH_IF_SUPPORTED(static_cast<void>(text[past]), "");
}

TEST(MsvcStlHardeningDeathTest, DereferencingAnEmptyOptionalTraps)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::optional<int> empty;
    volatile bool use = true;
    EXPECT_DEATH_IF_SUPPORTED(
        {
            if (use)
            {
                static_cast<void>(*empty);
            }
        },
        "");
}

#endif // _ITERATOR_DEBUG_LEVEL == 0

} // namespace

#endif
