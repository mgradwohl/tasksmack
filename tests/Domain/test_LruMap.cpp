/// @file test_LruMap.cpp
/// @brief Domain::LruMap (#1800): bounded to its capacity, dropping the least recently used entry,
/// with find() and put() of a present key making it the most recent.

#include "Domain/LruMap.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace Domain
{
namespace
{

[[nodiscard]] std::vector<int> keysInOrder(const LruMap<int, std::string>& map)
{
    std::vector<int> keys;
    map.forEach([&keys](int key, const std::string& /*value*/) { keys.push_back(key); });
    return keys;
}

TEST(LruMapTest, HoldsUpToItsCapacityAndDropsTheLeastRecentlyUsed)
{
    LruMap<int, std::string> map(2);
    EXPECT_FALSE(map.put(1, "one").has_value());
    EXPECT_FALSE(map.put(2, "two").has_value());
    const auto dropped = map.put(3, "three");
    ASSERT_TRUE(dropped.has_value());
    EXPECT_EQ(dropped->first, 1);
    EXPECT_EQ(dropped->second, "one");
    EXPECT_EQ(map.size(), 2U);
    EXPECT_EQ(map.find(1), nullptr);
    EXPECT_EQ(keysInOrder(map), (std::vector<int>{3, 2}));
}

TEST(LruMapTest, FindMakesAnEntryTheMostRecent)
{
    LruMap<int, std::string> map(2);
    static_cast<void>(map.put(1, "one"));
    static_cast<void>(map.put(2, "two"));
    ASSERT_NE(map.find(1), nullptr);
    const auto dropped = map.put(3, "three");
    ASSERT_TRUE(dropped.has_value());
    EXPECT_EQ(dropped->first, 2); // 1 was used since
    EXPECT_NE(map.find(1), nullptr);
}

TEST(LruMapTest, PeekLeavesTheOrderAlone)
{
    LruMap<int, std::string> map(2);
    static_cast<void>(map.put(1, "one"));
    static_cast<void>(map.put(2, "two"));
    const LruMap<int, std::string>& constMap = map;
    ASSERT_NE(constMap.peek(1), nullptr);
    EXPECT_EQ(*constMap.peek(1), "one");
    EXPECT_EQ(map.put(3, "three")->first, 1);
}

TEST(LruMapTest, PutOfAPresentKeyReplacesItsValueWithoutDropping)
{
    LruMap<int, std::string> map(2);
    static_cast<void>(map.put(1, "one"));
    static_cast<void>(map.put(2, "two"));
    EXPECT_FALSE(map.put(1, "uno").has_value());
    EXPECT_EQ(map.size(), 2U);
    EXPECT_EQ(*map.find(1), "uno");
    EXPECT_EQ(keysInOrder(map), (std::vector<int>{1, 2}));
}

TEST(LruMapTest, EraseAndClear)
{
    LruMap<int, std::string> map(3);
    static_cast<void>(map.put(1, "one"));
    static_cast<void>(map.put(2, "two"));
    EXPECT_TRUE(map.erase(1));
    EXPECT_FALSE(map.erase(1));
    EXPECT_EQ(map.size(), 1U);
    map.clear();
    EXPECT_EQ(map.size(), 0U);
    EXPECT_EQ(map.find(2), nullptr);
}

TEST(LruMapTest, AZeroCapacityHoldsOne)
{
    LruMap<int, int> map(0);
    EXPECT_EQ(map.capacity(), 1U);
    static_cast<void>(map.put(1, 1));
    EXPECT_NE(map.find(1), nullptr);
    EXPECT_TRUE(map.put(2, 2).has_value());
    EXPECT_EQ(map.size(), 1U);
}

} // namespace
} // namespace Domain
