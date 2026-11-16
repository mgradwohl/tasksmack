/// @file test_SectionCaches.cpp
/// @brief The caches Process Details' list sections share (#3887): FilteredRows builds the filter's rows again only
/// when the rows or the filter change, and CountedHeader formats the header only when its count changes.

#include "App/Panels/SectionCaches.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace App::Detail
{
namespace
{

TEST(SectionCachesTest, FilteredRowsAreBuiltAgainOnlyWhenTheRowsOrFilterChange)
{
    const std::vector<std::string> rows{"alpha", "beta", "alphabet"};
    int calls = 0;
    const auto matches = [&](const std::string& filter)
    {
        return [&rows, &calls, filter](std::size_t i)
        {
            ++calls;
            return rows[i].contains(filter);
        };
    };
    FilteredRows cache;
    std::string filter = "alpha";
    const auto first = cache.get(rows.size(), filter, matches(filter));
    EXPECT_EQ(std::vector<std::size_t>(first.begin(), first.end()), (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(calls, 3);
    static_cast<void>(cache.get(rows.size(), filter, matches(filter)));
    EXPECT_EQ(calls, 3) << "same rows, same filter: not built again";

    filter = "beta";
    EXPECT_EQ(cache.get(rows.size(), filter, matches(filter)).size(), 1U);
    EXPECT_EQ(calls, 6) << "another filter";

    cache.markDirty();
    static_cast<void>(cache.get(rows.size(), filter, matches(filter)));
    EXPECT_EQ(calls, 9) << "the rows changed";

    cache.reset();
    EXPECT_EQ(cache.get(0, filter, matches(filter)).size(), 0U) << "a new selection starts empty";
}

TEST(SectionCachesTest, TheHeaderShowsTheCountOnlyWhileListedAndKeepsItsId)
{
    CountedHeader header;
    EXPECT_EQ(std::string_view(header.label(false, 0, "Modules", "###ProcessModules")), "Modules###ProcessModules");
    const char* first = header.label(true, 87, "Modules", "###ProcessModules");
    EXPECT_EQ(std::string_view(first), "Modules (87)###ProcessModules");
    EXPECT_EQ(header.label(true, 87, "Modules", "###ProcessModules"), first) << "the same count: not formatted again";
    EXPECT_EQ(std::string_view(header.label(true, 88, "Modules", "###ProcessModules")), "Modules (88)###ProcessModules");
    EXPECT_EQ(std::string_view(header.label(true, 0, "Modules", "###ProcessModules")), "Modules (0)###ProcessModules");
}

} // namespace
} // namespace App::Detail
