/// @file test_SourceEncoding.cpp
/// @brief The sources are compiled as UTF-8 (#1648)
///
/// tasksmack_apply_text_encoding() (cmake/CompilerOptions.cmake) passes /utf-8 or
/// -finput-charset=UTF-8 -fexec-charset=UTF-8, so a non-ASCII narrow literal holds its UTF-8 bytes
/// whatever the build machine's code page, and the glyphs ImGui draws are the right ones. The
/// expectations are escapes, not literals, so they can't drift with the source charset.

#include "UI/Format.h"

#include <gtest/gtest.h>

#include <string_view>

namespace
{

TEST(SourceEncodingTest, NonAsciiLiteralsAreUtf8)
{
    EXPECT_EQ(std::string_view{"°"}, std::string_view{"\xC2\xB0"});
    EXPECT_EQ(std::string_view{"×"}, std::string_view{"\xC3\x97"});
    EXPECT_EQ(std::string_view{"≈"}, std::string_view{"\xE2\x89\x88"});
    EXPECT_EQ(std::string_view{"—"}, std::string_view{"\xE2\x80\x94"});
}

TEST(SourceEncodingTest, FormattedTemperatureIsUtf8)
{
    EXPECT_EQ(UI::Format::formatCelsius(65.0),
              std::string_view{"65\xC2\xB0"
                               "C"});
}

} // namespace
