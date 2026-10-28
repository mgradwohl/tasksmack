// The Processes table's incremental search (#3097): which row a typed name jumps to.

#include "App/Panels/IncrementalSearch.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace App
{
namespace
{

using IncrementalSearch::findMatch;

constexpr std::array<std::string_view, 5> NAMES{"systemd", "Xorg", "firefox", "bash", "firefox-bin"};

TEST(IncrementalSearchTest, TypingFindsTheFirstMatchFromTheSelectedRow)
{
    EXPECT_EQ(findMatch(NAMES, std::nullopt, "FIRE", false), std::optional<std::size_t>{2}); // from the top
    EXPECT_EQ(findMatch(NAMES, 2, "fire", false), std::optional<std::size_t>{2});            // the selected row matches still
    EXPECT_EQ(findMatch(NAMES, 3, "fire", false), std::optional<std::size_t>{4});
    EXPECT_EQ(findMatch(NAMES, 3, "xorg", false), std::optional<std::size_t>{1}); // wraps round
    EXPECT_EQ(findMatch(NAMES, 0, "zsh", false), std::nullopt);
    EXPECT_EQ(findMatch(NAMES, 0, "", false), std::nullopt);
    EXPECT_EQ(findMatch({}, 0, "a", false), std::nullopt);
}

TEST(IncrementalSearchTest, NextMovesOnAndWrapsRoundToTheSameRowLast)
{
    EXPECT_EQ(findMatch(NAMES, 2, "fire", true), std::optional<std::size_t>{4});
    EXPECT_EQ(findMatch(NAMES, 4, "fire", true), std::optional<std::size_t>{2});
    EXPECT_EQ(findMatch(NAMES, 1, "xorg", true), std::optional<std::size_t>{1}); // the only match: stays
    EXPECT_EQ(findMatch(NAMES, std::nullopt, "bash", true), std::optional<std::size_t>{3});
    EXPECT_EQ(findMatch(NAMES, 9, "systemd", true), std::optional<std::size_t>{0}); // a stale row: from the top
}

} // namespace
} // namespace App
