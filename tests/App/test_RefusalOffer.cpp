/// @file test_RefusalOffer.cpp
/// @brief One rule and one draw for what a refusal offers (#3812): "Retry as administrator" where the helper can
/// redo it (#2308), "Restart as administrator..." where there is none but TaskSmack can restart (#3658, #3669),
/// and neither otherwise. decideRefusalOffer() is checked on every input; RefusalOffer on what it holds, its line's
/// countdown and its retry; drawRefusalOffer() on what it draws and what a press does. Headless.

#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/RefusalOffer.h"
#include "App/Panels/RestartAsAdministratorButton.h"
#include "Support/RestartOffered.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // ImHashStr(), ActivateItemByID(), the log buffer

#include <array>
#include <functional>
#include <optional>
#include <string>

namespace App
{
namespace
{

// --- decideRefusalOffer(): every input ---

struct DecisionCase
{
    bool refusedForPermission = false;
    bool retryable = false;
    bool helperCanRedo = false;
    bool restartOffered = false;
    RefusalOfferKind expected = RefusalOfferKind::None;
    const char* why = "";
};

/// One row of the table: the four inputs, what they offer, and why.
constexpr DecisionCase
row(bool refusedForPermission, bool retryable, bool helperCanRedo, bool restartOffered, RefusalOfferKind expected, const char* why)
{
    return DecisionCase{
        .refusedForPermission = refusedForPermission,
        .retryable = retryable,
        .helperCanRedo = helperCanRedo,
        .restartOffered = restartOffered,
        .expected = expected,
        .why = why,
    };
}

TEST(RefusalOfferDecisionTest, EveryInputDecidesAsTheRulesSay)
{
    using enum RefusalOfferKind;
    // clang-format off
    constexpr std::array<DecisionCase, 16> CASES{
        // Refused for want of permission:
        row(true,  true,  true,  true,  Retry,   "the helper redoes it: Retry as administrator, never the restart"),
        row(true,  true,  true,  false, Retry,   "the helper's retry needs no restart"),
        row(true,  false, true,  true,  None,    "a helper, but nothing to redo (a retry's own refusal): neither"),
        row(true,  false, true,  false, None,    "a helper, but nothing to redo, and no restart: neither"),
        row(true,  true,  false, true,  Restart, "no helper, where TaskSmack can restart: Restart as administrator..."),
        row(true,  false, false, true,  Restart, "no helper and nothing to redo: the restart still gets it done"),
        row(true,  true,  false, false, None,    "no helper and no restart (elevated, Linux, a synthetic run): neither"),
        row(true,  false, false, false, None,    "nothing at all to offer"),
        // Any other failure: an administrator couldn't do better.
        row(false, true,  true,  true,  None,    "not a permission refusal"),
        row(false, true,  true,  false, None,    "not a permission refusal"),
        row(false, false, true,  true,  None,    "not a permission refusal"),
        row(false, false, true,  false, None,    "not a permission refusal"),
        row(false, true,  false, true,  None,    "not a permission refusal, even with the restart offered"),
        row(false, false, false, true,  None,    "not a permission refusal, even with the restart offered"),
        row(false, true,  false, false, None,    "not a permission refusal"),
        row(false, false, false, false, None,    "not a permission refusal"),
    };
    // clang-format on
    for (const DecisionCase& testCase : CASES)
    {
        EXPECT_EQ(decideRefusalOffer(testCase.refusedForPermission, testCase.retryable, testCase.helperCanRedo, testCase.restartOffered),
                  testCase.expected)
            << testCase.why << " (refused " << testCase.refusedForPermission << ", retryable " << testCase.retryable << ", helper "
            << testCase.helperCanRedo << ", restart " << testCase.restartOffered << ")";
    }
}

TEST(RefusalOfferDecisionTest, TheRestartFollowsRestartAsAdministratorsOwnRule)
{
    for (const bool refused : {false, true})
    {
        for (const bool helper : {false, true})
        {
            for (const bool restart : {false, true})
            {
                EXPECT_EQ(decideRefusalOffer(refused, true, helper, restart) == RefusalOfferKind::Restart,
                          RestartAsAdministrator::offersRestart(refused, helper, restart));
            }
        }
    }
    static_assert(decideRefusalOffer(true, true, true, false) == RefusalOfferKind::Retry, "decided at compile time too");
}

// --- RefusalOffer: what it holds ---

struct FakeRetry
{
    int id = 0;
};

using Offer = RefusalOffer<FakeRetry>;

TEST(RefusalOfferTest, StartsWithNothingOfferedAndNoLine)
{
    const Offer offer;
    EXPECT_EQ(offer.kind(), RefusalOfferKind::None);
    EXPECT_EQ(offer.shown(false), RefusalOfferKind::None);
    EXPECT_EQ(offer.retry(), nullptr);
    EXPECT_FALSE(offer.lineShown());
}

TEST(RefusalOfferTest, ARefusalTheHelperCanRedoHoldsItsRetryAndTheLine)
{
    Offer offer;
    offer.showLine(5.0F);
    EXPECT_EQ(offer.offer(true, FakeRetry{.id = 7}, /*helperCanRedo=*/true, /*restartOffered=*/false), RefusalOfferKind::Retry);
    EXPECT_EQ(offer.kind(), RefusalOfferKind::Retry);
    ASSERT_NE(offer.retry(), nullptr);
    EXPECT_EQ(offer.retry()->id, 7);
    // Held for the authorize time, not the plain line's: time to read it and decide.
    EXPECT_FALSE(offer.tick(Detail::AUTHORIZE_RESULT_SECONDS - 1.0F));
    EXPECT_TRUE(offer.lineShown());
    EXPECT_EQ(offer.kind(), RefusalOfferKind::Retry);
}

TEST(RefusalOfferTest, ARefusalWithNoHelperOffersTheRestartAndKeepsNoRetry)
{
    Offer offer;
    offer.showLine(5.0F);
    EXPECT_EQ(offer.offer(true, FakeRetry{.id = 7}, /*helperCanRedo=*/false, /*restartOffered=*/true), RefusalOfferKind::Restart);
    EXPECT_EQ(offer.retry(), nullptr) << "nothing for Retry as administrator to redo without a helper";
    EXPECT_FALSE(offer.takeRetry().has_value());
    EXPECT_EQ(offer.kind(), RefusalOfferKind::Restart) << "taking a retry that isn't offered leaves the restart";
    EXPECT_FALSE(offer.tick(Detail::AUTHORIZE_RESULT_SECONDS - 1.0F));
    EXPECT_TRUE(offer.lineShown());
}

TEST(RefusalOfferTest, AnotherFailureOffersNeitherAndLeavesTheLinesTime)
{
    Offer offer;
    offer.showLine(5.0F);
    EXPECT_EQ(offer.offer(false, FakeRetry{}, true, true), RefusalOfferKind::None);
    EXPECT_EQ(offer.retry(), nullptr);
    EXPECT_FALSE(offer.tick(4.0F));
    EXPECT_TRUE(offer.tick(1.0F)) << "the plain line's 5 seconds, not the authorize time";
}

TEST(RefusalOfferTest, NoRetryToRedoWithAHelperOffersNeither)
{
    Offer offer;
    EXPECT_EQ(offer.offer(true, std::nullopt, /*helperCanRedo=*/true, /*restartOffered=*/true), RefusalOfferKind::None)
        << "a refusal to the administrator isn't offered again";
    EXPECT_FALSE(offer.lineShown());
}

TEST(RefusalOfferTest, TakingTheRetryDropsTheOfferButNotTheLine)
{
    Offer offer;
    static_cast<void>(offer.offer(true, FakeRetry{.id = 3}, true, false));
    const std::optional<FakeRetry> taken = offer.takeRetry();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken.value_or(FakeRetry{}).id, 3);
    EXPECT_EQ(offer.kind(), RefusalOfferKind::None) << "a refusal to the administrator isn't offered again";
    EXPECT_EQ(offer.retry(), nullptr);
    EXPECT_TRUE(offer.lineShown());
    EXPECT_FALSE(offer.takeRetry().has_value()) << "taken once";
}

TEST(RefusalOfferTest, TheOfferGoesWithItsLine)
{
    Offer offer;
    static_cast<void>(offer.offer(true, FakeRetry{}, true, false));
    EXPECT_FALSE(offer.tick(Detail::AUTHORIZE_RESULT_SECONDS / 2.0F));
    EXPECT_TRUE(offer.tick(Detail::AUTHORIZE_RESULT_SECONDS)) << "true on the frame it runs out";
    EXPECT_EQ(offer.kind(), RefusalOfferKind::None);
    EXPECT_EQ(offer.retry(), nullptr);
    EXPECT_FALSE(offer.lineShown());
    EXPECT_FALSE(offer.tick(1.0F)) << "only the once";
}

TEST(RefusalOfferTest, ANegativeFrameNeverLengthensTheLine)
{
    Offer offer;
    offer.showLine(1.0F);
    EXPECT_FALSE(offer.tick(-10.0F));
    EXPECT_TRUE(offer.tick(1.0F));
}

TEST(RefusalOfferTest, ANewLineDropsTheLastOffer)
{
    Offer offer;
    static_cast<void>(offer.offer(true, FakeRetry{}, false, true));
    offer.showLine(5.0F);
    EXPECT_EQ(offer.kind(), RefusalOfferKind::None);
    EXPECT_TRUE(offer.lineShown());
    offer.showLine(0.0F);
    EXPECT_FALSE(offer.lineShown()) << "no line at all (a selection change)";
}

TEST(RefusalOfferTest, ClearDropsTheOfferAndKeepsTheLine)
{
    Offer offer;
    static_cast<void>(offer.offer(true, FakeRetry{}, true, false));
    offer.clear();
    EXPECT_EQ(offer.kind(), RefusalOfferKind::None);
    EXPECT_EQ(offer.retry(), nullptr);
    EXPECT_TRUE(offer.lineShown());
}

TEST(RefusalOfferTest, HoldLineKeepsTheLineWithNoOffer)
{
    Offer offer;
    offer.showLine(5.0F);
    offer.holdLine(Offer::HOLD_SECONDS);
    EXPECT_EQ(offer.kind(), RefusalOfferKind::None);
    EXPECT_FALSE(offer.tick(Offer::HOLD_SECONDS - 1.0F));
    EXPECT_TRUE(offer.lineShown());
}

TEST(RefusalOfferTest, ShownIsNothingWhileBusyAndNoRetryWhileHeld)
{
    Offer retry;
    static_cast<void>(retry.offer(true, FakeRetry{}, true, false));
    EXPECT_EQ(retry.shown(false), RefusalOfferKind::Retry);
    EXPECT_EQ(retry.shown(true), RefusalOfferKind::None) << "the retry (or another action) runs";
    EXPECT_EQ(retry.shown(false, /*retryHeld=*/true), RefusalOfferKind::None) << "another priority change runs (#3429, #3467)";
    EXPECT_EQ(retry.kind(), RefusalOfferKind::Retry) << "only hidden: the offer stays for when it's done";

    Offer restart;
    static_cast<void>(restart.offer(true, FakeRetry{}, false, true));
    EXPECT_EQ(restart.shown(false), RefusalOfferKind::Restart);
    EXPECT_EQ(restart.shown(true), RefusalOfferKind::None);
    EXPECT_EQ(restart.shown(false, /*retryHeld=*/true), RefusalOfferKind::Restart) << "a held retry doesn't hold the restart";
}

TEST(RefusalOfferTest, OffersTheRestartOnlyWhereMainSaysSo)
{
    Offer offer;
    EXPECT_EQ(offer.offer(true, FakeRetry{}, false), RefusalOfferKind::None) << "not offered: elevated, Linux, a synthetic run";
    const TestSupport::RestartOffered offered;
    EXPECT_EQ(offer.offer(true, FakeRetry{}, false), RefusalOfferKind::Restart) << "RestartAsAdministrator::offered() by default";
}

TEST(RefusalOfferTest, LabelsAreTheButtonsOwn)
{
    EXPECT_EQ(refusalOfferLabel(RefusalOfferKind::None), nullptr);
    EXPECT_STREQ(refusalOfferLabel(RefusalOfferKind::Retry), AuthorizeWording::RETRY_LABEL);
    EXPECT_STREQ(refusalOfferLabel(RefusalOfferKind::Restart), RestartAsAdministrator::LABEL);
    EXPECT_STREQ(refusalOfferLabel(RefusalOfferKind::Retry, RefusalOfferButton{.retryLabel = AuthorizeWording::READ_LABEL}),
                 AuthorizeWording::READ_LABEL);
}

// --- drawRefusalOffer(): what it draws, and what a press does ---

class RefusalOfferDrawTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // ImGui owns the font atlas
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of @p draw in a window of its own, and the text it drew.
    static std::string frame(const std::function<void()>& draw)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin(WINDOW, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        draw();
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

    /// Presses the window's button @p label on the next frame.
    static void press(const char* label)
    {
        const ImGuiWindow* window = ImGui::FindWindowByName(WINDOW);
        ASSERT_NE(window, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, window->ID));
    }

    static constexpr const char* WINDOW = "Line";

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(RefusalOfferDrawTest, NoneDrawsNothing)
{
    int retries = 0;
    bool pressed = true;
    const std::string text =
        frame([&] { pressed = drawRefusalOffer(RefusalOfferKind::None, [&retries] { ++retries; }, RefusalOfferButton{.sameLine = true}); });
    EXPECT_TRUE(text.empty()) << text;
    EXPECT_FALSE(pressed);
    EXPECT_EQ(retries, 0);
}

TEST_F(RefusalOfferDrawTest, RetryDrawsItsLabelAndAPressRetries)
{
    const TestSupport::RestartOffered offered;
    int retries = 0;
    const std::string text = frame([&retries] { static_cast<void>(drawRefusalOffer(RefusalOfferKind::Retry, [&retries] { ++retries; })); });
    EXPECT_TRUE(text.contains(AuthorizeWording::RETRY_TEXT)) << text;
    EXPECT_FALSE(text.contains("Restart as administrator...")) << text;
    EXPECT_EQ(retries, 0);

    press(AuthorizeWording::RETRY_LABEL);
    bool pressed = false;
    static_cast<void>(frame([&] { pressed = drawRefusalOffer(RefusalOfferKind::Retry, [&retries] { ++retries; }); }));
    EXPECT_TRUE(pressed);
    EXPECT_EQ(retries, 1);
    EXPECT_TRUE(offered.raised().empty()) << "the helper's retry, not the restart";
}

TEST_F(RefusalOfferDrawTest, RetryTakesTheViewsLabel)
{
    const std::string text = frame(
        []
        {
            static_cast<void>(drawRefusalOffer(
                RefusalOfferKind::Retry,
                [] {},
                RefusalOfferButton{
                    .retryLabel = AuthorizeWording::READ_LABEL,
                    .retryTooltip = AuthorizeWording::READ_TOOLTIP,
                }));
        });
    EXPECT_TRUE(text.contains(AuthorizeWording::READ_TEXT)) << text;
}

TEST_F(RefusalOfferDrawTest, RestartDrawsTheRestartAndAPressRaisesTheNotice)
{
    const TestSupport::RestartOffered offered;
    int retries = 0;
    const std::string text =
        frame([&retries] { static_cast<void>(drawRefusalOffer(RefusalOfferKind::Restart, [&retries] { ++retries; })); });
    EXPECT_TRUE(text.contains("Restart as administrator...")) << text;
    EXPECT_FALSE(text.contains(AuthorizeWording::RETRY_TEXT)) << text;

    press(RestartAsAdministrator::LABEL);
    bool pressed = false;
    static_cast<void>(frame([&] { pressed = drawRefusalOffer(RefusalOfferKind::Restart, [&retries] { ++retries; }); }));
    EXPECT_TRUE(pressed);
    EXPECT_TRUE(offered.raisedTheNoticeOnce());
    EXPECT_EQ(retries, 0) << "the restart, not a retry through a helper there isn't";
}

TEST_F(RefusalOfferDrawTest, SameLineFollowsTheLastItem)
{
    float textY = 0.0F;
    float buttonY = 0.0F;
    float textRight = 0.0F;
    float buttonLeft = 0.0F;
    const auto draw = [&](bool sameLine)
    {
        ImGui::TextUnformatted("Couldn't do it: access denied");
        textY = ImGui::GetItemRectMin().y;
        textRight = ImGui::GetItemRectMax().x;
        static_cast<void>(drawRefusalOffer(RefusalOfferKind::Retry, [] {}, RefusalOfferButton{.sameLine = sameLine}));
        buttonY = ImGui::GetItemRectMin().y;
        buttonLeft = ImGui::GetItemRectMin().x;
    };
    static_cast<void>(frame([&draw] { draw(true); }));
    EXPECT_FLOAT_EQ(buttonY, textY) << "on the line's row";
    EXPECT_GT(buttonLeft, textRight);
    static_cast<void>(frame([&draw] { draw(false); }));
    EXPECT_GT(buttonY, textY) << "under the line";
}

} // namespace
} // namespace App
