#include "Platform/UnicodeCaseFoldTable.h"
#include "Platform/Utf8CaseFold.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace Platform
{
namespace
{

// #4302: user-visible matching is Unicode simple case folding (the maintainer's D2).

TEST(Utf8CaseFoldTest, TheTableIsSortedAndFoldsTheIssuesLetters)
{
    const auto& table = UnicodeCaseFold::SIMPLE_FOLDS;
    EXPECT_TRUE(std::ranges::is_sorted(table, {}, [](const auto& pair) { return pair.first; }));
    EXPECT_EQ(simpleCaseFold(U'É'), U'é');
    EXPECT_EQ(simpleCaseFold(U'Ж'), U'ж');
    EXPECT_EQ(simpleCaseFold(U'Σ'), U'σ');
    EXPECT_EQ(simpleCaseFold(U'ς'), U'σ'); // final sigma folds with sigma
    EXPECT_EQ(simpleCaseFold(U'K'), U'k'); // KELVIN SIGN
    EXPECT_EQ(simpleCaseFold(U'ẞ'), U'ß'); // capital sharp s: the S mapping, not F's "ss"
    EXPECT_EQ(simpleCaseFold(U'İ'), U'İ') << "Turkish I with dot has only F and T mappings";
    EXPECT_EQ(simpleCaseFold(U'é'), U'é');
    EXPECT_EQ(simpleCaseFold(U'A'), U'a');
    EXPECT_EQ(simpleCaseFold(U'1'), U'1');
}

// #4662: three CaseFolding.txt S mappings the generator once missed.
TEST(Utf8CaseFoldTest, TheThreeSMappingsTheGeneratorOnceMissedFold)
{
    struct Pair
    {
        char32_t from;
        char32_t to;
        std::string_view fromUtf8;
        std::string_view toUtf8;
    };
    const auto pairs = std::to_array<Pair>({
        {.from = U'ΐ', .to = U'ΐ', .fromUtf8 = "ΐ", .toUtf8 = "ΐ"}, // U+1FD3 -> U+0390 (iota with dialytika)
        {.from = U'ΰ', .to = U'ΰ', .fromUtf8 = "ΰ", .toUtf8 = "ΰ"}, // U+1FE3 -> U+03B0 (upsilon with dialytika)
        {.from = U'ﬅ', .to = U'ﬆ', .fromUtf8 = "ﬅ", .toUtf8 = "ﬆ"}, // U+FB05 -> U+FB06 (long s t ligature)
    });
    for (const auto& pair : pairs)
    {
        EXPECT_EQ(simpleCaseFold(pair.from), pair.to);
        EXPECT_EQ(simpleCaseFold(pair.to), pair.to);
        EXPECT_TRUE(equalsFoldedCase(pair.fromUtf8, pair.toUtf8));
        EXPECT_TRUE(equalsFoldedCase(pair.toUtf8, pair.fromUtf8));
        EXPECT_TRUE(containsFoldedCase(std::string("a") + std::string(pair.fromUtf8) + "z", pair.toUtf8));
        EXPECT_TRUE(containsFoldedCase(std::string("a") + std::string(pair.toUtf8) + "z", pair.fromUtf8));
    }
}

TEST(Utf8CaseFoldTest, ContainsFindsTheIssuesScenarios)
{
    EXPECT_TRUE(containsFoldedCase("Émile.exe", "émile"));
    EXPECT_TRUE(containsFoldedCase("Яндекс", "яндекс"));
    EXPECT_TRUE(containsFoldedCase("Ölfilter.dll", "ölfilter"));
    EXPECT_TRUE(containsFoldedCase("ΣΟΦΊΑ", "σοφία"));
    EXPECT_TRUE(containsFoldedCase("ΣΟΦΊΑς", "σ"));
    EXPECT_TRUE(containsFoldedCase("273 K", "k")) << "an ASCII term still matches a non-ASCII fold";
    EXPECT_TRUE(containsFoldedCase("bash --login", "LOGIN"));
    EXPECT_TRUE(containsFoldedCase("anything", ""));
    EXPECT_FALSE(containsFoldedCase("Résumé", "resume")) << "no accent stripping";
    EXPECT_FALSE(containsFoldedCase("Straße", "STRASSE")) << "simple folding, not full: ß is not ss";
}

TEST(Utf8CaseFoldTest, EqualsAndAffixesFoldWhateverTheByteLengths)
{
    EXPECT_TRUE(equalsFoldedCase("Élodie", "élodie"));
    EXPECT_TRUE(equalsFoldedCase("K", "k")) << "three bytes against one";
    EXPECT_FALSE(equalsFoldedCase("Élodie", "élodi"));
    EXPECT_TRUE(startsWithFoldedCase("Élodie", "éLO"));
    EXPECT_FALSE(startsWithFoldedCase("Élo", "élodie"));
    EXPECT_TRUE(endsWithFoldedCase("WORKK", "rkk"));
    EXPECT_TRUE(endsWithFoldedCase("PC", ""));
    EXPECT_FALSE(endsWithFoldedCase("PC", "xpc"));
}

TEST(Utf8CaseFoldTest, IllFormedBytesNeverCrashAndNeverMatchACharacter)
{
    // A truncated sequence at the end, a lone lead byte, a stray continuation, an overlong and a surrogate.
    constexpr auto texts = std::to_array<std::string_view>({
        "caf\xC3",
        "Caf\xE9.txt",
        "\x80\xBF",
        "\xC0\xAF",
        "\xED\xA0\x80",
        "\xF0\x9F\x98",
    });
    for (const std::string_view text : texts)
    {
        EXPECT_FALSE(containsFoldedCase(text, "é")) << "an ill-formed byte is not é";
        EXPECT_FALSE(containsFoldedCase(text, "\xEF\xBF\xBD")) << "nor U+FFFD";
        EXPECT_TRUE(containsFoldedCase(text, text)) << "the same bytes still match themselves";
    }
    EXPECT_TRUE(containsFoldedCase("Caf\xE9.txt", ".TXT"));
    EXPECT_FALSE(containsFoldedCase("caf\xC3", "caf\xC3\xA9")) << "the term runs past the text's end";
}

TEST(Utf8CaseFoldTest, CompareSortsCaseVariantsTogetherThenByBytes)
{
    // #4324: "Émile" and "émile" sorted far apart. Folded they sort side by side; both still come after "Zoe" in
    // code point order (é is U+00E9), as folding is not collation.
    EXPECT_TRUE(compareFoldedCase("Émile", "émile") < 0) << "the byte-wise tie-break: É (C3 89) before é (C3 A9)";
    EXPECT_TRUE(compareFoldedCase("émile", "émilf") < 0) << "nothing that differs later sorts between them";
    EXPECT_TRUE(compareFoldedCase("Émilf", "émile") > 0);
    EXPECT_TRUE(compareFoldedCase("Zoe", "Émile") < 0);
    EXPECT_TRUE(compareFoldedCase("émile", "Émile") > 0);
    EXPECT_TRUE(compareFoldedCase("émile", "émile") == 0);
    EXPECT_TRUE(compareFoldedCase("bits", "BITS2") < 0) << "a prefix sorts first";
    EXPECT_TRUE(compareFoldedCase("Appinfo", "bits") < 0);
    EXPECT_TRUE(compareFoldedCase("BITS", "bits") < 0) << "ASCII ties break by bytes too";
    EXPECT_TRUE(compareFoldedCase("\u212A", "l") < 0) << "U+212A folds to k, which is before l";
}

TEST(Utf8CaseFoldTest, CompareFoldedTreatsCaseVariantsAsOneKey)
{
    // For keys such as Windows account names (#3310): no tie-break, so the variants are equivalent.
    EXPECT_TRUE(compareFolded("alice", "ALICE") == 0);
    EXPECT_TRUE(compareFolded("\xC3\x89lodie", "\xC3\xA9LODIE") == 0); // Élodie, éLODIE
    EXPECT_TRUE(compareFolded("alice", "alicex") < 0);
    EXPECT_TRUE(compareFolded("bob", "Alice") > 0);
}

} // namespace
} // namespace Platform
