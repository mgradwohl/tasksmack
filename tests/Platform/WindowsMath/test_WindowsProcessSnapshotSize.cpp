/// @file test_WindowsProcessSnapshotSize.cpp
/// @brief The first buffer a SystemProcessInformation snapshot is queried into, and how it grows when the
/// snapshot doesn't fit (#4277): a desktop's snapshot fits the first buffer, and a short buffer grows to a
/// quarter more than the kernel asked for, geometrically when it keeps falling short, and never past the 64 MiB
/// ceiling. Built on every platform (#1133).

#include "Platform/Windows/WindowsProcessSnapshotSize.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>

namespace Platform::Windows
{
namespace
{

constexpr std::size_t KIB = 1024;

TEST(WindowsProcessSnapshotSizeTest, TheFirstBufferFitsADesktopSnapshotWithRoomToGrow)
{
    // #4277's desktop: 344 processes, 618 KiB; the old 512 KiB start was smaller than that.
    constexpr std::size_t DESKTOP = 618 * KIB;
    EXPECT_GT(PROCESS_SNAPSHOT_INITIAL_BYTES, DESKTOP + (DESKTOP / 2));
}

TEST(WindowsProcessSnapshotSizeTest, AShortBufferGrowsToAQuarterMoreThanTheKernelAsked)
{
    // The kernel asked for 1.8 MiB of a 1 MiB buffer: a quarter more, past doubling the buffer
    EXPECT_EQ(grownProcessSnapshotBytes(1024 * KIB, 1800 * KIB).value_or(0), 2250 * KIB);
}

TEST(WindowsProcessSnapshotSizeTest, ABufferThatKeepsFallingShortAtLeastDoubles)
{
    // A ReturnLength just past the buffer, or none at all, still doubles it, so the retries stay few.
    EXPECT_EQ(grownProcessSnapshotBytes(1024 * KIB, (1024 * KIB) + 1).value_or(0), 2048 * KIB);
    EXPECT_EQ(grownProcessSnapshotBytes(1024 * KIB, 0).value_or(0), 2048 * KIB);

    std::size_t bytes = PROCESS_SNAPSHOT_INITIAL_BYTES;
    int retries = 0;
    constexpr std::size_t BIG = std::size_t{40} * 1024 * KIB; // A server's 40 MiB snapshot, the kernel saying only "too small"
    while (bytes < BIG)
    {
        const std::optional<std::size_t> grown = grownProcessSnapshotBytes(bytes, 0);
        ASSERT_TRUE(grown.has_value());
        bytes = grown.value_or(0);
        ++retries;
    }
    EXPECT_LE(retries, 6);
}

// #4277 review: growth stops at the ceiling. A buffer short of it grows to it at most; one already there, or a
// kernel asking for more than it, gets no bigger buffer, and the read fails instead.
TEST(WindowsProcessSnapshotSizeTest, GrowthStopsAtTheCeiling)
{
    EXPECT_EQ(PROCESS_SNAPSHOT_MAX_BYTES, std::size_t{64} * 1024 * KIB);
    EXPECT_EQ(grownProcessSnapshotBytes(PROCESS_SNAPSHOT_MAX_BYTES / 2, 0).value_or(0), PROCESS_SNAPSHOT_MAX_BYTES);
    // 60 MiB needed: a quarter more would pass the ceiling, so the ceiling it is, since the snapshot fits there
    EXPECT_EQ(grownProcessSnapshotBytes(PROCESS_SNAPSHOT_MAX_BYTES / 2, std::size_t{60} * 1024 * KIB).value_or(0),
              PROCESS_SNAPSHOT_MAX_BYTES);
    EXPECT_FALSE(grownProcessSnapshotBytes(PROCESS_SNAPSHOT_MAX_BYTES, 0).has_value());
    EXPECT_FALSE(grownProcessSnapshotBytes(1024 * KIB, PROCESS_SNAPSHOT_MAX_BYTES + 1).has_value());
    EXPECT_FALSE(grownProcessSnapshotBytes(1024 * KIB, std::size_t{0xFFFFFFFF}).has_value()) << "a ULONG ReturnLength's most";

    // However often the kernel says "too small", the buffer never passes the ceiling.
    std::size_t bytes = PROCESS_SNAPSHOT_INITIAL_BYTES;
    for (int retry = 0; retry < 100; ++retry)
    {
        const std::optional<std::size_t> grown = grownProcessSnapshotBytes(bytes, bytes + 1);
        if (!grown.has_value())
        {
            break;
        }
        bytes = *grown;
        EXPECT_LE(bytes, PROCESS_SNAPSHOT_MAX_BYTES);
    }
    EXPECT_EQ(bytes, PROCESS_SNAPSHOT_MAX_BYTES);
}

} // namespace
} // namespace Platform::Windows
