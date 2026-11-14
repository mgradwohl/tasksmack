/// @file test_ValueStripText.cpp
/// @brief The value-strip and NowBar texts the Memory, GPU and Process Details charts build every
/// frame (#843): the same text as before, held in place (UI::InlineText) or short enough for the
/// small-string buffer, so building them allocates nothing.

#include "App/Panels/MemorySection.h"
#include "App/Panels/ProcessDetailsChartHelpers.h"
#include "Domain/AllocationFailureHook.h"
#include "Domain/SystemSnapshot.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/InlineText.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <type_traits>

namespace App
{
namespace
{

constexpr std::uint64_t GIB = 1ULL << 30U;
constexpr std::uint64_t MIB = 1ULL << 20U;
/// "118.6 GiB": the RAM in use on the machine #843 measured, with its total TOTAL_BYTES.
constexpr std::uint64_t USED_BYTES = (118 * GIB) + (614 * MIB);
constexpr std::uint64_t TOTAL_BYTES = 184 * GIB;

// A strip entry's value is held in place: a std::string longer than its small-string buffer cost a
// heap allocation per entry per frame.
static_assert(std::is_same_v<decltype(UI::Widgets::ValueStripEntry::value), UI::InlineText>);
static_assert(std::is_same_v<decltype(UI::Format::bytesUsedTotalPercentCompactText(0, 0, 0.0)), UI::InlineText>);
static_assert(std::is_same_v<decltype(Detail::formatBytesWithRamShareText(0.0, 0.0)), UI::InlineText>);
static_assert(std::is_same_v<decltype(MemorySection::commitStripValue(Domain::SystemSnapshot{})), UI::InlineText>);
static_assert(std::is_same_v<decltype(MemorySection::compressedStripValue(Domain::SystemSnapshot{})), UI::InlineText>);

/// A machine with every strip entry the Memory chart can show: commit charge and zram.
[[nodiscard]] Domain::SystemSnapshot busyMachine()
{
    Domain::SystemSnapshot snap;
    snap.memoryTotalBytes = 184 * GIB;
    snap.memoryUsedBytes = 118 * GIB;
    snap.hasCommitCharge = true;
    snap.commitChargeBytes = 135 * GIB;
    snap.commitLimitBytes = 211 * GIB;
    snap.hasZram = true;
    snap.zramOriginalBytes = 3 * GIB;
    snap.zramCompressedBytes = 1 * GIB;
    snap.compressionRatio = Domain::compressionRatio(snap);
    return snap;
}

TEST(ValueStripTextTest, TheInlineTextsReadAsTheStringsTheyReplaced)
{
    // The longest of them, longer than any small-string buffer.
    const UI::InlineText ram = UI::Format::bytesUsedTotalPercentCompactText(USED_BYTES, TOTAL_BYTES, 64.0);
    EXPECT_EQ(ram.view(),
              std::format("{} / {} ({})",
                          UI::Format::formatBytesWithUnit(static_cast<double>(USED_BYTES), UI::Format::unitForTotalBytes(TOTAL_BYTES)),
                          UI::Format::formatBytesWithUnit(static_cast<double>(TOTAL_BYTES), UI::Format::unitForTotalBytes(TOTAL_BYTES)),
                          UI::Format::formatPercent(64.0)));
    EXPECT_GT(ram.size(), 15U); // past libstdc++'s and MSVC's small-string buffers
    EXPECT_EQ(UI::Format::bytesUsedTotalPercentCompact(3 * GIB, 16 * GIB, 20.0),
              std::string(UI::Format::bytesUsedTotalPercentCompactText(3 * GIB, 16 * GIB, 20.0).view()));

    EXPECT_EQ(Detail::formatBytesWithRamShareText(412.0e6, 1.0e-8).view(),
              std::format("{} ({} of RAM)", UI::Format::formatBytes(412.0e6), UI::Format::percentOneDecimal(4.12)));
    EXPECT_EQ(Detail::formatBytesWithRamShare(412.0e6, 1.0e-8), std::string(Detail::formatBytesWithRamShareText(412.0e6, 1.0e-8).view()));
    EXPECT_EQ(Detail::formatBytesWithRamShareText(std::numeric_limits<double>::quiet_NaN(), 1.0e-8).view(), "N/A");

    const Domain::SystemSnapshot snap = busyMachine();
    const double commitPercent = 100.0 * (135.0 / 211.0);
    EXPECT_EQ(MemorySection::commitStripValue(snap).view(), UI::Format::bytesUsedTotalPercentCompact(135 * GIB, 211 * GIB, commitPercent));
    EXPECT_EQ(MemorySection::compressedStripValue(snap).view(),
              "3.0× (" + UI::Format::formatBytes(static_cast<double>(3 * GIB)) + " in " +
                  UI::Format::formatBytes(static_cast<double>(1 * GIB)) + ")");

    // The GPU strip's clock and temperature, now built in a local buffer: unchanged.
    EXPECT_EQ(UI::Format::formatCelsius(61.4), "61°C");
    EXPECT_EQ(UI::Format::formatCelsius(-0.2), "0°C");
    EXPECT_EQ(UI::Format::formatMegahertz(1849.5), "1850 MHz");
    EXPECT_EQ(UI::Format::formatMegahertz(std::numeric_limits<double>::quiet_NaN()), "N/A");
}

// The improvement itself, where the test binary counts allocations (Linux debug and release; the
// Windows C runtime build compiles the allocator hook out, see AllocationFailureHook.h).
TEST(ValueStripTextTest, BuildingTheStripTextsAllocatesNothing)
{
#ifdef TASKSMACK_NO_ALLOCATOR_HOOK
    GTEST_SKIP() << "no allocation count here: " << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    const Domain::SystemSnapshot snap = busyMachine();
    const auto buildAll = [&snap]
    {
        // What one frame of the Memory, GPU and Process Details charts builds for their strips.
        const UI::Widgets::ValueStripEntry commit{.label = "Commit", .value = MemorySection::commitStripValue(snap), .color = {}};
        const UI::Widgets::ValueStripEntry compressed{
            .label = "Compressed", .value = MemorySection::compressedStripValue(snap), .color = {}};
        const UI::InlineText used = UI::Widgets::tooltipRowText(
            "Used", UI::Format::bytesUsedTotalPercentCompactText(snap.memoryUsedBytes, snap.memoryTotalBytes, 64.0));
        const UI::InlineText processUsed =
            UI::InlineText::format("{}: {}", "Memory", Detail::formatBytesWithRamShareText(412.0e6, 1.0e-8).view());
        const std::string clock = UI::Format::formatMegahertz(1850.0);
        const std::string temperature = UI::Format::formatCelsius(61.0);
        return commit.value.size() + compressed.value.size() + used.size() + processUsed.size() + clock.size() + temperature.size();
    };
    EXPECT_GT(buildAll(), 0U); // warm-up: the locale's punctuation is looked up once
    const std::uint64_t before = TestSupport::allocationsOnThisThread();
    EXPECT_GT(buildAll(), 0U);
    EXPECT_EQ(TestSupport::allocationsOnThisThread() - before, 0U);
#endif
}

} // namespace
} // namespace App
