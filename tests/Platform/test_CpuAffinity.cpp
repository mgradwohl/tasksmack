/// @file test_CpuAffinity.cpp
/// @brief Tests for Platform::CpuAffinity, the variable-width process CPU affinity (#1247): parsing
/// a kernel CPU list (Linux Cpus_allowed_list) with processors at 64 and above, rejecting malformed
/// lists, the 64-bit mask conversion the Windows probe uses, and the ordering the Affinity column
/// sorts by.

#include "Platform/CpuAffinity.h"

#include <gtest/gtest.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{
namespace
{

/// The processors in `affinity`, ascending.
[[nodiscard]] std::vector<std::size_t> cpusOf(const CpuAffinity& affinity)
{
    std::vector<std::size_t> cpus;
    const std::size_t bits = affinity.words().size() * CpuAffinity::BITS_PER_WORD;
    for (std::size_t cpu = 0; cpu < bits; ++cpu)
    {
        if (affinity.test(cpu))
        {
            cpus.push_back(cpu);
        }
    }
    return cpus;
}

[[nodiscard]] CpuAffinity parsed(std::string_view list)
{
    const auto affinity = CpuAffinity::fromCpuList(list);
    EXPECT_TRUE(affinity.has_value()) << list;
    return affinity.value_or(CpuAffinity{});
}

// ========== Parsing Cpus_allowed_list ==========

TEST(CpuAffinityTest, ParsesASingleCpuAt64OrAbove)
{
    // `taskset -c 70 sleep 600`: the 64-bit mask dropped this process's only CPU.
    const auto affinity = parsed("70");
    EXPECT_EQ(cpusOf(affinity), (std::vector<std::size_t>{70}));
    EXPECT_EQ(affinity.count(), 1U);
    EXPECT_FALSE(affinity.empty());
    EXPECT_FALSE(affinity.test(6)); // Not folded into the low word
}

TEST(CpuAffinityTest, ParsesRangesAcrossWordBoundaries)
{
    const auto affinity = parsed("0-3,64-127");
    EXPECT_EQ(affinity.count(), 4U + 64U);
    EXPECT_TRUE(affinity.test(3));
    EXPECT_FALSE(affinity.test(4));
    EXPECT_FALSE(affinity.test(63));
    EXPECT_TRUE(affinity.test(64));
    EXPECT_TRUE(affinity.test(127));
    EXPECT_FALSE(affinity.test(128));
    ASSERT_EQ(affinity.words().size(), 2U);
    EXPECT_EQ(affinity.words()[0], 0xFULL);
    EXPECT_EQ(affinity.words()[1], ~std::uint64_t{0});

    EXPECT_EQ(parsed("60-70").count(), 11U);
    EXPECT_EQ(parsed("0-255").count(), 256U);
    EXPECT_EQ(cpusOf(parsed("1,5,130-131,8191")), (std::vector<std::size_t>{1, 5, 130, 131, 8191}));
}

TEST(CpuAffinityTest, ParsesTheLowWordAsTheOld64BitMaskDid)
{
    EXPECT_EQ(parsed("0-3"), CpuAffinity::fromMask(0xF));
    EXPECT_EQ(parsed("0-63"), CpuAffinity::fromMask(~std::uint64_t{0}));
    EXPECT_EQ(parsed("0,2,4-7"), CpuAffinity::fromMask(0xF5));
    ASSERT_EQ(parsed("63").words().size(), 1U); // Stays inline below processor 64
}

TEST(CpuAffinityTest, IgnoresSurroundingWhitespace)
{
    // The value after "Cpus_allowed_list:" starts with a tab; the line may still carry its newline.
    EXPECT_EQ(cpusOf(parsed("\t0-1,70\n")), (std::vector<std::size_t>{0, 1, 70}));
}

TEST(CpuAffinityTest, OverlappingAndUnorderedItemsUnion)
{
    EXPECT_EQ(parsed("70,0-3,2-5,70"), parsed("0-5,70"));
}

TEST(CpuAffinityTest, RejectsMalformedListsRatherThanReadingPart)
{
    for (const std::string_view bad : {
             "",
             " \t\n",
             "x",
             "0-",
             "-3",
             "5-3",                     // reversed range
             "0-3,",                    // trailing comma
             ",0-3",                    // leading comma
             "0,,3",                    // empty item
             "0-3,64-x",                // a valid prefix followed by junk
             "0-7:2",                   // stride (accepted by bitmap_parselist on input, never printed in status)
             "0 3",                     // space-separated
             "+1",                      // sign
             "0x10",                    // hex
             "65536",                   // at MAX_CPUS
             "0-65536",                 // range end at MAX_CPUS
             "99999999999999999999999", // overflows size_t
         })
    {
        EXPECT_FALSE(CpuAffinity::fromCpuList(bad).has_value()) << '"' << bad << '"';
    }
    EXPECT_TRUE(CpuAffinity::fromCpuList("65535").has_value()); // Just below the cap
}

// ========== The Windows probe's mask conversion ==========

TEST(CpuAffinityTest, FromMaskKeepsEveryBitAsItsProcessor)
{
    EXPECT_TRUE(CpuAffinity::fromMask(0).empty());
    EXPECT_EQ(cpusOf(CpuAffinity::fromMask(0x5)), (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(cpusOf(CpuAffinity::fromMask(0x8000000000000001ULL)), (std::vector<std::size_t>{0, 63}));
    EXPECT_EQ(CpuAffinity::fromMask(~std::uint64_t{0}).count(), 64U);
}

// ========== Building, equality and ordering ==========

TEST(CpuAffinityTest, SetSpillsPastProcessor63WithoutLosingTheLowWord)
{
    CpuAffinity affinity = CpuAffinity::fromMask(0x3);
    affinity.set(64);
    affinity.set(200);
    EXPECT_EQ(cpusOf(affinity), (std::vector<std::size_t>{0, 1, 64, 200}));
    EXPECT_EQ(affinity, parsed("0-1,64,200"));
    ASSERT_EQ(affinity.words().size(), 4U); // No trailing zero word
    EXPECT_NE(affinity.words().back(), 0U);
}

TEST(CpuAffinityTest, CopiesAreIndependentAndEqual)
{
    const CpuAffinity wide = parsed("0-3,64-127");
    CpuAffinity copy = wide;
    EXPECT_EQ(copy, wide);
    copy.set(300);
    EXPECT_NE(copy, wide);
    EXPECT_FALSE(wide.test(300));

    const CpuAffinity narrow = CpuAffinity::fromMask(0xF);
    const CpuAffinity narrowCopy = narrow; // NOLINT(performance-unnecessary-copy-initialization) - the copy is under test
    EXPECT_EQ(narrowCopy.words().size(), 1U);
    EXPECT_EQ(narrowCopy.words()[0], 0xFULL);
}

TEST(CpuAffinityTest, OrdersByNumericValueOfTheBitset)
{
    // Within 64 processors this is the old uint64_t mask order.
    EXPECT_LT(CpuAffinity::fromMask(0x1), CpuAffinity::fromMask(0x2));
    EXPECT_LT(CpuAffinity::fromMask(0x7), CpuAffinity::fromMask(0x8));
    // An unread (empty) affinity sorts below every reading.
    EXPECT_LT(CpuAffinity{}, CpuAffinity::fromMask(0x1));
    // A processor at 64 or above outranks any set of processors 0-63.
    EXPECT_LT(CpuAffinity::fromMask(~std::uint64_t{0}), parsed("64"));
    EXPECT_LT(parsed("64"), parsed("70"));
    EXPECT_LT(parsed("70"), parsed("0-3,64-127"));
    EXPECT_LT(parsed("0-3,64-127"), parsed("200"));
    EXPECT_LT(parsed("1,64"), parsed("2,64"));
    EXPECT_EQ(parsed("64-127") <=> parsed("64-127"), std::strong_ordering::equal);
}

} // namespace
} // namespace Platform
