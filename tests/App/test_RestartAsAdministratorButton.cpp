/// @file test_RestartAsAdministratorButton.cpp
/// @brief "Restart as administrator..." beside a refused action or read where there is no helper (#3658):
/// its words are the chip's (AuthorizeWording::WINDOWS_WORDING.switchSession), it is offered only where
/// main.cpp says TaskSmack can restart and has no helper, and a press raises Core::OpenElevationNoticeEvent,
/// the notice whose Restart as administrator... is the same restart as #2728's. The Process Details sections'
/// shared button (AuthenticateToReadButton.h) offers it for a refused read the helper can't read. Headless.

#include "App/Panels/AuthenticateToReadButton.h"
#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/RestartAsAdministratorButton.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // ImHashStr(), ActivateItemByID(), the log buffer

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

/// Offers the restart for the test's length, as main.cpp does with no helper, and keeps what a press raised.
class RestartOffered
{
  public:
    RestartOffered()
    {
        RestartAsAdministrator::setOffered([this](Core::Event& event) { m_Raised.push_back(event.getEventType()); });
    }
    ~RestartOffered()
    {
        RestartAsAdministrator::setOffered({});
    }
    RestartOffered(const RestartOffered&) = delete;
    RestartOffered& operator=(const RestartOffered&) = delete;
    RestartOffered(RestartOffered&&) = delete;
    RestartOffered& operator=(RestartOffered&&) = delete;

    [[nodiscard]] const std::vector<Core::EventType>& raised() const noexcept
    {
        return m_Raised;
    }

  private:
    std::vector<Core::EventType> m_Raised;
};

/// A Process Details section, as AuthenticateToReadButton.h sees one.
struct FakeSection
{
    bool refused = false;   ///< The last read was refused for want of permission
    bool canHelper = false; ///< A helper can read it as an administrator
    int authorizedReads = 0;

    [[nodiscard]] bool offersAuthenticateToRead() const noexcept
    {
        return refused && canHelper;
    }
    [[nodiscard]] bool readRefusedForPermission() const noexcept
    {
        return refused;
    }
    void requestAuthorizedRead() noexcept
    {
        ++authorizedReads;
    }
};

class RestartAsAdministratorButtonTest : public ::testing::Test
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

    static constexpr const char* WINDOW = "Section";

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST(RestartAsAdministratorWordingTest, SaysWhatTheChipsNoticeSays)
{
    EXPECT_TRUE(std::string_view(RestartAsAdministrator::LABEL).ends_with(AuthorizeWording::WINDOWS_WORDING.switchSession));
    EXPECT_STREQ(AuthorizeWording::WINDOWS_WORDING.switchSession, "Restart as administrator...");
    EXPECT_TRUE(std::string_view(RestartAsAdministrator::TOOLTIP).contains("UAC")) << "names the prompt it raises";
}

TEST(RestartAsAdministratorWordingTest, OfferedOnlyForAPermissionRefusalWithNoHelperWhereTaskSmackCanRestart)
{
    using RestartAsAdministrator::offersRestart;
    EXPECT_TRUE(offersRestart(/*refusedForPermission=*/true, /*helperCanRedo=*/false, /*restartOffered=*/true));
    EXPECT_FALSE(offersRestart(true, true, true)) << "the helper's Retry or Read as administrator instead";
    EXPECT_FALSE(offersRestart(false, false, true)) << "a failure an administrator couldn't fix";
    EXPECT_FALSE(offersRestart(true, false, false)) << "elevated, Linux, or a synthetic run";
}

TEST(RestartAsAdministratorWordingTest, NotOfferedUntilMainSaysSo)
{
    EXPECT_FALSE(RestartAsAdministrator::offered());
    RestartAsAdministrator::requestRestart(); // raises nothing, and doesn't crash
    {
        const RestartOffered offered;
        EXPECT_TRUE(RestartAsAdministrator::offered());
    }
    EXPECT_FALSE(RestartAsAdministrator::offered());
}

TEST_F(RestartAsAdministratorButtonTest, APressRaisesTheElevationNotice)
{
    const RestartOffered offered;
    const std::string text = frame([] { static_cast<void>(RestartAsAdministrator::drawButton()); });
    EXPECT_TRUE(text.contains("Restart as administrator...")) << text;
    EXPECT_TRUE(offered.raised().empty());

    press(RestartAsAdministrator::LABEL);
    bool pressed = false;
    static_cast<void>(frame([&pressed] { pressed = RestartAsAdministrator::drawButton(); }));
    EXPECT_TRUE(pressed);
    ASSERT_EQ(offered.raised().size(), 1U);
    EXPECT_EQ(offered.raised().front(), Core::EventType::OpenElevationNotice);
    EXPECT_EQ(offered.raised().front(), Core::OpenElevationNoticeEvent::getStaticType());
}

TEST_F(RestartAsAdministratorButtonTest, ARefusedReadWithNoHelperOffersTheRestartInPlaceOfRead)
{
    const RestartOffered offered;
    FakeSection section{.refused = true, .canHelper = false};
    std::string text = frame([&section] { Detail::drawAuthenticateToReadButton(section); });
    EXPECT_TRUE(text.contains("Restart as administrator...")) << text;
    EXPECT_FALSE(text.contains(AuthorizeWording::READ_TEXT)) << text;

    press(RestartAsAdministrator::LABEL);
    static_cast<void>(frame([&section] { Detail::drawAuthenticateToReadButton(section); }));
    ASSERT_EQ(offered.raised().size(), 1U);
    EXPECT_EQ(offered.raised().front(), Core::EventType::OpenElevationNotice);
    EXPECT_EQ(section.authorizedReads, 0) << "the restart, not a read through a helper there isn't";

    // With a helper, its Read as administrator, as before.
    section.canHelper = true;
    text = frame([&section] { Detail::drawAuthenticateToReadButton(section); });
    EXPECT_TRUE(text.contains(AuthorizeWording::READ_TEXT)) << text;
    EXPECT_FALSE(text.contains("Restart as administrator...")) << text;

    // A read that wasn't refused offers neither.
    section = FakeSection{};
    text = frame([&section] { Detail::drawAuthenticateToReadButton(section); });
    EXPECT_FALSE(text.contains("Restart as administrator...")) << text;
    EXPECT_FALSE(text.contains(AuthorizeWording::READ_TEXT)) << text;
}

TEST_F(RestartAsAdministratorButtonTest, ARefusedReadOffersNothingWhereTaskSmackCantRestart)
{
    FakeSection section{.refused = true, .canHelper = false};
    const std::string text = frame([&section] { Detail::drawAuthenticateToReadButton(section); });
    EXPECT_FALSE(text.contains("Restart as administrator...")) << text;
    EXPECT_FALSE(text.contains(AuthorizeWording::READ_TEXT)) << text;
}

} // namespace
} // namespace App
