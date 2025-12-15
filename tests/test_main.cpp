#include <gtest/gtest.h>

#include <string>

TEST(ExampleTest, BasicAssertion)
{
    EXPECT_EQ(1 + 1, 2);
}

TEST(ExampleTest, StringComparison)
{
    std::string hello = "Hello";
    std::string world = "World";
    EXPECT_NE(hello, world);
    EXPECT_EQ(hello + ", " + world + "!", "Hello, World!");
}
