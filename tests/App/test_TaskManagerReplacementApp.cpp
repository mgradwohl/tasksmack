/// @file test_TaskManagerReplacementApp.cpp
/// @brief Replace Task Manager in App (#3072), without ImGui: the command line Windows starts TaskSmack
/// with in Task Manager's place, what a click on the Settings toggle asks for, the rows' and notice's words,
/// and the controller that reads and changes it on a worker.

#include "App/TaskManagerLaunch.h"
#include "App/TaskManagerReplacementController.h"
#include "App/TaskManagerReplacementText.h"
#include "Mocks/FakeTaskManagerReplacement.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/TaskManagerReplacement.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Platform::TaskManagerReplacement::State;

// ---------------------------------------------------------------------------------------------------------
// The command line
// ---------------------------------------------------------------------------------------------------------

struct LaunchCase
{
    std::vector<std::string> arguments;
    bool asTaskManager = false;
};

TEST(TaskManagerLaunchTest, RecognisesTaskManagersPathAsTheFirstArgument)
{
    const std::vector<LaunchCase> cases{
        {.arguments = {}, .asTaskManager = false},
        {.arguments = {R"(C:\WINDOWS\system32\taskmgr.exe)"}, .asTaskManager = true},
        {.arguments = {R"(C:\Windows\System32\Taskmgr.exe)", "/4"}, .asTaskManager = true},
        {.arguments = {R"(C:\Windows\System32\taskmgr.exe)", "/7", "/startup"}, .asTaskManager = true},
        {.arguments = {R"("C:\Windows\System32\taskmgr.exe")"}, .asTaskManager = true},
        {.arguments = {"taskmgr.exe"}, .asTaskManager = true},
        {.arguments = {"TASKMGR"}, .asTaskManager = true},
        {.arguments = {"  taskmgr.exe  "}, .asTaskManager = true},
        {.arguments = {"C:/Windows/System32/taskmgr.exe"}, .asTaskManager = true},
        {.arguments = {"/4", R"(C:\Windows\System32\taskmgr.exe)"}, .asTaskManager = false}, // not first
        {.arguments = {R"(C:\Windows\System32\notepad.exe)"}, .asTaskManager = false},
        {.arguments = {R"(C:\Windows\System32\taskmgr.exe.bak)"}, .asTaskManager = false},
        {.arguments = {R"(C:\Windows\System32\mytaskmgr.exe)"}, .asTaskManager = false},
        {.arguments = {R"(C:\taskmgr.exe\other.exe)"}, .asTaskManager = false},
        {.arguments = {""}, .asTaskManager = false},
        {.arguments = {"\""}, .asTaskManager = false},
        {.arguments = {"--some-flag"}, .asTaskManager = false},
    };
    for (const LaunchCase& c : cases)
    {
        EXPECT_EQ(TaskManagerLaunch::isTaskManagerLaunch(c.arguments), c.asTaskManager)
            << (c.arguments.empty() ? "(none)" : c.arguments.front());
    }
}

// ---------------------------------------------------------------------------------------------------------
// The toggle and the words
// ---------------------------------------------------------------------------------------------------------

TEST(TaskManagerToggleTest, AClickAsksForWhatTheStateCallsFor)
{
    EXPECT_EQ(toggleOutcome(State::Off).request, TaskManagerRequest::TurnOn);
    EXPECT_FALSE(toggleOutcome(State::Off).confirm);
    EXPECT_EQ(toggleOutcome(State::ThisCopy).request, TaskManagerRequest::TurnOff);
    EXPECT_EQ(toggleOutcome(State::OtherTaskSmack).request, TaskManagerRequest::TurnOff);
    EXPECT_EQ(toggleOutcome(State::OtherProgram).request, TaskManagerRequest::None) << "never at once";
    EXPECT_TRUE(toggleOutcome(State::OtherProgram).confirm);

    EXPECT_FALSE(TaskManagerReplacementText::toggleChecked(State::Off));
    EXPECT_TRUE(TaskManagerReplacementText::toggleChecked(State::ThisCopy));
    EXPECT_TRUE(TaskManagerReplacementText::toggleChecked(State::OtherTaskSmack));
    EXPECT_FALSE(TaskManagerReplacementText::toggleChecked(State::OtherProgram));
}

[[nodiscard]] TaskManagerReplacementView viewOf(State state, std::string program = {}, bool exists = true, bool installed = true)
{
    TaskManagerReplacementView view;
    view.supported = true;
    Platform::TaskManagerReplacementInfo info;
    info.status = {.state = state, .program = std::move(program)};
    info.otherProgramExists = exists;
    info.installedCopy = installed;
    info.selfPath = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";
    view.info = std::move(info);
    view.readsCompleted = 1;
    return view;
}

TEST(TaskManagerTextTest, TheStateLineSaysOffOnOrWhatElse)
{
    TaskManagerReplacementView none;
    none.supported = true;
    EXPECT_EQ(TaskManagerReplacementText::stateText(none), "Status: checking...");
    EXPECT_EQ(TaskManagerReplacementText::stateText(viewOf(State::Off)), "Status: Off");
    EXPECT_EQ(TaskManagerReplacementText::stateText(viewOf(State::ThisCopy)), "Status: On (this copy)");
    EXPECT_EQ(TaskManagerReplacementText::stateText(viewOf(State::OtherTaskSmack, R"(D:\Old\TaskSmack.exe)")),
              R"(Status: points to another TaskSmack, D:\Old\TaskSmack.exe)");
    EXPECT_EQ(TaskManagerReplacementText::stateText(viewOf(State::OtherTaskSmack, R"(D:\Old\TaskSmack.exe)", false)),
              R"(Status: points to another TaskSmack, D:\Old\TaskSmack.exe, which is no longer there)");
    EXPECT_EQ(TaskManagerReplacementText::stateText(viewOf(State::OtherProgram, R"(C:\Tools\procexp64.exe)")),
              R"(Status: points to another program, C:\Tools\procexp64.exe)");

    TaskManagerReplacementView failed = viewOf(State::Off);
    failed.info->readFailed = true;
    failed.info->readError = "access denied";
    EXPECT_EQ(TaskManagerReplacementText::stateText(failed), "Status: unknown (access denied)");

    EXPECT_NE(TaskManagerReplacementText::confirmText(R"(C:\Tools\procexp64.exe)").find(R"(C:\Tools\procexp64.exe)"), std::string::npos);
}

TEST(TaskManagerTextTest, TheNoticeShowsOnlyForAnotherTaskSmackThatShouldBeRepaired)
{
    EXPECT_TRUE(TaskManagerReplacementText::showsNotice(viewOf(State::OtherTaskSmack, R"(D:\Gone\TaskSmack.exe)", false, false)));
    EXPECT_TRUE(TaskManagerReplacementText::showsNotice(viewOf(State::OtherTaskSmack, R"(D:\Zip\TaskSmack.exe)", true, true)));
    EXPECT_FALSE(
        TaskManagerReplacementText::showsNotice(viewOf(State::OtherTaskSmack, R"(C:\Program Files\X\TaskSmack.exe)", true, false)));
    EXPECT_FALSE(TaskManagerReplacementText::showsNotice(viewOf(State::Off)));
    EXPECT_FALSE(TaskManagerReplacementText::showsNotice(viewOf(State::ThisCopy)));
    EXPECT_FALSE(TaskManagerReplacementText::showsNotice(viewOf(State::OtherProgram, R"(C:\Tools\procexp64.exe)", false)));
    EXPECT_FALSE(TaskManagerReplacementText::showsNotice(TaskManagerReplacementView{}));

    TaskManagerReplacementView unsupported = viewOf(State::OtherTaskSmack, R"(D:\Gone\TaskSmack.exe)", false);
    unsupported.supported = false;
    EXPECT_FALSE(TaskManagerReplacementText::showsNotice(unsupported));
    TaskManagerReplacementView failed = viewOf(State::OtherTaskSmack, R"(D:\Gone\TaskSmack.exe)", false);
    failed.info->readFailed = true;
    EXPECT_FALSE(TaskManagerReplacementText::showsNotice(failed));

    const std::string gone = TaskManagerReplacementText::noticeText(*viewOf(State::OtherTaskSmack, R"(D:\Gone\TaskSmack.exe)", false).info);
    EXPECT_NE(gone.find(R"(D:\Gone\TaskSmack.exe)"), std::string::npos);
    EXPECT_NE(gone.find("no longer there"), std::string::npos);
    const std::string there = TaskManagerReplacementText::noticeText(*viewOf(State::OtherTaskSmack, R"(D:\Zip\TaskSmack.exe)", true).info);
    EXPECT_EQ(there.find("no longer there"), std::string::npos);
}

// #3276: the restore notice shows when the user had turned it on and the installed copy finds it off.
TEST(TaskManagerTextTest, TheRestoreNoticeShowsForAReplacementTurnedOffSinceItWasTurnedOn)
{
    TaskManagerReplacementView off = viewOf(State::Off);
    EXPECT_EQ(TaskManagerReplacementText::noticeKind(off), NoticeKind::None) << "never turned on";
    off.intended = true;
    EXPECT_TRUE(TaskManagerReplacementText::showsRestoreNotice(off));
    EXPECT_EQ(TaskManagerReplacementText::noticeKind(off), NoticeKind::Restore);

    TaskManagerReplacementView portable = viewOf(State::Off, {}, true, false);
    portable.intended = true;
    EXPECT_EQ(TaskManagerReplacementText::noticeKind(portable), NoticeKind::None) << "the installed copy's to ask";
    for (const State state : {State::ThisCopy, State::OtherProgram})
    {
        TaskManagerReplacementView view = viewOf(state, R"(C:\Tools\procexp64.exe)");
        view.intended = true;
        EXPECT_EQ(TaskManagerReplacementText::noticeKind(view), NoticeKind::None);
    }
    TaskManagerReplacementView gone = viewOf(State::OtherTaskSmack, R"(D:\Gone\TaskSmack.exe)", false);
    gone.intended = true;
    EXPECT_EQ(TaskManagerReplacementText::noticeKind(gone), NoticeKind::Repair) << "the repair notice comes first";

    TaskManagerReplacementView failed = off;
    failed.info->readFailed = true;
    EXPECT_EQ(TaskManagerReplacementText::noticeKind(failed), NoticeKind::None);
    TaskManagerReplacementView unsupported = off;
    unsupported.supported = false;
    EXPECT_EQ(TaskManagerReplacementText::noticeKind(unsupported), NoticeKind::None);
    TaskManagerReplacementView unread;
    unread.supported = true;
    unread.intended = true;
    EXPECT_EQ(TaskManagerReplacementText::noticeKind(unread), NoticeKind::None);
    EXPECT_TRUE(TaskManagerReplacementText::RESTORE_NOTICE_TEXT.contains("turned off"));

    TaskManagerReplacementView unchecked = off;
    unchecked.intentChecked = false; // the uninstall mark not checked yet
    EXPECT_EQ(TaskManagerReplacementText::noticeKind(unchecked), NoticeKind::None);
}

// ---------------------------------------------------------------------------------------------------------
// The controller
// ---------------------------------------------------------------------------------------------------------

using FakeReplacement = Mocks::FakeTaskManagerReplacement;

TEST(TaskManagerControllerTest, AnUnsupportedControllerDoesNothing)
{
    TaskManagerReplacementController controller(nullptr);
    EXPECT_FALSE(controller.view().supported);
    controller.refresh();
    controller.request(TaskManagerRequest::TurnOn);
    controller.poll();
    EXPECT_FALSE(controller.view().busy);
    EXPECT_FALSE(controller.view().info.has_value());
}

TEST(TaskManagerControllerTest, RefreshReadsOnTheWorker)
{
    const auto fake = std::make_shared<FakeReplacement>();
    fake->state = State::OtherTaskSmack;
    fake->program = R"(D:\Old\TaskSmack.exe)";
    TaskManagerReplacementController controller(fake);
    EXPECT_TRUE(controller.view().supported);
    controller.refresh();
    EXPECT_TRUE(controller.view().busy);
    controller.waitForWork();
    EXPECT_FALSE(controller.view().busy);
    ASSERT_TRUE(controller.view().info.has_value());
    EXPECT_EQ(controller.view().info->status.state, State::OtherTaskSmack);
    EXPECT_EQ(controller.view().readsCompleted, 1U);
    EXPECT_FALSE(controller.view().lastChange.has_value());
}

TEST(TaskManagerControllerTest, EachRequestIsItsChangeThenARead)
{
    const auto fake = std::make_shared<FakeReplacement>();
    TaskManagerReplacementController controller(fake);
    struct Expected
    {
        TaskManagerRequest request;
        Platform::Broker::TaskManagerChange change;
        bool replaceOther;
        State after;
    };
    for (const Expected& e : {
             Expected{.request = TaskManagerRequest::TurnOn,
                      .change = Platform::Broker::TaskManagerChange::Set,
                      .replaceOther = false,
                      .after = State::ThisCopy},
             Expected{.request = TaskManagerRequest::TurnOff,
                      .change = Platform::Broker::TaskManagerChange::Clear,
                      .replaceOther = false,
                      .after = State::Off},
             Expected{.request = TaskManagerRequest::TurnOnReplacingOther,
                      .change = Platform::Broker::TaskManagerChange::Set,
                      .replaceOther = true,
                      .after = State::ThisCopy},
             Expected{.request = TaskManagerRequest::Repair,
                      .change = Platform::Broker::TaskManagerChange::Set,
                      .replaceOther = false,
                      .after = State::ThisCopy},
         })
    {
        controller.request(e.request);
        controller.waitForWork();
        ASSERT_FALSE(fake->changes.empty());
        EXPECT_EQ(fake->changes.back().first, e.change);
        EXPECT_EQ(fake->changes.back().second, e.replaceOther);
        ASSERT_TRUE(controller.view().info.has_value());
        EXPECT_EQ(controller.view().info->status.state, e.after);
        ASSERT_TRUE(controller.view().lastChange.has_value());
        EXPECT_TRUE(controller.view().lastChange->success);
    }
    EXPECT_EQ(fake->reads, 4);
}

TEST(TaskManagerControllerTest, AFailedChangeIsShownAndNoneIsIgnored)
{
    const auto fake = std::make_shared<FakeReplacement>();
    fake->answer = Platform::ProcessActionResult::error("The operation was canceled by the user");
    TaskManagerReplacementController controller(fake);
    controller.request(TaskManagerRequest::None);
    EXPECT_FALSE(controller.view().busy);
    controller.request(TaskManagerRequest::TurnOn);
    controller.request(TaskManagerRequest::TurnOff); // ignored: one at a time
    controller.waitForWork();
    ASSERT_EQ(fake->changes.size(), 1U);
    ASSERT_TRUE(controller.view().lastChange.has_value());
    EXPECT_FALSE(controller.view().lastChange->success);
    ASSERT_TRUE(controller.view().info.has_value());
    EXPECT_EQ(controller.view().info->status.state, State::Off);
}

// #3276: what the user turned on is remembered: a read that finds this copy remembers it, Turn off and
// Forget forget it, a read that finds another program forgets it, and Off or another TaskSmack leave it.
TEST(TaskManagerControllerTest, WhatTheUserTurnedOnIsRemembered)
{
    const auto fake = std::make_shared<FakeReplacement>();
    TaskManagerReplacementController controller(fake);
    std::vector<bool> saved;
    controller.rememberIntent(false, [&saved](bool intended) { saved.push_back(intended); });
    EXPECT_FALSE(controller.view().intended);

    controller.refresh();
    controller.waitForWork();
    EXPECT_TRUE(saved.empty()) << "Off leaves it as it was";

    controller.request(TaskManagerRequest::TurnOn);
    controller.waitForWork();
    EXPECT_TRUE(controller.view().intended);
    EXPECT_EQ(saved, std::vector<bool>{true});
    controller.refresh();
    controller.waitForWork();
    EXPECT_EQ(saved, std::vector<bool>{true}) << "saved only when it changes";

    controller.request(TaskManagerRequest::TurnOff);
    EXPECT_FALSE(controller.view().intended) << "at once: what the user asked for, whatever the change does";
    controller.waitForWork();
    EXPECT_EQ(saved, (std::vector<bool>{true, false}));

    fake->state = State::ThisCopy; // turned on by another TaskSmack, or before this was remembered
    controller.refresh();
    controller.waitForWork();
    EXPECT_TRUE(controller.view().intended);
    fake->state = State::OtherTaskSmack;
    controller.refresh();
    controller.waitForWork();
    EXPECT_TRUE(controller.view().intended);
    fake->state = State::OtherProgram; // the user let another tool take it over
    controller.refresh();
    controller.waitForWork();
    EXPECT_FALSE(controller.view().intended);
    EXPECT_EQ(saved, (std::vector<bool>{true, false, true, false}));
}

// #3276: the startup check's word on an uninstall. Until it comes the intent isn't final; an uninstall
// forgets it, and either answer makes it final.
TEST(TaskManagerControllerTest, TheIntentWaitsForTheUninstallCheck)
{
    const auto fake = std::make_shared<FakeReplacement>();
    TaskManagerReplacementController controller(fake);
    std::vector<bool> saved;
    controller.rememberIntent(true, [&saved](bool intended) { saved.push_back(intended); });
    EXPECT_TRUE(controller.view().intentChecked) << "final unless a check is awaited";
    controller.awaitIntentCheck();
    EXPECT_FALSE(controller.view().intentChecked);
    controller.intentChecked(false);
    EXPECT_TRUE(controller.view().intentChecked);
    EXPECT_TRUE(controller.view().intended) << "no uninstall: kept";
    EXPECT_TRUE(saved.empty());

    controller.awaitIntentCheck();
    controller.intentChecked(true);
    EXPECT_TRUE(controller.view().intentChecked);
    EXPECT_FALSE(controller.view().intended) << "uninstalled since: forgotten";
    EXPECT_EQ(saved, std::vector<bool>{false});
    EXPECT_TRUE(fake->changes.empty()) << "nothing changed in the registry";
}

TEST(TaskManagerControllerTest, ForgetChangesNothingButWhatIsRemembered)
{
    const auto fake = std::make_shared<FakeReplacement>();
    TaskManagerReplacementController controller(fake);
    std::vector<bool> saved;
    controller.rememberIntent(true, [&saved](bool intended) { saved.push_back(intended); });
    EXPECT_TRUE(controller.view().intended);
    controller.request(TaskManagerRequest::Forget);
    EXPECT_FALSE(controller.view().busy) << "no change, no read";
    EXPECT_FALSE(controller.view().intended);
    EXPECT_EQ(saved, std::vector<bool>{false});
    EXPECT_TRUE(fake->changes.empty());
    EXPECT_EQ(fake->reads, 0);

    TaskManagerReplacementController unsaved(fake);
    unsaved.rememberIntent(true, {});
    unsaved.request(TaskManagerRequest::Forget);
    EXPECT_FALSE(unsaved.view().intended) << "no save function: still forgotten";

    TaskManagerReplacementController unsupported(nullptr);
    unsupported.rememberIntent(true, [&saved](bool intended) { saved.push_back(intended); });
    unsupported.request(TaskManagerRequest::Forget);
    EXPECT_EQ(saved.size(), 1U) << "an unsupported controller does nothing";
}

} // namespace
} // namespace App
