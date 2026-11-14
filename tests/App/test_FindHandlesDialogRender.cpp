/// @file test_FindHandlesDialogRender.cpp
/// @brief The Find handles dialog on its own, headless (#3067, #3374): Stop ends a search that is still reading, before
/// its other processes are read; and a search some processes refused offers "Search them as administrator", which
/// reads only those, through the reader's authorized read, and lists what it finds. Fake readers: nothing is read
/// from this machine and no password is asked for.

#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/FindHandlesDialog.h"
#include "Domain/HandleSearch.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID()
#include <implot.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using namespace std::chrono_literals;

constexpr int MAX_FRAMES = 500;

/// Reads one open file per process, but holds the first read until release(): a search that is still reading.
class HeldOpenFiles final : public Platform::IProcessOpenFilesReader
{
  public:
    [[nodiscard]] bool hasOpenFiles() const override
    {
        return true;
    }

    [[nodiscard]] Platform::OpenFilesReadResult readOpenFiles(const Platform::ProcessTarget& /*target*/) override
    {
        {
            std::unique_lock lock(m_Mutex);
            ++m_Reads;
            m_Changed.notify_all();
            // Bounded, so a test that fails before it releases can't hang the suite.
            static_cast<void>(m_Changed.wait_for(lock, 10s, [this] { return m_Released; }));
        }
        Platform::OpenFilesReadResult result;
        result.status = Platform::OpenFilesReadStatus::Ok;
        Platform::OpenFile open;
        open.kind = Platform::OpenFileKind::File;
        open.path = "/home/me/report.ods";
        result.files.push_back(std::move(open));
        return result;
    }

    /// Waits until a read has begun.
    bool waitForRead()
    {
        std::unique_lock lock(m_Mutex);
        return m_Changed.wait_for(lock, 10s, [this] { return m_Reads > 0; });
    }

    void release()
    {
        const std::scoped_lock lock(m_Mutex);
        m_Released = true;
        m_Changed.notify_all();
    }

    [[nodiscard]] int reads()
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Reads;
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    int m_Reads = 0;
    bool m_Released = false;
};

class FindHandlesDialogRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        m_PlotContext = ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        m_Dialog.reset(); // its worker joins before the contexts go
        ImPlot::DestroyContext(m_PlotContext);
        ImGui::DestroyContext(m_Context);
    }

    /// One frame with the dialog drawn over a window of its own, and the text it drew.
    std::string frame()
    {
        ImGui::NewFrame();
        ImGui::Begin("##Host", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        static_cast<void>(m_Dialog->render([this] { return m_Processes; }));
        std::string text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return text;
    }

    /// Opens the dialog, types @p query and presses Enter, as a user does.
    void search(const char* query)
    {
        m_Dialog->open();
        static_cast<void>(frame());
        static_cast<void>(frame());
        ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr) << "the dialog didn't open";
        ImGui::GetIO().AddInputCharactersUTF8(query);
        static_cast<void>(frame());
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
        static_cast<void>(frame());
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    }

    /// Clicks the button labelled @p label in the dialog.
    void click(const char* label)
    {
        const ImGuiWindow* dialog = ImGui::GetTopMostPopupModal();
        ASSERT_NE(dialog, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, dialog->ID));
        static_cast<void>(frame());
    }

    /// Frames until the text drawn contains @p wanted, and that text.
    std::string frameUntil(const std::string& wanted)
    {
        std::string text;
        for (int i = 0; i < MAX_FRAMES; ++i)
        {
            text = frame();
            if (text.contains(wanted))
            {
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        return text;
    }

    std::unique_ptr<FindHandlesDialog> m_Dialog = std::make_unique<FindHandlesDialog>();
    std::vector<Domain::HandleSearch::SearchProcess> m_Processes{
        {.target = {.pid = 100, .startTimeTicks = 1}, .name = "soffice"},
        {.target = {.pid = 200, .startTimeTicks = 2}, .name = "evince"},
    };

  private:
    ImGuiContext* m_Context = nullptr;
    ImPlotContext* m_PlotContext = nullptr;
};

// Stop while the first process is being read: the walk ends before the second is read, the status line says how far
// it got, and Find is offered again.
TEST_F(FindHandlesDialogRenderTest, StopEndsASearchBeforeItsOtherProcessesAreRead)
{
    auto reader = std::make_unique<HeldOpenFiles>();
    HeldOpenFiles& held = *reader;
    m_Dialog->setReaders(std::move(reader), nullptr, {.openFiles = true, .modules = false, .authorize = false});

    search("report");
    ASSERT_TRUE(held.waitForRead()) << "the search never began reading";
    const std::string running = frameUntil("Stop");
    ASSERT_TRUE(running.contains("Searching... 0 of 2 processes")) << running;
    EXPECT_TRUE(m_Dialog->searching());

    click("Stop");
    held.release();
    const std::string text = frameUntil("processes searched");
    EXPECT_TRUE(text.contains("1 processes searched")) << text;
    EXPECT_FALSE(text.contains("Stop")) << "Find is offered again: " << text;
    EXPECT_EQ(held.reads(), 1) << "the second process isn't read after Stop";
    EXPECT_FALSE(m_Dialog->searching());
}

// A process the OS refused, with a reader that can read as an administrator: the status line counts it and offers the
// administrator's search, which reads only that process, authorized, and lists what it held.
TEST_F(FindHandlesDialogRenderTest, ARefusedProcessIsSearchedAgainAsAnAdministrator)
{
    auto files = std::make_unique<TestMocks::MockProcessOpenFilesReader>();
    Platform::OpenFilesReadResult refused;
    refused.status = Platform::OpenFilesReadStatus::PermissionDenied;
    files->setResult(refused);
    Platform::OpenFilesReadResult granted;
    granted.status = Platform::OpenFilesReadStatus::Ok;
    Platform::OpenFile open;
    open.kind = Platform::OpenFileKind::File;
    open.path = "/srv/data/report.db";
    granted.files.push_back(std::move(open));
    files->setAuthorizedResult(std::move(granted));
    TestMocks::MockProcessOpenFilesReader& mock = *files;
    m_Processes.resize(1);
    const FindHandlesDialog::Capabilities capabilities = FindHandlesDialog::capabilitiesOf(files.get(), nullptr);
    ASSERT_TRUE(capabilities.authorize);
    m_Dialog->setReaders(std::move(files), nullptr, capabilities);

    search("report");
    const std::string refusedText = frameUntil("couldn't be read");
    EXPECT_TRUE(refusedText.contains("1 processes searched \xC2\xB7 0 found \xC2\xB7 1 couldn't be read")) << refusedText;
    ASSERT_TRUE(refusedText.contains(AuthorizeWording::SEARCH_LABEL)) << refusedText;
    EXPECT_EQ(mock.authorizedReadCount(), 0) << "nothing is read as an administrator until asked";

    click(AuthorizeWording::SEARCH_LABEL);
    const std::string text = frameUntil("/srv/data/report.db");
    EXPECT_TRUE(text.contains("/srv/data/report.db")) << text;
    EXPECT_TRUE(text.contains("soffice")) << text;
    EXPECT_EQ(mock.authorizedReadCount(), 1);
    EXPECT_EQ(mock.readCount(), 1) << "the administrator's search reads only what was refused, authorized";
}

} // namespace
} // namespace App
