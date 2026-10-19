/// @file test_WindowsConversions.cpp
/// @brief WindowsConversions.h (#2563): a FILETIME as 100 ns ticks (zero, the high word, all bits set),
/// those ticks as Unix seconds (around the 1601-to-1970 epoch offset, and the largest count), and a GUID's
/// registry text. Fakes with FILETIME's and GUID's fields stand in for the Windows types, so these run on
/// every platform (#1133).

#include "Platform/Windows/WindowsConversions.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace Platform::Windows
{
namespace
{

/// FILETIME's fields, as <minwinbase.h> declares them.
struct FakeFileTime
{
    std::uint32_t dwLowDateTime = 0;
    std::uint32_t dwHighDateTime = 0;
};

/// GUID's fields, as <guiddef.h> declares them.
struct FakeGuid
{
    std::uint32_t Data1 = 0;
    std::uint16_t Data2 = 0;
    std::uint16_t Data3 = 0;
    std::array<std::uint8_t, 8> Data4{};
};

constexpr std::uint32_t ALL_BITS = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t MAX_TICKS = std::numeric_limits<std::uint64_t>::max();

TEST(WindowsConversionsTest, FileTimeTicksJoinsTheTwoWords)
{
    EXPECT_EQ(fileTimeTicks(FakeFileTime{}), 0U);
    EXPECT_EQ(fileTimeTicks(FakeFileTime{.dwLowDateTime = ALL_BITS, .dwHighDateTime = 0}), 0xFFFF'FFFFULL);
    // The high word lands in the top 32 bits, not added to or overlapping the low one.
    EXPECT_EQ(fileTimeTicks(FakeFileTime{.dwLowDateTime = 0, .dwHighDateTime = 1}), 0x1'0000'0000ULL);
    EXPECT_EQ(fileTimeTicks(FakeFileTime{.dwLowDateTime = 0x89AB'CDEF, .dwHighDateTime = 0x0123'4567}), 0x0123'4567'89AB'CDEFULL);
    EXPECT_EQ(fileTimeTicks(FakeFileTime{.dwLowDateTime = ALL_BITS, .dwHighDateTime = ALL_BITS}), MAX_TICKS);
    static_assert(fileTimeTicks(FakeFileTime{.dwLowDateTime = 2, .dwHighDateTime = 1}) == 0x1'0000'0002ULL);
}

TEST(WindowsConversionsTest, FileTimeToUnixSecondsAroundTheEpoch)
{
    // 1601-01-01 to 1970-01-01 is 11644473600 seconds, a whole number of them.
    EXPECT_EQ(FILETIME_UNIX_EPOCH, 11'644'473'600ULL * FILETIME_TICKS_PER_SECOND);

    EXPECT_EQ(fileTimeToUnixSeconds(0), 0U);
    EXPECT_EQ(fileTimeToUnixSeconds(FILETIME_UNIX_EPOCH - 1), 0U) << "before the Unix epoch";
    EXPECT_EQ(fileTimeToUnixSeconds(FILETIME_UNIX_EPOCH), 0U);
    EXPECT_EQ(fileTimeToUnixSeconds(FILETIME_UNIX_EPOCH + FILETIME_TICKS_PER_SECOND - 1), 0U) << "rounds down";
    EXPECT_EQ(fileTimeToUnixSeconds(FILETIME_UNIX_EPOCH + FILETIME_TICKS_PER_SECOND), 1U);

    // 2024-01-02T03:04:05Z.
    constexpr std::uint64_t UNIX_SECONDS = 1704164645;
    EXPECT_EQ(fileTimeToUnixSeconds(FILETIME_UNIX_EPOCH + (UNIX_SECONDS * FILETIME_TICKS_PER_SECOND)), UNIX_SECONDS);
    EXPECT_EQ(fileTimeToUnixSeconds(FILETIME_UNIX_EPOCH + (UNIX_SECONDS * FILETIME_TICKS_PER_SECOND) + 9'999'999), UNIX_SECONDS);

    // The largest tick count, with no overflow.
    EXPECT_EQ(fileTimeToUnixSeconds(MAX_TICKS), (MAX_TICKS - FILETIME_UNIX_EPOCH) / FILETIME_TICKS_PER_SECOND);
    EXPECT_EQ(fileTimeToUnixSeconds(fileTimeTicks(FakeFileTime{.dwLowDateTime = ALL_BITS, .dwHighDateTime = ALL_BITS})),
              1'833'029'933'770ULL);
}

TEST(WindowsConversionsTest, GuidTextIsTheRegistryForm)
{
    EXPECT_EQ(guidText(FakeGuid{}), "{00000000-0000-0000-0000-000000000000}");
    EXPECT_EQ(guidText(FakeGuid{
                  .Data1 = 0x4D36'E972, .Data2 = 0xE325, .Data3 = 0x11CE, .Data4 = {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}}),
              "{4D36E972-E325-11CE-BFC1-08002BE10318}")
        << "upper-case and zero-padded";
    EXPECT_EQ(
        guidText(FakeGuid{.Data1 = ALL_BITS, .Data2 = 0xFFFF, .Data3 = 0xFFFF, .Data4 = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}),
        "{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}");
}

} // namespace
} // namespace Platform::Windows
