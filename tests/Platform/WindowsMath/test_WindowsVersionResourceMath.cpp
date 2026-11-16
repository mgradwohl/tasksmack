/// @file test_WindowsVersionResourceMath.cpp
/// @brief The version resource walk the Publisher column reads a held file with (#4057):
/// VersionResourceMath::findVersionResource() on PE files built byte by byte (tests/Support/PeImageBuilder.h), and on
/// truncated, hostile and randomly damaged ones, where it must answer none or a span inside the file, never read
/// outside it or loop. Built on every platform, so Linux's sanitizers run it too.

#include "Platform/Windows/WindowsVersionResourceMath.h"
#include "Support/PeImageBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using Platform::VersionResourceMath::findVersionResource;
using TestSupport::buildPeImage;
using TestSupport::PeImage;
using TestSupport::put16;
using TestSupport::put32;
using TestSupport::versionInfoBlock;

[[nodiscard]] std::optional<std::span<const std::byte>> find(const std::vector<std::byte>& bytes)
{
    return findVersionResource(std::span<const std::byte>(bytes));
}

/// Whether @p found is exactly the block built at @p at in @p image.
[[nodiscard]] bool isBlockAt(const std::optional<std::span<const std::byte>>& found, const PeImage& image, std::size_t at, std::size_t size)
{
    return found.has_value() && found->data() == image.bytes.data() + at && found->size() == size;
}

TEST(VersionResourceMathTest, AVersionBlockIsFoundInAPe32PlusAndAPe32File)
{
    const std::vector<std::byte> block = versionInfoBlock(u"TaskSmack Test Co");
    const PeImage plus = buildPeImage({{0x0409U, block}});
    EXPECT_TRUE(isBlockAt(find(plus.bytes), plus, plus.blocks[0], block.size()));

    const PeImage pe32 = buildPeImage({{0x0409U, block}}, /*pe32=*/true);
    EXPECT_TRUE(isBlockAt(find(pe32.bytes), pe32, pe32.blocks[0], block.size()));

    const PeImage iconFirst = buildPeImage({{0x0409U, block}}, false, /*iconFirst=*/true);
    EXPECT_TRUE(isBlockAt(find(iconFirst.bytes), iconFirst, iconFirst.blocks[0], block.size())) << "other types are skipped";
}

// The block's own length (wLength) bounds it, not the data entry's size, which may be padded.
TEST(VersionResourceMathTest, TheBlockIsItsOwnLengthInsideALargerDataEntry)
{
    std::vector<std::byte> block = versionInfoBlock(u"Padded");
    const std::size_t length = block.size();
    block.resize(length + 64);
    const PeImage image = buildPeImage({{0x0409U, block}});
    EXPECT_TRUE(isBlockAt(find(image.bytes), image, image.blocks[0], length));
}

// The neutral language, else US English, else the first listed (not FindResource's UI-language order).
TEST(VersionResourceMathTest, TheNeutralLanguageIsTakenThenUsEnglishThenTheFirst)
{
    const std::vector<std::byte> german = versionInfoBlock(u"Deutsch");
    const std::vector<std::byte> neutral = versionInfoBlock(u"Neutral");
    const std::vector<std::byte> english = versionInfoBlock(u"English");
    const PeImage withNeutral = buildPeImage({{0x0407U, german}, {0x0409U, english}, {0x0000U, neutral}});
    EXPECT_TRUE(isBlockAt(find(withNeutral.bytes), withNeutral, withNeutral.blocks[2], neutral.size()));

    const PeImage withEnglish = buildPeImage({{0x0407U, german}, {0x0409U, english}});
    EXPECT_TRUE(isBlockAt(find(withEnglish.bytes), withEnglish, withEnglish.blocks[1], english.size()));

    const std::vector<std::byte> french = versionInfoBlock(u"Francais");
    const PeImage neither = buildPeImage({{0x0407U, german}, {0x040CU, french}});
    EXPECT_TRUE(isBlockAt(find(neither.bytes), neither, neither.blocks[0], german.size()));
}

TEST(VersionResourceMathTest, NotAPeFileOrNoVersionResourceIsNone)
{
    EXPECT_FALSE(find({}).has_value());
    EXPECT_FALSE(find(std::vector<std::byte>(4096)).has_value()) << "all zeros";

    PeImage image = buildPeImage(u"Co");
    put16(image.bytes, 0, 0x4D5A); // "ZM"
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, 0x40, 0x00004551); // not "PE\0\0"
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put16(image.bytes, image.optionalHeader, 0x0107); // a ROM image's magic
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.resourceSlot, 0); // no resource directory
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.typeEntry, 14); // RT_GROUP_ICON, not RT_VERSION
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.nameEntry, 2); // not VS_VERSION_INFO
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(std::vector<std::pair<std::uint32_t, std::vector<std::byte>>>{});
    EXPECT_FALSE(find(image.bytes).has_value()) << "a language directory with no entries";
}

// Every prefix of a valid file stops short of its block (the block is last), so each is none: every read is checked
// against the end, wherever the file is cut.
TEST(VersionResourceMathTest, EveryTruncationIsNone)
{
    const PeImage image = buildPeImage(u"TaskSmack Test Co");
    ASSERT_TRUE(find(image.bytes).has_value());
    for (std::size_t length = 0; length < image.bytes.size(); ++length)
    {
        const std::span<const std::byte> cut(image.bytes.data(), length);
        EXPECT_FALSE(findVersionResource(cut).has_value()) << "cut at " << length;
    }
}

TEST(VersionResourceMathTest, HeadersThatPointOutsideTheFileAreNone)
{
    PeImage image = buildPeImage(u"Co");
    put32(image.bytes, 0x3C, 0xFFFFFFFFU); // e_lfanew past the end
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, 0x3C, static_cast<std::uint32_t>(image.bytes.size() - 2)); // the signature runs off the end
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.resourceSlot - 20, 2); // NumberOfRvaAndSizes stops before the resource slot
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put16(image.bytes, 0x44 + 16, 100); // SizeOfOptionalHeader too small to hold the resource slot
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put16(image.bytes, 0x44 + 2, 0xFFFF); // 65535 sections: the table runs off the end
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put16(image.bytes, 0x44 + 2, 0); // no sections: no RVA is anywhere in the file
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.resourceSlot, 0x7FFFF000U); // an RVA no section holds
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.section + 20, 0xFFFFFF00U); // PointerToRawData past the end, near 2^32
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.section + 16, 0xFFFFFFFFU); // SizeOfRawData far past the end: bounded by the file
    EXPECT_TRUE(find(image.bytes).has_value()) << "the file still holds every byte the walk reads";
}

TEST(VersionResourceMathTest, HostileResourceDirectoriesAreNone)
{
    PeImage image = buildPeImage(u"Co");
    put16(image.bytes, image.resources + 12, 0xFFFF); // 65535 named and 65535 ID entries in a few hundred bytes
    put16(image.bytes, image.resources + 14, 0xFFFF);
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.typeEntry + 4, 0x18); // RT_VERSION's entry is data, not a subdirectory
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.typeEntry + 4, 0xFFFFFFFFU); // a subdirectory at 0x7FFFFFFF, far outside the section
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.typeEntry + 4, 0x80000000U); // RT_VERSION's names are the root itself: a cycle
    EXPECT_FALSE(find(image.bytes).has_value()) << "the root has no VS_VERSION_INFO entry, and the walk doesn't loop";

    image = buildPeImage(u"Co");
    put32(image.bytes, image.nameEntry + 4, 0x80000000U | (image.nameDirectory - image.resources)); // names -> names
    EXPECT_FALSE(find(image.bytes).has_value()) << "a language level that points back at the name level";

    image = buildPeImage(u"Co");
    put32(image.bytes, image.languageEntries[0] + 4, 0x80000000U); // a subdirectory where the data entry belongs
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.languageEntries[0] + 4, 0x7FFFFFF0U); // a data entry outside the section
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.languageEntries[0] + 4, static_cast<std::uint32_t>(image.bytes.size() - image.resources - 6));
    EXPECT_FALSE(find(image.bytes).has_value()) << "a data entry cut by the end of the file";
}

TEST(VersionResourceMathTest, HostileDataEntriesAndBlocksAreNone)
{
    PeImage image = buildPeImage(u"Co");
    put32(image.bytes, image.dataEntries[0] + 4, 0xFFFFFFFFU); // a 4 GB block
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.dataEntries[0] + 4, 8); // too small to be a VS_VERSIONINFO
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.dataEntries[0], 0xFFFFFFF0U); // the data's RVA in no section
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put32(image.bytes, image.dataEntries[0], TestSupport::RESOURCE_RVA + 0x10000); // past the section's raw data
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put16(image.bytes, image.blocks[0], 0xFFFF); // wLength past the data entry
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put16(image.bytes, image.blocks[0], 10); // wLength shorter than its own key
    EXPECT_FALSE(find(image.bytes).has_value());

    image = buildPeImage(u"Co");
    put16(image.bytes, image.blocks[0] + 6, 'X'); // the key isn't VS_VERSION_INFO (a 16-bit block's is ANSI)
    EXPECT_FALSE(find(image.bytes).has_value());
}

/// The offset in @p bytes of the version node whose key is @p key: its header, 6 bytes before the key.
[[nodiscard]] std::size_t nodeAt(const std::vector<std::byte>& bytes, std::u16string_view key)
{
    std::vector<std::byte> pattern;
    for (const char16_t c : key)
    {
        put16(pattern, pattern.size(), c);
    }
    put16(pattern, pattern.size(), 0);
    for (std::size_t at = 6; at + pattern.size() <= bytes.size(); at += 2)
    {
        if (std::equal(pattern.begin(), pattern.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at)))
        {
            return at - 6;
        }
    }
    ADD_FAILURE() << "no such node";
    return 0;
}

/// A file whose block was changed by @p change, which is given the block (offset into the file) to change.
template<typename Change> [[nodiscard]] PeImage changedBlock(Change change)
{
    PeImage image = buildPeImage(u"TaskSmack Test Co");
    change(image.bytes, image.blocks[0]);
    return image;
}

// #4057 review: VerQueryValueW walks the block by each node's own wLength and returns each value with its declared
// length, so a node that lies about either is refused before it sees them, however the root looks.
TEST(VersionResourceMathTest, NestedNodesThatDontFitTheirParentAreNone)
{
    const std::vector<std::byte> block = versionInfoBlock(u"TaskSmack Test Co");
    const std::size_t translation = nodeAt(block, u"Translation");
    const std::size_t company = nodeAt(block, u"CompanyName");
    const std::size_t table = nodeAt(block, u"040904B0");

    const auto at = [](std::size_t node, std::size_t field)
    {
        return [node, field](std::vector<std::byte>& bytes, std::size_t blockAt, std::uint16_t value)
        {
            put16(bytes, blockAt + node + field, value);
        };
    };
    const auto refused = [](auto poke, std::uint16_t value)
    {
        const PeImage image = changedBlock([&](std::vector<std::byte>& bytes, std::size_t blockAt) { poke(bytes, blockAt, value); });
        return !findVersionResource(std::span<const std::byte>(image.bytes)).has_value();
    };

    EXPECT_TRUE(refused(at(translation, 2), 0xFFFF)) << "a Translation value of 64 KB in a 4-byte node";
    EXPECT_TRUE(refused(at(translation, 2), 8)) << "a Translation value one entry too long";
    EXPECT_TRUE(refused(at(company, 2), 0xFFFF)) << "a CompanyName longer than its node, either way it's counted";
    EXPECT_TRUE(refused(at(company, 0), 0)) << "a zero-length node, which would never advance";
    EXPECT_TRUE(refused(at(company, 0), 4)) << "a node shorter than its header";
    EXPECT_TRUE(refused(at(table, 0), 0x7FFF)) << "a string table longer than its parent";
    EXPECT_TRUE(refused(at(0, 2), 0xFFFF)) << "the root's fixed info longer than the block";

    // A text value counted in bytes, not characters, as some linkers write it, still fits, so the file still reads.
    const std::size_t chars = std::u16string_view(u"TaskSmack Test Co").size() + 1;
    EXPECT_FALSE(refused(at(company, 2), static_cast<std::uint16_t>(2 * chars)));
    EXPECT_FALSE(refused(at(company, 2), static_cast<std::uint16_t>(chars))) << "as built";
}

// A tree deeper than any path VerQueryValueW is asked for is refused, rather than walked without bound.
TEST(VersionResourceMathTest, ATreeTooDeepIsNone)
{
    const auto nested = [](int levels)
    {
        TestSupport::VersionNode node{.key = u"Leaf", .value = TestSupport::utf16Value(u"x"), .text = true, .children = {}};
        for (int i = 0; i < levels; ++i)
        {
            node = TestSupport::VersionNode{.key = u"Level", .value = {}, .text = true, .children = {node}};
        }
        const std::vector<std::byte> fixed(52);
        return TestSupport::VersionNode{.key = u"VS_VERSION_INFO", .value = fixed, .text = false, .children = {node}}.bytes();
    };
    const PeImage shallow = buildPeImage({{0x0409U, nested(3)}});
    EXPECT_TRUE(find(shallow.bytes).has_value());
    const PeImage deep = buildPeImage({{0x0409U, nested(12)}});
    EXPECT_FALSE(find(deep.bytes).has_value());
}
// Random damage, from a fixed seed so a failure repeats: whatever the walk answers is inside the file and no
// longer than a VS_VERSIONINFO can be.
TEST(VersionResourceMathTest, RandomlyDamagedFilesAnswerNoneOrASpanInsideTheFile)
{
    const PeImage image = buildPeImage({{0x0409U, versionInfoBlock(u"A")}, {0x0000U, versionInfoBlock(u"B")}});
    std::mt19937 random(4057U); // NOLINT(bugprone-random-generator-seed) - fixed, so a failure repeats
    std::uniform_int_distribution<std::size_t> position(0, image.bytes.size() - 1);
    std::uniform_int_distribution<int> value(0, 255);
    std::uniform_int_distribution<int> damage(1, 8);
    int found = 0;
    for (int round = 0; round < 20000; ++round)
    {
        std::vector<std::byte> bytes = image.bytes;
        for (int i = damage(random); i > 0; --i)
        {
            bytes[position(random)] = static_cast<std::byte>(value(random));
        }
        if (round % 7 == 0)
        {
            bytes.resize(position(random)); // and sometimes cut
        }
        const std::optional<std::span<const std::byte>> block = find(bytes);
        if (block.has_value())
        {
            ++found;
            ASSERT_GE(block->data(), bytes.data());
            ASSERT_LE(block->data() + block->size(), bytes.data() + bytes.size());
            ASSERT_LE(block->size(), 0xFFFFU);
        }
    }
    EXPECT_GT(found, 0) << "most damage misses the walk's bytes, so some files still answer";
}

} // namespace
