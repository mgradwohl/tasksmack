/// @file test_WindowsProcessSnapshotSize.cpp
/// @brief The first buffer a SystemProcessInformation snapshot is queried into, and how it grows when the
/// snapshot doesn't fit (#4277): a desktop's snapshot fits the first buffer, and a short buffer grows to a
/// quarter more than the kernel asked for, geometrically when it keeps falling short. Built on every
/// platform (#1133).

#include "Platform/Windows/WindowsProcessSnapshotSize.h"

#include <gtest/gtest.h>

#include <cstddef>

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
    // The kernel asked for 1.5 MiB of a 1 MiB buffer: a quarter more, past doubling the buffer
    EXPECT_EQ(grownProcessSnapshotBytes(1024 * KIB, 1800 * KIB), 2250 * KIB);
    EXPECT_GE(grownProcessSnapshotBytes(1024 * KIB, 1800 * KIB), 1800 * KIB);
}

TEST(WindowsProcessSnapshotSizeTest, ABufferThatKeepsFallingShortAtLeastDoubles)
{
    // A ReturnLength just past the buffer, or none at all, still doubles it, so the retries stay few.
    EXPECT_EQ(grownProcessSnapshotBytes(1024 * KIB, (1024 * KIB) + 1), 2048 * KIB);
    EXPECT_EQ(grownProcessSnapshotBytes(1024 * KIB, 0), 2048 * KIB);

    std::size_t bytes = PROCESS_SNAPSHOT_INITIAL_BYTES;
    int retries = 0;
    constexpr std::size_t BIG = 40 * 1024 * KIB; // A server's 40 MiB snapshot, the kernel saying only "too small"
    while (bytes < BIG)
    {
        bytes = grownProcessSnapshotBytes(bytes, 0);
        ++retries;
    }
    EXPECT_LE(retries, 6);
}

} // namespace
} // namespace Platform::Windows
