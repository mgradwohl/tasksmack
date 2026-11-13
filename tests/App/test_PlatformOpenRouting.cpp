/// @file test_PlatformOpenRouting.cpp
/// @brief App::PlatformOpen's choice of opener (#3658): an unelevated TaskSmack opens in its own process
/// (ShellExecuteW, xdg-open); an elevated one only through the signed-in user's shell (Explorer's
/// IShellDispatch2), and never falls back to opening elevated, for a URL, a file or a folder alike. Fakes
/// stand in for both openers: no browser, editor or Explorer window is started. The real shell call is
/// checked by hand (the PR's checklist).

#include "App/PlatformOpen.h"

#include <gtest/gtest.h>

#include <string>

namespace App
{
namespace
{

using PlatformOpen::OpenResult;
using PlatformOpen::Detail::Openers;
using PlatformOpen::Detail::openRouted;

/// Fake openers that count their calls and answer as set.
struct FakeOpeners
{
    bool directOpens = true;
    bool unelevatedOpens = true;
    int directCalls = 0;
    int unelevatedCalls = 0;

    [[nodiscard]] Openers openers()
    {
        return {
            .direct =
                [this]
            {
                ++directCalls;
                return directOpens;
            },
            .unelevated =
                [this]
            {
                ++unelevatedCalls;
                return unelevatedOpens;
            },
        };
    }
};

TEST(PlatformOpenRoutingTest, NotElevatedOpensInThisProcess)
{
    FakeOpeners fakes;
    EXPECT_EQ(openRouted(false, fakes.openers()), OpenResult::Opened);
    EXPECT_EQ(fakes.directCalls, 1);
    EXPECT_EQ(fakes.unelevatedCalls, 0) << "Explorer's shell is only for an elevated TaskSmack";

    fakes.directOpens = false;
    EXPECT_EQ(openRouted(false, fakes.openers()), OpenResult::Failed) << "a plain failure, not withheld";
    EXPECT_EQ(fakes.unelevatedCalls, 0);
}

TEST(PlatformOpenRoutingTest, ElevatedOpensOnlyThroughTheSignedInUsersShell)
{
    FakeOpeners fakes;
    EXPECT_EQ(openRouted(true, fakes.openers()), OpenResult::Opened);
    EXPECT_EQ(fakes.unelevatedCalls, 1);
    EXPECT_EQ(fakes.directCalls, 0) << "never ShellExecuteW in the elevated process";
}

// Explorer isn't running, or a step of the IShellDispatch2 chain failed: nothing opens elevated instead.
TEST(PlatformOpenRoutingTest, ElevatedWithTheShellFailingIsWithheldNotOpenedElevated)
{
    FakeOpeners fakes;
    fakes.unelevatedOpens = false;
    EXPECT_EQ(openRouted(true, fakes.openers()), OpenResult::Withheld);
    EXPECT_EQ(fakes.unelevatedCalls, 1);
    EXPECT_EQ(fakes.directCalls, 0) << "no fallback to an elevated browser, editor or Explorer window";

    Openers noShell = fakes.openers();
    noShell.unelevated = nullptr; // no unelevated opener at all
    EXPECT_EQ(openRouted(true, noShell), OpenResult::Withheld);
    EXPECT_EQ(fakes.directCalls, 0);
}

TEST(PlatformOpenRoutingTest, NoDirectOpenerIsAFailure)
{
    EXPECT_EQ(openRouted(false, Openers{}), OpenResult::Failed);
}

TEST(PlatformOpenRoutingTest, TheWithheldNoticeNamesTheTargetAndWhy)
{
    const std::string url = "https://github.com/mgradwohl/tasksmack/issues";
    const std::string notice = PlatformOpen::withheldNotice(url);
    EXPECT_TRUE(notice.contains("administrator")) << notice;
    EXPECT_TRUE(notice.ends_with("Open it yourself: " + url)) << notice;
}

} // namespace
} // namespace App
