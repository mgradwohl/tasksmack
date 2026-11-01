/// @file test_TaskManagerReplacementRender.cpp
/// @brief Replace Task Manager's views (#3072), headless: the Settings rows (the toggle and what a click on
/// it asks for in each state, the confirmation before another program's Debugger is replaced, Repair, the
/// portable copy's warning, a copy that can't change it) and the startup notice (Repair, Turn off, Not now,
/// then the result and Close), and the notice layer deciding on the first read.

#include "App/TaskManagerNoticeLayer.h"
#include "App/TaskManagerReplacementController.h"
#include "App/TaskManagerReplacementRows.h"
#include "App/TaskManagerReplacementText.h"
#include "Mocks/FakeTaskManagerReplacement.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/TaskManagerReplacement.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // ImHashStr(), ActivateItemByID(), the windows' list

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace App
{
namespace
{

using Platform::TaskManagerReplacement::State;

[[nodiscard]] TaskManagerReplacementView viewOf(State state, std::string program = {}, bool installed = true)
{
    TaskManagerReplacementView view;
    view.supported = true;
    Platform::TaskManagerReplacementInfo info;
    info.status = {.state = state, .program = std::move(program)};
    info.installedCopy = installed;
    info.selfPath = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";
    view.info = std::move(info);
    view.readsCompleted = 1;
    return view;
}

class TaskManagerReplacementRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // ImGui owns the font atlas
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of the Settings rows in a window of their own; what they asked for, and the rows' ID scope.
    TaskManagerRequest settingsFrame(const TaskManagerReplacementView& view)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Rows", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        m_RowsScope = ImHashStr("TaskManagerReplacement", 0, ImGui::GetCurrentWindow()->ID);
        const TaskManagerRequest request = TaskManagerReplacementRows::renderSettings(view, m_Row, 400.0F);
        m_Drawn = ImGui::GetCurrentWindow()->DC.CursorPos.y;
        ImGui::End();
        ImGui::Render();
        return request;
    }

    /// Clicks @p label among the Settings rows on the next frame, and returns what that frame asked for.
    TaskManagerRequest clickInRows(const TaskManagerReplacementView& view, const char* label)
    {
        static_cast<void>(settingsFrame(view)); // the item exists
        ImGui::ActivateItemByID(ImHashStr(label, 0, m_RowsScope));
        TaskManagerRequest request = settingsFrame(view);
        if (request == TaskManagerRequest::None)
        {
            request = settingsFrame(view);
        }
        return request;
    }

    /// One frame of the notice; what it asked for.
    TaskManagerRequest noticeFrame(const TaskManagerReplacementView& view)
    {
        ImGui::NewFrame();
        const TaskManagerRequest request = TaskManagerReplacementRows::renderNotice(view, m_Notice);
        ImGui::Render();
        return request;
    }

    /// The notice's window, when it was drawn on the last frame.
    [[nodiscard]] static const ImGuiWindow* noticeWindow()
    {
        for (const ImGuiWindow* window : GImGui->Windows)
        {
            if (std::string_view(window->Name).contains("###TaskManagerNotice") && window->Active)
            {
                return window;
            }
        }
        return nullptr;
    }

    /// Clicks @p label in the notice on the next frames, and returns what they asked for.
    TaskManagerRequest clickInNotice(const TaskManagerReplacementView& view, const char* label)
    {
        static_cast<void>(noticeFrame(view));
        const ImGuiWindow* window = noticeWindow();
        EXPECT_NE(window, nullptr);
        if (window == nullptr)
        {
            return TaskManagerRequest::None;
        }
        ImGui::ActivateItemByID(ImHashStr(label, 0, window->ID));
        TaskManagerRequest request = noticeFrame(view);
        if (request == TaskManagerRequest::None)
        {
            request = noticeFrame(view);
        }
        return request;
    }

    TaskManagerReplacementRows::RowState m_Row;
    TaskManagerReplacementRows::NoticeState m_Notice;
    ImGuiID m_RowsScope = 0;
    float m_Drawn = 0.0F;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(TaskManagerReplacementRenderTest, AnUnsupportedViewDrawsNothing)
{
    TaskManagerReplacementView view;
    EXPECT_EQ(settingsFrame(view), TaskManagerRequest::None);
    const float empty = m_Drawn;
    view = viewOf(State::Off);
    static_cast<void>(settingsFrame(view));
    EXPECT_GT(m_Drawn, empty) << "the supported rows take room";
}

TEST_F(TaskManagerReplacementRenderTest, TheToggleTurnsItOnFromOff)
{
    EXPECT_EQ(clickInRows(viewOf(State::Off), TaskManagerReplacementText::TOGGLE_LABEL), TaskManagerRequest::TurnOn);
}

TEST_F(TaskManagerReplacementRenderTest, TheToggleTurnsItOffFromEitherTaskSmack)
{
    EXPECT_EQ(clickInRows(viewOf(State::ThisCopy), TaskManagerReplacementText::TOGGLE_LABEL), TaskManagerRequest::TurnOff);
    EXPECT_EQ(clickInRows(viewOf(State::OtherTaskSmack, R"(D:\Old\TaskSmack.exe)"), TaskManagerReplacementText::TOGGLE_LABEL),
              TaskManagerRequest::TurnOff);
}

TEST_F(TaskManagerReplacementRenderTest, RepairShowsForAnotherTaskSmack)
{
    EXPECT_EQ(clickInRows(viewOf(State::OtherTaskSmack, R"(D:\Old\TaskSmack.exe)"), TaskManagerReplacementText::REPAIR_LABEL),
              TaskManagerRequest::Repair);
}

TEST_F(TaskManagerReplacementRenderTest, AnotherProgramIsReplacedOnlyOnceConfirmed)
{
    const TaskManagerReplacementView view = viewOf(State::OtherProgram, R"(C:\Tools\procexp64.exe)");
    EXPECT_EQ(clickInRows(view, TaskManagerReplacementText::TOGGLE_LABEL), TaskManagerRequest::None) << "asks first";
    EXPECT_TRUE(m_Row.confirmingReplaceOther);

    EXPECT_EQ(clickInRows(view, TaskManagerReplacementText::CANCEL_LABEL), TaskManagerRequest::None);
    EXPECT_FALSE(m_Row.confirmingReplaceOther);

    static_cast<void>(clickInRows(view, TaskManagerReplacementText::TOGGLE_LABEL));
    ASSERT_TRUE(m_Row.confirmingReplaceOther);
    EXPECT_EQ(clickInRows(view, TaskManagerReplacementText::REPLACE_LABEL), TaskManagerRequest::TurnOnReplacingOther);
    EXPECT_FALSE(m_Row.confirmingReplaceOther);
}

TEST_F(TaskManagerReplacementRenderTest, AConfirmationEndsWhenTheStateChangesUnderIt)
{
    m_Row.confirmingReplaceOther = true;
    static_cast<void>(settingsFrame(viewOf(State::Off)));
    EXPECT_FALSE(m_Row.confirmingReplaceOther);
}

TEST_F(TaskManagerReplacementRenderTest, ACopyThatCantChangeItOrIsBusyAsksNothing)
{
    TaskManagerReplacementView unavailable = viewOf(State::Off);
    unavailable.info->unavailableReason = "needs the helper";
    EXPECT_EQ(clickInRows(unavailable, TaskManagerReplacementText::TOGGLE_LABEL), TaskManagerRequest::None);

    TaskManagerReplacementView busy = viewOf(State::Off);
    busy.busy = true;
    EXPECT_EQ(clickInRows(busy, TaskManagerReplacementText::TOGGLE_LABEL), TaskManagerRequest::None);

    TaskManagerReplacementView unread;
    unread.supported = true;
    EXPECT_EQ(clickInRows(unread, TaskManagerReplacementText::TOGGLE_LABEL), TaskManagerRequest::None);

    TaskManagerReplacementView failed = viewOf(State::Off);
    failed.info->readFailed = true;
    failed.info->readError = "access denied";
    EXPECT_EQ(clickInRows(failed, TaskManagerReplacementText::TOGGLE_LABEL), TaskManagerRequest::None);
}

TEST_F(TaskManagerReplacementRenderTest, APortableCopyIsWarnedAndResultsShow)
{
    static_cast<void>(settingsFrame(viewOf(State::ThisCopy, {}, /*installed=*/true)));
    const float installed = m_Drawn;
    TaskManagerReplacementView portable = viewOf(State::ThisCopy, {}, /*installed=*/false);
    static_cast<void>(settingsFrame(portable));
    EXPECT_GT(m_Drawn, installed) << "the warning's line";

    portable.lastChange = Platform::ProcessActionResult::error("The operation was canceled by the user");
    static_cast<void>(settingsFrame(portable));
    const float withResult = m_Drawn;
    portable.lastChange = Platform::ProcessActionResult::ok("TaskSmack now opens in place of Task Manager");
    static_cast<void>(settingsFrame(portable));
    EXPECT_FLOAT_EQ(m_Drawn, withResult);
    portable.busy = true;
    static_cast<void>(settingsFrame(portable));
}

TEST_F(TaskManagerReplacementRenderTest, TheNoticeOffersRepairTurnOffAndNotNow)
{
    const TaskManagerReplacementView view = viewOf(State::OtherTaskSmack, R"(D:\Gone\TaskSmack.exe)");
    EXPECT_EQ(noticeFrame(view), TaskManagerRequest::None);
    EXPECT_EQ(noticeWindow(), nullptr) << "closed until the layer opens it";

    m_Notice.open = true;
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::REPAIR_LABEL), TaskManagerRequest::Repair);
    EXPECT_TRUE(m_Notice.acted);

    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Repair};
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::TURN_OFF_LABEL), TaskManagerRequest::TurnOff);

    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Repair};
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::NOT_NOW_LABEL), TaskManagerRequest::None);
    EXPECT_FALSE(m_Notice.open);
}

TEST_F(TaskManagerReplacementRenderTest, TheNoticeShowsTheResultThenCloses)
{
    TaskManagerReplacementView view = viewOf(State::OtherTaskSmack, R"(D:\Gone\TaskSmack.exe)");
    m_Notice = {.open = true, .acted = true, .kind = NoticeKind::Repair};
    view.busy = true;
    static_cast<void>(noticeFrame(view));
    EXPECT_NE(noticeWindow(), nullptr);
    view.busy = false;
    view.lastChange = Platform::ProcessActionResult::ok("TaskSmack now opens in place of Task Manager");
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::CLOSE_LABEL), TaskManagerRequest::None);
    EXPECT_FALSE(m_Notice.open);
}

TEST_F(TaskManagerReplacementRenderTest, ANoticeForACopyThatCantChangeItOffersNothing)
{
    TaskManagerReplacementView view = viewOf(State::OtherTaskSmack, R"(D:\Gone\TaskSmack.exe)");
    view.info->unavailableReason = "needs the helper";
    m_Notice.open = true;
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::REPAIR_LABEL), TaskManagerRequest::None);
    EXPECT_FALSE(m_Notice.acted);
}

TEST_F(TaskManagerReplacementRenderTest, TheLayerOpensTheNoticeOnTheFirstReadOnly)
{
    const auto fake = std::make_shared<Mocks::FakeTaskManagerReplacement>();
    fake->state = State::OtherTaskSmack;
    fake->program = R"(D:\Gone\TaskSmack.exe)";
    fake->otherProgramExists = false;
    const auto controller = std::make_shared<TaskManagerReplacementController>(fake);
    TaskManagerNoticeLayer layer(controller);
    layer.onAttach();
    controller->waitForWork();
    ImGui::NewFrame();
    layer.onRender();
    ImGui::Render();
    EXPECT_TRUE(layer.noticeState().open);

    // Repair through the layer: the change, then a read, on the controller's worker.
    const ImGuiWindow* window = noticeWindow();
    ASSERT_NE(window, nullptr);
    ImGui::ActivateItemByID(ImHashStr(TaskManagerReplacementText::REPAIR_LABEL, 0, window->ID));
    for (int frame = 0; frame < 2; ++frame)
    {
        ImGui::NewFrame();
        layer.onRender();
        ImGui::Render();
    }
    controller->waitForWork();
    ASSERT_EQ(fake->changes.size(), 1U);
    EXPECT_EQ(fake->changes.front().first, Platform::Broker::TaskManagerChange::Set);
    EXPECT_EQ(controller->view().info->status.state, State::ThisCopy);
}

TEST_F(TaskManagerReplacementRenderTest, TheLayerLeavesTheNoticeClosedWhenAllIsWell)
{
    const auto fake = std::make_shared<Mocks::FakeTaskManagerReplacement>();
    fake->state = State::ThisCopy;
    const auto controller = std::make_shared<TaskManagerReplacementController>(fake);
    TaskManagerNoticeLayer layer(controller);
    layer.onAttach();
    controller->waitForWork();
    ImGui::NewFrame();
    layer.onRender();
    ImGui::Render();
    EXPECT_FALSE(layer.noticeState().open);

    TaskManagerNoticeLayer without(nullptr); // a run with no controller: nothing at all
    without.onAttach();
    ImGui::NewFrame();
    without.onRender();
    ImGui::Render();
    EXPECT_FALSE(without.noticeState().open);
}

// #3276: the restore notice. Repair turns it on; Turn off changes nothing, forgets it and closes the notice,
// even for a copy that can't change it.
TEST_F(TaskManagerReplacementRenderTest, TheRestoreNoticeOffersToTurnItBackOn)
{
    TaskManagerReplacementView view = viewOf(State::Off);
    view.intended = true;
    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Restore};
    static_cast<void>(noticeFrame(view));
    EXPECT_NE(noticeWindow(), nullptr);
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::REPAIR_LABEL), TaskManagerRequest::TurnOn);
    EXPECT_TRUE(m_Notice.acted) << "its result shows, then Close";

    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Restore};
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::TURN_OFF_LABEL), TaskManagerRequest::Forget);
    EXPECT_FALSE(m_Notice.open) << "nothing to wait for";
    EXPECT_FALSE(m_Notice.acted);

    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Restore};
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::NOT_NOW_LABEL), TaskManagerRequest::None);
    EXPECT_FALSE(m_Notice.open);

    view.info->unavailableReason = "needs the helper";
    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Restore};
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::REPAIR_LABEL), TaskManagerRequest::None);
    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Restore};
    EXPECT_EQ(clickInNotice(view, TaskManagerReplacementText::TURN_OFF_LABEL), TaskManagerRequest::Forget) << "it needs no rights";
}

// The layer opens the restore notice on the first read when the user had turned it on and it is off, and its
// Repair turns it back on through the controller; Turn off would only forget it.
TEST_F(TaskManagerReplacementRenderTest, TheLayerOffersToTurnBackOnWhatSomethingTurnedOff)
{
    const auto fake = std::make_shared<Mocks::FakeTaskManagerReplacement>();
    fake->state = State::Off;
    const auto controller = std::make_shared<TaskManagerReplacementController>(fake);
    int saves = 0;
    controller->rememberIntent(true, [&saves](bool /*intended*/) { ++saves; });
    TaskManagerNoticeLayer layer(controller);
    layer.onAttach();
    controller->waitForWork();
    ImGui::NewFrame();
    layer.onRender();
    ImGui::Render();
    EXPECT_TRUE(layer.noticeState().open);
    EXPECT_EQ(layer.noticeState().kind, NoticeKind::Restore);

    const ImGuiWindow* window = noticeWindow();
    ASSERT_NE(window, nullptr);
    ImGui::ActivateItemByID(ImHashStr(TaskManagerReplacementText::REPAIR_LABEL, 0, window->ID));
    for (int frame = 0; frame < 2; ++frame)
    {
        ImGui::NewFrame();
        layer.onRender();
        ImGui::Render();
    }
    controller->waitForWork();
    ASSERT_EQ(fake->changes.size(), 1U);
    EXPECT_EQ(fake->changes.front().first, Platform::Broker::TaskManagerChange::Set);
    EXPECT_FALSE(fake->changes.front().second) << "never over another program's";
    EXPECT_EQ(controller->view().info->status.state, State::ThisCopy);
    EXPECT_TRUE(controller->view().intended);
    EXPECT_EQ(saves, 0) << "it was remembered on all along";
}

// #3276: Turn off forgets only while the last read says it is off and nothing runs. Turned on since (from
// Settings, with the notice still open), it is a real TurnOff, with its checks; while a change runs, nothing.
TEST_F(TaskManagerReplacementRenderTest, TheRestoreNoticesTurnOffTurnsOffWhatWasTurnedOnSince)
{
    TaskManagerReplacementView on = viewOf(State::ThisCopy);
    on.intended = true;
    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Restore};
    EXPECT_EQ(clickInNotice(on, TaskManagerReplacementText::TURN_OFF_LABEL), TaskManagerRequest::TurnOff);
    EXPECT_TRUE(m_Notice.acted) << "its result shows, then Close";

    on.info->unavailableReason = "needs the helper";
    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Restore};
    EXPECT_EQ(clickInNotice(on, TaskManagerReplacementText::TURN_OFF_LABEL), TaskManagerRequest::None) << "TurnOff's own check";

    TaskManagerReplacementView changing = viewOf(State::Off);
    changing.intended = true;
    changing.busy = true; // Settings turning it on right now
    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Restore};
    EXPECT_EQ(clickInNotice(changing, TaskManagerReplacementText::TURN_OFF_LABEL), TaskManagerRequest::None);
    EXPECT_TRUE(m_Notice.open);

    TaskManagerReplacementView failed = viewOf(State::Off);
    failed.intended = true;
    failed.info->readFailed = true;
    m_Notice = {.open = true, .acted = false, .kind = NoticeKind::Restore};
    EXPECT_EQ(clickInNotice(failed, TaskManagerReplacementText::TURN_OFF_LABEL), TaskManagerRequest::TurnOff)
        << "a read that failed doesn't say it is off";
}

// The layer closes a restore notice nobody acted on once there is nothing to restore: turned on from Settings.
TEST_F(TaskManagerReplacementRenderTest, TheLayerClosesTheRestoreNoticeOnceItIsOnAgain)
{
    const auto fake = std::make_shared<Mocks::FakeTaskManagerReplacement>();
    fake->state = State::Off;
    const auto controller = std::make_shared<TaskManagerReplacementController>(fake);
    controller->rememberIntent(true, {});
    TaskManagerNoticeLayer layer(controller);
    layer.onAttach();
    controller->waitForWork();
    ImGui::NewFrame();
    layer.onRender();
    ImGui::Render();
    ASSERT_TRUE(layer.noticeState().open);

    controller->request(TaskManagerRequest::TurnOn); // what Settings' toggle hands it
    controller->waitForWork();
    ImGui::NewFrame();
    layer.onRender();
    ImGui::Render();
    EXPECT_FALSE(layer.noticeState().open);
    EXPECT_EQ(noticeWindow(), nullptr);
}

// The layer decides only once the uninstall mark was checked (#3276): after an uninstall it asks nothing.
TEST_F(TaskManagerReplacementRenderTest, TheLayerWaitsForTheUninstallCheck)
{
    const auto fake = std::make_shared<Mocks::FakeTaskManagerReplacement>();
    fake->state = State::Off;
    const auto controller = std::make_shared<TaskManagerReplacementController>(fake);
    controller->rememberIntent(true, {});
    controller->awaitIntentCheck();
    TaskManagerNoticeLayer layer(controller);
    layer.onAttach();
    controller->waitForWork();
    ImGui::NewFrame();
    layer.onRender();
    ImGui::Render();
    EXPECT_FALSE(layer.noticeState().open) << "not decided yet";

    controller->intentChecked(true); // uninstalled since
    ImGui::NewFrame();
    layer.onRender();
    ImGui::Render();
    EXPECT_FALSE(layer.noticeState().open);
    EXPECT_EQ(layer.noticeState().kind, NoticeKind::None);
}

// A minimized start (#3276): the layer takes the controller's read in while hidden, so what it remembers is
// up to date before anything draws.
TEST_F(TaskManagerReplacementRenderTest, TheLayerTakesTheReadInWhileHidden)
{
    const auto fake = std::make_shared<Mocks::FakeTaskManagerReplacement>();
    fake->state = State::ThisCopy;
    const auto controller = std::make_shared<TaskManagerReplacementController>(fake);
    bool saved = false;
    controller->rememberIntent(false, [&saved](bool intended) { saved = intended; });
    TaskManagerNoticeLayer layer(controller);
    layer.onAttach();
    for (int wake = 0; wake < 200 && controller->view().busy; ++wake)
    {
        layer.onHiddenUpdate();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(controller->view().busy);
    EXPECT_TRUE(saved);
    TaskManagerNoticeLayer without(nullptr);
    without.onHiddenUpdate(); // no controller: nothing
}

TEST_F(TaskManagerReplacementRenderTest, TheLayerAsksNothingWhenItWasNeverTurnedOn)
{
    const auto fake = std::make_shared<Mocks::FakeTaskManagerReplacement>();
    fake->state = State::Off;
    const auto controller = std::make_shared<TaskManagerReplacementController>(fake);
    TaskManagerNoticeLayer layer(controller);
    layer.onAttach();
    controller->waitForWork();
    ImGui::NewFrame();
    layer.onRender();
    ImGui::Render();
    EXPECT_FALSE(layer.noticeState().open);
    EXPECT_EQ(layer.noticeState().kind, NoticeKind::None);
}

} // namespace
} // namespace App
