/// @file test_AccessNoticeOnDisplay.cpp
/// @brief The startup notice in a real Core::Application (#2728): OpenElevationNoticeEvent opens it, and on
/// Linux, Standard or Administrator's Administrator choice holds the helper for the session (a fake
/// broker here, as the real prompt stays a manual check) and closes it, while Continue as Standard
/// closes it holding nothing. Windows still shows the Limited Data notice, which its OK closes. With
/// TASKSMACK_UI_CAPTURE_DIR set, the open notice is saved there as access-notice.png.
///
/// Needs a display with a GL 3.3 core context (Xvfb in Linux CI, where TASKSMACK_REQUIRE_DISPLAY=1
/// turns a missing one into a failure); skipped elsewhere, like UiLayersOnDisplayTest.

#include "App/AccessChoice.h"
#include "App/AccessNoticeDialog.h"
#include "App/ElevationNoticeLayer.h"
#include "App/PanelPlatforms.h"
#include "App/Panels/AuthorizeWording.h"
#include "Core/AnimationRequest.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/EnvRead.h"
#include "Core/FramebufferCapture.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Core/Window.h"
#include "Mocks/FakeBroker.h"
#include "UI/UILayer.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): the modal and its buttons

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

/// A display with a GL 3.3 core context, as UiLayersOnDisplayTest finds one.
bool hasDisplay()
{
#ifdef _WIN32
    const bool available = TestSupport::detectWindowsDisplay();
#else
    // NOLINTBEGIN(concurrency-mt-unsafe) - read-only environment access during single-threaded test setup
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe)
    const bool named = (display != nullptr && *display != '\0') || (waylandDisplay != nullptr && *waylandDisplay != '\0');
    const bool available =
        (named && TestSupport::probeGLCapability()) || (TestSupport::tryEnableOffscreenVideoDriver() && TestSupport::probeGLCapability());
#endif
    return TestSupport::enforceDisplayRequirement(available);
}

constexpr bool LINUX = App::AccessChoice::HOST == App::AccessChoice::AccessPlatform::Linux;

/// What the driver saw.
struct NoticeObserved
{
    bool opened = false;   ///< The notice was up
    bool closed = false;   ///< And then wasn't
    bool captured = false; ///< Saved to TASKSMACK_UI_CAPTURE_DIR
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after the notice's layer: raises the event that opens it, presses @p label once it is up
/// (a footer button, or the filled one when @p filled), and stops the app once it has closed.
class NoticeDriverLayer : public Core::Layer
{
  public:
    NoticeDriverLayer(NoticeObserved& observed, const char* label) : Layer("NoticeDriver"), m_Observed(observed), m_Label(label)
    {}

    void onAttach() override
    {
        ImGui::GetIO().ConfigDebugIgnoreFocusLoss = true; // presses come from here, not the keyboard
    }

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE);
        const int frame = ++m_Observed.frames;
        if (frame > MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        if (frame == SETTLE)
        {
            Core::OpenElevationNoticeEvent open(0);
            Core::Application::get().raiseEvent(open);
            return;
        }
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        const bool up = modal != nullptr && !modal->Hidden;
        if (up && !m_Observed.opened)
        {
            m_Observed.opened = true;
            m_PressAt = frame + PRESS_DELAY;
            m_CaptureNext = true;
        }
        if (up && frame >= m_PressAt && (frame - m_PressAt) % RETRY == 0)
        {
            ImGui::ActivateItemByID(ImHashStr(m_Label, 0, modal->ID)); // again until it takes
        }
        if (m_Observed.opened && modal == nullptr)
        {
            m_Observed.closed = true;
            Core::Application::get().stop();
        }
    }

    // After UILayer has drawn the frame and before it is presented: the open notice, when capturing.
    void onPostRender() override
    {
        if (!std::exchange(m_CaptureNext, false))
        {
            return;
        }
        const char* dir = Core::readEnv("TASKSMACK_UI_CAPTURE_DIR");
        if (dir == nullptr || *dir == '\0')
        {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(Core::utf8ToPath(dir), ec);
        const auto [width, height] = Core::Application::get().getWindow().getSizeInPixels();
        std::vector<std::uint8_t> pixels;
        m_Observed.captured =
            Core::FramebufferCapture::readDefaultFramebuffer(width, height, pixels).has_value() &&
            Core::FramebufferCapture::writePng(Core::utf8ToPath(dir) / "access-notice.png", width, height, pixels).has_value();
    }

  private:
    static constexpr int SETTLE = 10;
    static constexpr int PRESS_DELAY = 5; // let it draw (and be captured) first
    static constexpr int RETRY = 10;
    static constexpr int MAX_FRAMES = 1200;

    NoticeObserved& m_Observed;
    const char* m_Label;
    int m_PressAt = 0;
    bool m_CaptureNext = false;
};

/// Runs the notice in a real application, @p broker standing in for the helper, pressing @p label.
NoticeObserved runNotice(const std::shared_ptr<Mocks::FakeBroker>& broker, const char* label)
{
    App::Detail::setSharedPrivilegedBrokerForTesting(broker);
    Core::ApplicationSpecification spec;
    spec.Name = "AccessNoticeOnDisplayTest";
    spec.VSync = false;
    NoticeObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<App::ElevationNoticeLayer>();
        app.pushLayer<NoticeDriverLayer>(observed, label);
        app.run();
    }
    catch (const std::exception& e)
    {
        ADD_FAILURE() << "Application failed after the display probe passed: " << e.what();
    }
    App::Detail::setSharedPrivilegedBrokerForTesting(nullptr);
    return observed;
}

TEST(AccessNoticeOnDisplayTest, AdministratorHoldsTheHelperForTheSessionAndClosesTheNotice)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    if constexpr (!LINUX)
    {
        GTEST_SKIP() << "Windows' Administrator restarts TaskSmack elevated, which isn't built yet; it keeps the Limited Data notice";
    }
    const auto broker = std::make_shared<Mocks::FakeBroker>(false);
    broker->setHoldsSessions(true);
    const NoticeObserved observed = runNotice(broker, App::AuthorizeWording::LINUX_WORDING.switchSession);
    EXPECT_FALSE(observed.timedOut);
    EXPECT_TRUE(observed.opened);
    EXPECT_TRUE(observed.closed) << "a held session closes the notice";
    EXPECT_TRUE(broker->heldForSession());
    EXPECT_EQ(broker->connectCount(), 1) << "one prompt";
}

TEST(AccessNoticeOnDisplayTest, ContinueAsStandardClosesItHoldingNothing)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    const auto broker = std::make_shared<Mocks::FakeBroker>(false);
    broker->setHoldsSessions(true);
    const char* label = LINUX ? App::AccessNoticeDialog::CONTINUE_LABEL : "OK";
    const NoticeObserved observed = runNotice(broker, label);
    EXPECT_FALSE(observed.timedOut);
    EXPECT_TRUE(observed.opened);
    EXPECT_TRUE(observed.closed);
    EXPECT_FALSE(broker->heldForSession());
    EXPECT_EQ(broker->connectCount(), 0) << "no prompt";
}

} // namespace
