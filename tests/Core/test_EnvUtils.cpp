#include "Core/EnvUtils.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace Core
{
namespace
{

TEST(EnvUtilsTest, NullAndEmptyValuesAreDisabled)
{
    EXPECT_FALSE(isEnvFlagEnabled(nullptr));
    EXPECT_FALSE(isEnvFlagEnabled(""));
}

TEST(EnvUtilsTest, DisabledValuesAreCaseInsensitive)
{
    constexpr std::array<std::string_view, 8> disabledValues = {
        "0",
        "false",
        "FALSE",
        "off",
        "OFF",
        "no",
        "NO",
        "No",
    };

    for (const std::string_view value : disabledValues)
    {
        EXPECT_FALSE(isEnvFlagEnabled(value.data())) << value;
    }
}

TEST(EnvUtilsTest, OtherNonEmptyValuesAreEnabled)
{
    constexpr std::array<std::string_view, 5> enabledValues = {
        "1",
        "true",
        "yes",
        "on",
        "anything",
    };

    for (const std::string_view value : enabledValues)
    {
        EXPECT_TRUE(isEnvFlagEnabled(value.data())) << value;
    }
}

} // namespace
} // namespace Core
