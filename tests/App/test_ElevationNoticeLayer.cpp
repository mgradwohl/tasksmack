/// @file test_ElevationNoticeLayer.cpp
/// @brief The startup notice's layer, headless (#2728): on Windows its Restart as administrator... runs the restart
/// on a worker, closes TaskSmack only once Windows has started the elevated copy, and stays Standard, saying so,
/// when the prompt is declined or the restart fails. The restart is a fake: no UAC prompt is ever raised.

#include "App/AccessChoice.h"
#include "App/AccessNoticeDialog.h"
#include "App/ElevationNoticeLayer.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Platform/IElevatedRestart.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): the modal and its buttons

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace App
{
namespace
{

using namespace std::chrono_literals;

/// Answers restart() with a set result, and records what it was asked.
class FakeRestart final : public Platform::IElevatedRestart
{
  public:
    FakeRestart(Platform::AdministratorPrompt prompt, Platform::ElevatedRestartResult answer)
        : m_Prompt(prompt), m_Answer(std::move(answer))
    {}

    [[nodiscard]] Platform::AdministratorPrompt prompt() const noexcept override
    {
        return m_Prompt;
    }

    [[nodiscard]] Platform::ElevatedRestartResult restart(const std::filesystem::path& configDir) override
    {
        const std::scoped_lock lock(m_Mutex);
        m_ConfigDir = configDir;
        ++m_Calls;
        return m_Answer;
    }

    [[nodiscard]] int calls() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Calls;
    }

    [[nodiscard]] std::filesystem::path configDir() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_ConfigDir;
    }

  private:
    Platform::AdministratorPrompt m_Prompt;
    Platform::ElevatedRestartResult m_Answer;
    mutable std::mutex m_Mutex;
    std::filesystem::path m_ConfigDir;
    int m_Calls = 0;
};

class ElevationNoticeLayerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if constexpr (AccessChoice::HOST != AccessChoice::AccessPlatform::Windows)
        {
            GTEST_SKIP() << "Restart as administrator is Windows' (a Linux session is started as Administrator, #3580)";
        }
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 900.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        m_Layer.reset(); // waits for a restart still on its worker
        if (m_Context != nullptr)
        {
            ImGui::DestroyContext(m_Context);
        }
    }

    void makeLayer(std::shared_ptr<Platform::IElevatedRestart> restart)
    {
        m_Layer = std::make_unique<ElevationNoticeLayer>(std::move(restart), [this] { ++m_Closes; });
    }

    /// One frame, drawing the layer as the app does; what it drew.
    std::string frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        ImGui::LogToBuffer();
        m_Layer->onRender();
        std::string text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        m_Open = ImGui::IsPopupOpen(AccessNoticeDialog::POPUP_ID); // inside the frame, which IsPopupOpen() needs
        ImGui::Render();
        return text;
    }

    void openNotice()
    {
        Core::OpenElevationNoticeEvent open(0);
        m_Layer->onEvent(open);
        for (int i = 0; i < SETTLE_FRAMES; ++i)
        {
            static_cast<void>(frame());
        }
        ASSERT_TRUE(m_Open);
    }

    static void pressRestart()
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr(AccessNoticeDialog::RESTART_LABEL, 0, modal->ID));
    }

    /// Frames until @p done(the frame's text), or a few seconds; the last frame's text.
    template<typename Done> std::string framesUntil(Done done)
    {
        std::string text;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            text = frame();
            if (done(text))
            {
                return text;
            }
            std::this_thread::sleep_for(5ms);
        }
        ADD_FAILURE() << "timed out";
        return text;
    }

    static constexpr int SETTLE_FRAMES = 6;
    std::unique_ptr<ElevationNoticeLayer> m_Layer;
    std::atomic<int> m_Closes{0};
    bool m_Open = false; ///< Whether the notice was up at the end of the last frame

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ElevationNoticeLayerTest, ApprovedRestartClosesTaskSmackWithTheLaunchingUsersConfigDirectory)
{
    const auto restart = std::make_shared<FakeRestart>(
        Platform::AdministratorPrompt::Consent,
        Platform::ElevatedRestartResult{.outcome = Platform::ElevatedRestartResult::Outcome::Started, .detail = {}});
    makeLayer(restart);
    openNotice();
    EXPECT_EQ(restart->calls(), 0) << "TaskSmack never restarts on its own";
    pressRestart();
    static_cast<void>(framesUntil([this](const std::string& /*text*/) { return m_Closes.load() > 0; }));
    EXPECT_EQ(restart->calls(), 1);
    EXPECT_EQ(m_Closes.load(), 1);
    EXPECT_EQ(restart->configDir(), UserConfig::get().configPath().parent_path()) << "this user's settings go with it";
}

TEST_F(ElevationNoticeLayerTest, ADeclinedPromptStaysStandardAndSaysSo)
{
    const auto restart = std::make_shared<FakeRestart>(
        Platform::AdministratorPrompt::Credentials,
        Platform::ElevatedRestartResult{.outcome = Platform::ElevatedRestartResult::Outcome::Declined, .detail = {}});
    makeLayer(restart);
    openNotice();
    pressRestart();
    const std::string text = framesUntil([](const std::string& drawn) { return drawn.contains("didn't restart TaskSmack"); });
    EXPECT_EQ(m_Closes.load(), 0) << "nothing closes";
    EXPECT_TRUE(m_Open);
    EXPECT_TRUE(text.contains("didn't restart TaskSmack")) << text;
    EXPECT_TRUE(text.contains("name and password")) << "a standard user's prompt: " << text;
}

TEST_F(ElevationNoticeLayerTest, AFailedRestartStaysStandardAndSaysWhy)
{
    const auto restart = std::make_shared<FakeRestart>(
        Platform::AdministratorPrompt::Consent,
        Platform::ElevatedRestartResult{.outcome = Platform::ElevatedRestartResult::Outcome::Failed, .detail = "no process"});
    makeLayer(restart);
    openNotice();
    pressRestart();
    const std::string text = framesUntil([](const std::string& drawn) { return drawn.contains("no process"); });
    EXPECT_EQ(m_Closes.load(), 0);
    EXPECT_TRUE(text.contains("no process")) << text;
}

TEST_F(ElevationNoticeLayerTest, WithNoRestartTheButtonDoesNothing)
{
    makeLayer(nullptr); // a synthetic run
    openNotice();
    pressRestart();
    std::string text;
    for (int i = 0; i < SETTLE_FRAMES; ++i)
    {
        text = frame();
    }
    EXPECT_EQ(m_Closes.load(), 0);
    EXPECT_TRUE(text.contains("can't restart itself")) << text;
}

} // namespace
} // namespace App
