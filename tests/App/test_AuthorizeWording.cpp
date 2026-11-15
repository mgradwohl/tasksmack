// Tests for AuthorizeWording (#2550): the "act or read as an administrator" buttons say what each
// platform's prompt does. Linux's pkexec asks for a password ("Authenticate"); Windows' UAC prompt asks
// for an administrator's approval, as "Run as administrator" does.

#include "App/Panels/AuthorizeWording.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>

namespace App
{
namespace
{

TEST(AuthorizeWordingTest, TheLabelsAreTheLockThenTheWords)
{
    EXPECT_EQ(std::string(AuthorizeWording::RETRY_LABEL), std::string(ICON_FA_LOCK "  ") + AuthorizeWording::RETRY_TEXT);
    EXPECT_EQ(std::string(AuthorizeWording::READ_LABEL), std::string(ICON_FA_LOCK "  ") + AuthorizeWording::READ_TEXT);
}

TEST(AuthorizeWordingTest, EachPlatformSaysWhatItsPromptAsksFor)
{
#ifdef _WIN32
    EXPECT_EQ(std::string_view(AuthorizeWording::RETRY_TEXT), "Retry as administrator");
    EXPECT_EQ(std::string_view(AuthorizeWording::READ_TEXT), "Read as administrator");
    EXPECT_TRUE(std::string_view(AuthorizeWording::RETRY_TOOLTIP).contains("UAC")) << "Windows asks for approval, not a password";
    EXPECT_FALSE(std::string_view(AuthorizeWording::READ_TOOLTIP).contains("password"));
#else
    EXPECT_EQ(std::string_view(AuthorizeWording::RETRY_TEXT), "Authenticate and retry");
    EXPECT_EQ(std::string_view(AuthorizeWording::READ_TEXT), "Authenticate to read");
    EXPECT_TRUE(std::string_view(AuthorizeWording::RETRY_TOOLTIP).starts_with("Asks for an administrator's password"));
#endif
    for (const std::string_view tooltip : {std::string_view(AuthorizeWording::RETRY_TOOLTIP),
                                           std::string_view(AuthorizeWording::READ_TOOLTIP),
                                           std::string_view(AuthorizeWording::PRIORITY_TOOLTIP),
                                           std::string_view(AuthorizeWording::IO_PRIORITY_TOOLTIP)})
    {
        EXPECT_TRUE(tooltip.ends_with("as the administrator")) << tooltip;
    }
}

} // namespace
} // namespace App
