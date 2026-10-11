/// @file test_FormatProperties.cpp
/// @brief Property-based tests (#139, rapidcheck) for the locale-aware number formatting in UI/Format.h:
/// digit grouping for any digit string under any grouping and separator, and the integer formatter's
/// digits for any value. Each property is checked against a few hundred generated cases, shrunk to the
/// smallest failing one when it breaks.

#include "UI/Format.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

namespace UI::Format
{
namespace
{

/// The digits Detail::putGroupedDigits() writes for @p digits under @p punct.
[[nodiscard]] std::string grouped(const std::string& digits, const NumericPunctuation& punct)
{
    // putGroupedDigits() is noexcept, so its sink writes into fixed room: four bytes per digit covers
    // a three-byte separator after every one.
    std::vector<char> buffer(digits.size() * 4);
    std::size_t length = 0;
    const auto put = [&buffer, &length](char c) noexcept
    {
        if (length < buffer.size())
        {
            buffer[length] = c;
        }
        ++length;
    };
    Detail::putGroupedDigits(digits.data(), digits.size(), punct, put);
    RC_ASSERT(length <= buffer.size());
    return {buffer.data(), length};
}

/// @p text with every occurrence of @p separator removed (nothing to remove for an empty one).
[[nodiscard]] std::string withoutSeparators(std::string text, const std::string& separator)
{
    if (separator.empty())
    {
        return text;
    }
    for (std::size_t at = text.find(separator); at != std::string::npos; at = text.find(separator, at))
    {
        text.erase(at, separator.size());
    }
    return text;
}

/// A digit string of one to 40 digits.
[[nodiscard]] rc::Gen<std::string> digitStrings()
{
    return rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('0', static_cast<char>('9' + 1))));
}

/// A numpunct grouping: one to four sizes of 1..5, possibly ending in CHAR_MAX ("no more grouping").
[[nodiscard]] rc::Gen<std::string> groupings()
{
    return rc::gen::map(rc::gen::pair(rc::gen::container<std::vector<int>>(rc::gen::inRange(1, 6)), rc::gen::arbitrary<bool>()),
                        [](const std::pair<std::vector<int>, bool>& parts)
                        {
                            std::string grouping;
                            for (std::size_t i = 0; i < parts.first.size() && i < 4; ++i)
                            {
                                grouping += static_cast<char>(parts.first[i]);
                            }
                            if (grouping.empty())
                            {
                                grouping = "\3";
                            }
                            if (parts.second)
                            {
                                grouping += static_cast<char>(127); // CHAR_MAX: stop grouping there
                            }
                            return grouping;
                        });
}

/// A separator as locales have them: ',' '.' ' ', or U+202F (narrow no-break space, three bytes).
[[nodiscard]] rc::Gen<std::string> separators()
{
    return rc::gen::element(std::string(","), std::string("."), std::string(" "), std::string("\xE2\x80\xAF"));
}

// Grouping only adds separators: taking them out gives the digits back, unchanged and in order.
RC_GTEST_PROP(FormatPropertiesTest, GroupingOnlyInsertsSeparators, ())
{
    const std::string digits = *digitStrings();
    const NumericPunctuation punct{.decimalPoint = '.', .thousandsSep = *separators(), .grouping = *groupings()};
    RC_ASSERT(withoutSeparators(grouped(digits, punct), punct.thousandsSep) == digits);
}

// A separator never starts or ends the number, and two never touch: each one sits between digits.
RC_GTEST_PROP(FormatPropertiesTest, SeparatorsSitBetweenDigits, ())
{
    const std::string digits = *digitStrings();
    const NumericPunctuation punct{.decimalPoint = '.', .thousandsSep = *separators(), .grouping = *groupings()};
    const std::string text = grouped(digits, punct);
    const std::string& sep = punct.thousandsSep;
    RC_ASSERT(!text.starts_with(sep));
    RC_ASSERT(!text.ends_with(sep));
    RC_ASSERT(text.find(sep + sep) == std::string::npos);
}

// The first group from the right is exactly the grouping's first size, whenever there are more digits
// than that: "1234567" under "\3" ends ",567".
RC_GTEST_PROP(FormatPropertiesTest, TheRightmostGroupHasTheFirstSize, ())
{
    const std::string digits = *digitStrings();
    const NumericPunctuation punct{.decimalPoint = '.', .thousandsSep = ",", .grouping = *groupings()};
    const auto first = static_cast<std::size_t>(static_cast<unsigned char>(punct.grouping.front()));
    RC_PRE(digits.size() > first);
    const std::string text = grouped(digits, punct);
    const std::size_t last = text.rfind(',');
    RC_ASSERT(last != std::string::npos);
    RC_ASSERT(text.size() - last - 1 == first);
}

// No grouping (the classic locale), or no separator: the digits exactly as they are.
RC_GTEST_PROP(FormatPropertiesTest, NoGroupingLeavesTheDigitsAlone, ())
{
    const std::string digits = *digitStrings();
    RC_ASSERT(grouped(digits, NumericPunctuation{.decimalPoint = '.', .thousandsSep = ",", .grouping = ""}) == digits);
    RC_ASSERT(grouped(digits, NumericPunctuation{.decimalPoint = '.', .thousandsSep = "", .grouping = "\3"}) == digits);
}

// The integer formatter writes every value's decimal digits (and sign), whatever the display grouping.
RC_GTEST_PROP(FormatPropertiesTest, IntegersKeepTheirDigits, (std::int64_t value))
{
    std::array<char, 64> out{};
    const std::size_t length = formatIntLocalizedTo(out.data(), out.size(), value);
    RC_ASSERT(length > 0);
    const std::string text(out.data(), length);
    RC_ASSERT(withoutSeparators(text, numericPunctuation().thousandsSep) == std::to_string(value));
}

} // namespace
} // namespace UI::Format
