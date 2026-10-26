/// @file test_SystemdActions.cpp
/// @brief systemd service control (#2006 phase 2). SystemdActions' decisions: what each start type asks
/// systemd for, how a unit's ActiveState answers a waiting start or stop, and the words a refusal is
/// shown in. LinuxServiceActions' capabilities where systemd does and doesn't run. And SystemdBus's
/// startUnit/stopUnit/setUnitStartType against a fake systemd on a private bus (FakeSystemBus.h): a
/// start that runs, one that fails and one that never gets there; a stop refused for a running
/// dependent and one that stops; each start type's unit-file calls; and polkit's refusal.

#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/Linux/LinuxServiceActions.h"
#include "Platform/Linux/SystemdActions.h"
#include "Platform/Linux/SystemdBus.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#ifdef TASKSMACK_HAS_SDBUS
#include "FakeSystemBus.h"

#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include <stdlib.h>                  // NOLINT(modernize-deprecated-headers) - POSIX setenv()/unsetenv(), which <cstdlib> doesn't declare
#include <systemd/sd-bus-protocol.h> // NOLINT(misc-include-cleaner) - sd_bus_message here on newer libsystemd, sd-bus.h on older
#include <systemd/sd-bus.h>
#endif

namespace Platform
{
namespace
{

using SystemdActions::FileChange;
using SystemdActions::Goal;
using SystemdActions::WaitStep;

// ========== SystemdActions: the decisions ==========

TEST(SystemdActionsTest, EachStartTypeAsksForItsUnitFileChange)
{
    // Automatic enables, Manual disables, Disabled masks; moving off Disabled unmasks first (#2006).
    const auto automatic = SystemdActions::startTypeSteps(ServiceStartType::Automatic);
    ASSERT_TRUE(automatic.has_value());
    EXPECT_EQ(automatic.value_or(SystemdActions::StartTypeSteps{}).change, FileChange::Enable);
    EXPECT_TRUE(automatic.value_or(SystemdActions::StartTypeSteps{}).unmaskFirst);

    const auto manual = SystemdActions::startTypeSteps(ServiceStartType::Manual);
    ASSERT_TRUE(manual.has_value());
    EXPECT_EQ(manual.value_or(SystemdActions::StartTypeSteps{}).change, FileChange::Disable);
    EXPECT_TRUE(manual.value_or(SystemdActions::StartTypeSteps{}).unmaskFirst);

    const auto disabled = SystemdActions::startTypeSteps(ServiceStartType::Disabled);
    ASSERT_TRUE(disabled.has_value());
    EXPECT_EQ(disabled.value_or(SystemdActions::StartTypeSteps{}).change, FileChange::Mask);
    EXPECT_FALSE(disabled.value_or(SystemdActions::StartTypeSteps{}).unmaskFirst);

    // No delayed start on systemd, and the read-only types can't be set.
    EXPECT_FALSE(SystemdActions::startTypeSteps(ServiceStartType::AutomaticDelayed).has_value());
    EXPECT_FALSE(SystemdActions::startTypeSteps(ServiceStartType::Boot).has_value());
    EXPECT_FALSE(SystemdActions::startTypeSteps(ServiceStartType::Unknown).has_value());
}

TEST(SystemdActionsTest, AStartWaitsForActiveAndFailsIfItsJobEndsElsewhere)
{
    EXPECT_EQ(SystemdActions::waitStep(Goal::Running, "active", false), WaitStep::Done);
    EXPECT_EQ(SystemdActions::waitStep(Goal::Running, "reloading", true), WaitStep::Done);
    EXPECT_EQ(SystemdActions::waitStep(Goal::Running, "activating", false), WaitStep::Wait);
    EXPECT_EQ(SystemdActions::waitStep(Goal::Running, "activating", true), WaitStep::Wait); // still coming up
    EXPECT_EQ(SystemdActions::waitStep(Goal::Running, "inactive", false), WaitStep::Wait);  // its job hasn't run yet
    EXPECT_EQ(SystemdActions::waitStep(Goal::Running, "inactive", true), WaitStep::Failed); // a condition unmet
    EXPECT_EQ(SystemdActions::waitStep(Goal::Running, "failed", false), WaitStep::Failed);
}

TEST(SystemdActionsTest, AStopIsDoneOnceTheUnitIsNotRunning)
{
    EXPECT_EQ(SystemdActions::waitStep(Goal::Stopped, "inactive", false), WaitStep::Done);
    EXPECT_EQ(SystemdActions::waitStep(Goal::Stopped, "failed", true), WaitStep::Done); // stopped, just not cleanly
    EXPECT_EQ(SystemdActions::waitStep(Goal::Stopped, "deactivating", true), WaitStep::Wait);
    EXPECT_EQ(SystemdActions::waitStep(Goal::Stopped, "active", false), WaitStep::Wait);
    EXPECT_EQ(SystemdActions::waitStep(Goal::Stopped, "active", true), WaitStep::Failed); // its job was dropped
}

TEST(SystemdActionsTest, RunningMeansAStopWouldTakeItDown)
{
    EXPECT_TRUE(SystemdActions::isRunning("active"));
    EXPECT_TRUE(SystemdActions::isRunning("activating"));
    EXPECT_FALSE(SystemdActions::isRunning("inactive"));
    EXPECT_FALSE(SystemdActions::isRunning("failed"));
    EXPECT_FALSE(SystemdActions::isRunning(""));
}

TEST(SystemdActionsTest, RefusalsAfterAllowingAPromptSayWhatHappenedToIt)
{
    using SystemdActions::errorText;
    EXPECT_EQ(errorText(SystemdActions::INTERACTIVE_AUTHORIZATION_REQUIRED, "", false), "Requires administrator");
    EXPECT_EQ(errorText(SystemdActions::INTERACTIVE_AUTHORIZATION_REQUIRED, "", false, true),
              "No authentication agent is running to ask for the password");
    EXPECT_EQ(errorText("org.freedesktop.DBus.Error.AccessDenied", "", false, true), "Authentication was canceled or refused");
    EXPECT_EQ(errorText("org.freedesktop.systemd1.NoSuchUnit", "", false, true), "No such service") << "unrelated errors unchanged";
}

// #2402: a call that let polkit prompt and ran out of time may still be carried out (systemd holds the
// request until the prompt is answered), so it isn't reported as a plain failure.
TEST(SystemdActionsTest, AnInteractiveTimeoutSaysTheActionMayStillHappen)
{
    EXPECT_EQ(SystemdActions::errorText("", "", true, true), "No answer in time; it may still happen if the password prompt is answered");
    EXPECT_EQ(SystemdActions::errorText("", "", true, false), "systemd didn't answer in time");
}

// #2402: an action that may prompt waits as long as pkexec's prompt does; every other call keeps the
// short bound that keeps a hung systemd from holding a worker.
TEST(SystemdActionsTest, OnlyAnActionThatMayPromptGetsTheLongTimeout)
{
    EXPECT_EQ(SystemdBus::actionCallTimeoutUsec(false), SystemdBus::CALL_TIMEOUT_USEC);
    EXPECT_GE(SystemdBus::actionCallTimeoutUsec(true), std::uint64_t{120'000'000});
}

TEST(SystemdActionsTest, RefusalsReadInTheUsersTerms)
{
    // polkit's answer to an unprivileged caller that won't be prompted (#2006), and plain denial.
    EXPECT_EQ(SystemdActions::errorText(
                  "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired", "Interactive authentication required.", false),
              "Requires administrator");
    EXPECT_EQ(SystemdActions::errorText("org.freedesktop.DBus.Error.AccessDenied", "x", false), "Requires administrator");
    EXPECT_EQ(SystemdActions::errorText("org.freedesktop.DBus.Error.ServiceUnknown", "x", false), "systemd isn't running");
    EXPECT_EQ(SystemdActions::errorText("org.freedesktop.systemd1.NoSuchUnit", "x", false), "No such service");
    EXPECT_TRUE(SystemdActions::errorText("org.freedesktop.systemd1.UnitMasked", "x", false).contains("masked"));
    EXPECT_EQ(SystemdActions::errorText("", "", true), "systemd didn't answer in time");
    EXPECT_EQ(SystemdActions::errorText("org.example.Other", "Something broke", false), "Something broke");
    EXPECT_EQ(SystemdActions::errorText("org.example.Other", "", false), "systemd refused it");

    const std::vector<std::string> dependents{"web.service", "api.service"};
    EXPECT_EQ(SystemdActions::dependentsText(dependents), "Stop the services that depend on it first: web.service, api.service");
    EXPECT_EQ(SystemdActions::dependentsText({}), "Other running services depend on it");
}

// ========== LinuxServiceActions: where it can act ==========

TEST(LinuxServiceActionsTest, ActsOnlyWhereSystemdRunsAndNeverOffersADelayedStart)
{
    const TestSupport::ScopedTempDir root("ts_service_actions_root");
    std::filesystem::create_directories(root.path / "run" / "systemd" / "system");
    const LinuxServiceActions withSystemd(root.path, /*built=*/true);
    const ServiceActionCapabilities caps = withSystemd.capabilities();
    EXPECT_TRUE(caps.canStart);
    EXPECT_TRUE(caps.canStop);
    EXPECT_TRUE(caps.canRestart);
    EXPECT_TRUE(caps.canSetStartType);
    EXPECT_FALSE(caps.canDelayStart);
    EXPECT_TRUE(caps.canAuthorizeInteractively); // polkit, through systemd (#2304)

    const TestSupport::ScopedTempDir bare("ts_service_actions_bare");
    LinuxServiceActions withoutSystemd(bare.path, /*built=*/true);
    EXPECT_FALSE(withoutSystemd.capabilities().canStart);
    EXPECT_FALSE(withoutSystemd.capabilities().canSetStartType);
    const std::stop_source stop;
    EXPECT_EQ(withoutSystemd.start("cron.service", stop.get_token()).message, "systemd isn't running");
    EXPECT_EQ(withoutSystemd.setStartType("cron.service", ServiceStartType::Manual).message, "systemd isn't running");
    EXPECT_FALSE(withoutSystemd.capabilities().canAuthorizeInteractively);
    EXPECT_EQ(withoutSystemd.runAuthorized(ServiceAction::Start, "cron.service", ServiceStartType::Unknown, stop.get_token()).message,
              "systemd isn't running");

    LinuxServiceActions notBuilt(root.path, /*built=*/false);
    EXPECT_FALSE(notBuilt.capabilities().canStop);
    EXPECT_FALSE(notBuilt.capabilities().canAuthorizeInteractively);
    EXPECT_FALSE(notBuilt.stop("cron.service", stop.get_token()).ok);
}

TEST(UnsupportedServiceActionsTest, RefusesEveryActionAndOffersNone)
{
    // A synthetic run's, or a build without systemd support's: no capability, every action refused.
    UnsupportedServiceActions actions;
    const ServiceActionCapabilities caps = actions.capabilities();
    EXPECT_FALSE(caps.canStart || caps.canStop || caps.canRestart || caps.canSetStartType);
    const std::stop_source stop;
    for (const ServiceActionResult& result : {actions.start("cron.service", stop.get_token()),
                                              actions.stop("cron.service", stop.get_token()),
                                              actions.restart("cron.service", stop.get_token()),
                                              actions.setStartType("cron.service", ServiceStartType::Manual)})
    {
        EXPECT_FALSE(result.ok);
        EXPECT_FALSE(result.message.empty());
    }
}

#ifdef TASKSMACK_HAS_SDBUS

// ========== SystemdBus against a fake systemd ==========

using ::TestSupport::FakeSystemBus;

constexpr const char* SYSTEMD = "org.freedesktop.systemd1";
constexpr const char* SYSTEMD_PATH = "/org/freedesktop/systemd1";
constexpr const char* MANAGER = "org.freedesktop.systemd1.Manager";
constexpr const char* UNIT = "org.freedesktop.systemd1.Unit";
constexpr const char* SERVICE = "org.freedesktop.systemd1.Service";
constexpr const char* CRON_PATH = "/org/freedesktop/systemd1/unit/cron_2eservice";
constexpr const char* WEB_PATH = "/org/freedesktop/systemd1/unit/web_2eservice";
constexpr const char* JOB_PATH = "/org/freedesktop/systemd1/job/42";
constexpr SystemdBus::UnitWait QUICK{.timeout = std::chrono::milliseconds(300), .poll = std::chrono::milliseconds(10)};

class SystemdActionsFakeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!FakeSystemBus::available())
        {
            GTEST_SKIP() << "dbus-daemon isn't installed";
        }
        // LoadUnit answers each unit's object.
        m_Bus.addRawMethod(SYSTEMD_PATH,
                           MANAGER,
                           "LoadUnit",
                           [](sd_bus_message* call) -> int
                           {
                               const char* name = nullptr;
                               if (sd_bus_message_read(call, "s", &name) < 0 || name == nullptr)
                               {
                                   return sd_bus_reply_method_errorf(call, "org.freedesktop.DBus.Error.InvalidArgs", "bad");
                               }
                               const std::string unit(name);
                               if (unit == "cron.service")
                               {
                                   return sd_bus_reply_method_return(call, "o", CRON_PATH);
                               }
                               if (unit == "web.service")
                               {
                                   return sd_bus_reply_method_return(call, "o", WEB_PATH);
                               }
                               return sd_bus_reply_method_errorf(call, "org.freedesktop.systemd1.NoSuchUnit", "Unit %s not found.", name);
                           });
    }

    /// StartUnit or StopUnit (ss): records the call and moves cron to @p resultingState, answering a job.
    void serveJob(const char* method, const std::string& resultingState)
    {
        m_Bus.addRawMethod(SYSTEMD_PATH,
                           MANAGER,
                           method,
                           // NOLINTNEXTLINE(bugprone-exception-escape) - a test fake: an allocation failure here fails the test
                           [this, method, resultingState](sd_bus_message* call) -> int
                           {
                               const char* name = nullptr;
                               const char* mode = nullptr;
                               if (sd_bus_message_read(call, "ss", &name, &mode) < 0)
                               {
                                   return sd_bus_reply_method_errorf(call, "org.freedesktop.DBus.Error.InvalidArgs", "bad");
                               }
                               record(std::string(method) + " " + (name != nullptr ? name : "") + " " + (mode != nullptr ? mode : ""));
                               m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = resultingState;
                               return sd_bus_reply_method_return(call, "o", JOB_PATH);
                           });
    }

    /// StartUnit for a Type=oneshot cron (#2123): it runs and is back to inactive by the time the job
    /// is gone. @p ran moves its main process start time on (it ran); @p result is the run's Result.
    void serveOneshotStart(bool ran, const std::string& result)
    {
        m_Bus.properties()[CRON_PATH][SERVICE]["Type"] = std::string("oneshot");
        m_Bus.properties()[CRON_PATH][SERVICE]["Result"] = std::string("success"); // from an earlier run
        m_Bus.properties()[CRON_PATH][SERVICE]["ExecMainStartTimestampMonotonic"] = std::uint64_t{1000};
        m_Bus.addRawMethod(SYSTEMD_PATH,
                           MANAGER,
                           "StartUnit",
                           // NOLINTNEXTLINE(bugprone-exception-escape) - a test fake: an allocation failure here fails the test
                           [this, ran, result](sd_bus_message* call) -> int
                           {
                               record("StartUnit");
                               m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
                               if (ran)
                               {
                                   m_Bus.properties()[CRON_PATH][SERVICE]["ExecMainStartTimestampMonotonic"] = std::uint64_t{2000};
                                   m_Bus.properties()[CRON_PATH][SERVICE]["Result"] = result;
                               }
                               return sd_bus_reply_method_return(call, "o", JOB_PATH);
                           });
    }

    /// A unit-file call (as, b... ): records it, and answers as systemd does (EnableUnitFiles: b a(sss)).
    /// @p carriesInstallInfo is EnableUnitFiles' answer: false for a unit file without an [Install] section.
    void serveUnitFileCall(const char* method, bool carriesInstallInfo = true)
    {
        m_Bus.addRawMethod(SYSTEMD_PATH,
                           MANAGER,
                           method,
                           // NOLINTNEXTLINE(bugprone-exception-escape) - as above
                           [this, method, carriesInstallInfo](sd_bus_message* call) -> int
                           {
                               char** files = nullptr;
                               if (sd_bus_message_read_strv(call, &files) < 0 || files == nullptr || files[0] == nullptr)
                               {
                                   return sd_bus_reply_method_errorf(call, "org.freedesktop.DBus.Error.InvalidArgs", "bad");
                               }
                               record(std::string(method) + " " + files[0]);
                               // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) - a C strv
                               for (char* const* entry = files; *entry != nullptr; ++entry)
                               {
                                   std::free(*entry); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
                               }
                               std::free(static_cast<void*>(files)); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
                               sd_bus_message* reply = nullptr;
                               int result = sd_bus_message_new_method_return(call, &reply);
                               if (result >= 0 && std::string_view(method) == "EnableUnitFiles")
                               {
                                   result = sd_bus_message_append(reply, "b", carriesInstallInfo ? 1 : 0);
                               }
                               if (result >= 0)
                               {
                                   result = sd_bus_message_append(reply, "a(sss)", 0);
                               }
                               if (result >= 0)
                               {
                                   result = sd_bus_send(nullptr, reply, nullptr);
                               }
                               sd_bus_message_unref(reply);
                               return result >= 0 ? 1 : result;
                           });
    }

    void record(std::string call)
    {
        const std::scoped_lock lock(m_CallsMutex);
        m_Calls.push_back(std::move(call));
    }

    [[nodiscard]] std::vector<std::string> calls()
    {
        const std::scoped_lock lock(m_CallsMutex);
        return m_Calls;
    }

    FakeSystemBus m_Bus;

  private:
    std::mutex m_CallsMutex;
    std::vector<std::string> m_Calls;
};

TEST_F(SystemdActionsFakeTest, AStartIsSentInReplaceModeAndWaitsUntilRunning)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
    serveJob("StartUnit", "active");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_EQ(calls(), (std::vector<std::string>{"StartUnit cron.service replace"}));
}

// #2728: tasksmack-helper holds one connection, opened before its sandbox, and every action borrows it --
// without closing it, so the second action still has it.
TEST_F(SystemdActionsFakeTest, ActionsBorrowTheHeldConnectionAndLeaveItOpen)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
    serveJob("StartUnit", "active");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    ASSERT_EQ(SystemdBus::holdSystemBusForActions(), std::nullopt);
    EXPECT_EQ(SystemdBus::holdSystemBusForActions(), std::nullopt) << "holding again keeps the one it has";
    const std::stop_source stop;
    const ServiceActionResult first = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    const ServiceActionResult second = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    SystemdBus::releaseHeldSystemBusForTesting();
    EXPECT_TRUE(first.ok) << first.message;
    EXPECT_TRUE(second.ok) << second.message;
    EXPECT_EQ(calls(), (std::vector<std::string>{"StartUnit cron.service replace", "StartUnit cron.service replace"}));
}

// #2123: a oneshot service that ran and succeeded is inactive again when its job is gone: that's a start.
TEST_F(SystemdActionsFakeTest, AOneshotThatRanSuccessfullyStarted)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
    serveOneshotStart(/*ran=*/true, "success");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_TRUE(result.ok) << result.message;
}

TEST_F(SystemdActionsFakeTest, AOneshotWhoseRunFailedDidNotStart)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
    serveOneshotStart(/*ran=*/true, "exit-code");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "It didn't start (inactive)");
}

// A condition unmet or a dependency failed: nothing ran, and the Result left from an earlier run counts
// for nothing.
TEST_F(SystemdActionsFakeTest, AOneshotThatNeverRanDidNotStart)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
    serveOneshotStart(/*ran=*/false, "success");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "It didn't start (inactive)");
}

TEST(SystemdActionsTest, OnlyAOneshotRunThatStartedAndSucceededCounts)
{
    using SystemdActions::oneshotRanSuccessfully;
    EXPECT_TRUE(oneshotRanSuccessfully("oneshot", "success", 1, 2));
    EXPECT_FALSE(oneshotRanSuccessfully("oneshot", "success", 2, 2));
    EXPECT_FALSE(oneshotRanSuccessfully("oneshot", "exit-code", 1, 2));
    EXPECT_FALSE(oneshotRanSuccessfully("simple", "success", 1, 2));
}

TEST_F(SystemdActionsFakeTest, AStartWhoseUnitEndsUpNotRunningSaysSo)
{
    // The job ran (its object is gone) and the unit is still inactive: a condition unmet, or it exited.
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
    serveJob("StartUnit", "inactive");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "It didn't start (inactive)");
}

TEST_F(SystemdActionsFakeTest, AStartThatNeverGetsThereGivesUpAfterTheWait)
{
    // Still activating, and its job still there, when the wait runs out.
    m_Bus.properties()[JOB_PATH]["org.freedesktop.systemd1.Job"]["State"] = std::string("running");
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
    serveJob("StartUnit", "activating");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::startUnit(
        "cron.service", stop.get_token(), {.timeout = std::chrono::milliseconds(150), .poll = std::chrono::milliseconds(10)});
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.message.starts_with("It didn't start within")) << result.message;
}

TEST_F(SystemdActionsFakeTest, AStopRequestedBeforehandSendsNothing)
{
    serveJob("StartUnit", "active");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    std::stop_source stop;
    stop.request_stop();
    const ServiceActionResult result = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_TRUE(result.cancelled);
    EXPECT_TRUE(calls().empty());
}

TEST_F(SystemdActionsFakeTest, AnUnprivilegedStartIsRefusedAsRequiringAdministrator)
{
    // polkit's answer when it would have to prompt and the caller doesn't allow it (#2006).
    m_Bus.addRawMethod(SYSTEMD_PATH,
                       MANAGER,
                       "StartUnit",
                       [](sd_bus_message* call) -> int
                       {
                           return sd_bus_reply_method_errorf(
                               call, "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired", "Interactive authentication required.");
                       });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "Requires administrator");
}

// #2304: the plain call never lets polkit prompt, and its refusal says a prompt could allow it; the
// user's retry lets it prompt, and its refusals say what happened to the prompt.
TEST_F(SystemdActionsFakeTest, OnlyTheAuthorizedRetryLetsPolkitPrompt)
{
    m_Bus.addRawMethod(SYSTEMD_PATH,
                       MANAGER,
                       "StartUnit",
                       [](sd_bus_message* call) -> int
                       {
                           // polkit with a password needed: refused without the flag; with it, the user
                           // dismissed the prompt.
                           return sd_bus_message_get_allow_interactive_authorization(call) > 0
                                    ? sd_bus_reply_method_errorf(call, "org.freedesktop.DBus.Error.AccessDenied", "Access denied")
                                    : sd_bus_reply_method_errorf(call,
                                                                 "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired",
                                                                 "Interactive authentication required.");
                       });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;

    const ServiceActionResult plain = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_FALSE(plain.ok);
    EXPECT_EQ(plain.message, "Requires administrator");
    EXPECT_TRUE(plain.needsAuthorization);
    EXPECT_FALSE(plain.authorizationUnavailable);

    const ServiceActionResult retried = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK, /*allowInteractive=*/true);
    EXPECT_FALSE(retried.ok);
    EXPECT_EQ(retried.message, "Authentication was canceled or refused");
    EXPECT_FALSE(retried.needsAuthorization);
    EXPECT_FALSE(retried.authorizationUnavailable);
}

// #2402: systemd answers an action allowed to prompt only once the password is entered. One answered
// after the 2 s read bound still succeeds.
TEST_F(SystemdActionsFakeTest, AnAuthorizedRetryWaitsPastTheReadTimeoutForThePrompt)
{
    m_Bus.addRawMethod(SYSTEMD_PATH,
                       MANAGER,
                       "StopUnit",
                       // NOLINTNEXTLINE(bugprone-exception-escape) - a test fake: an allocation failure here fails the test
                       [this](sd_bus_message* call) -> int
                       {
                           std::this_thread::sleep_for(std::chrono::microseconds(SystemdBus::CALL_TIMEOUT_USEC) +
                                                       std::chrono::milliseconds(500)); // the user typing
                           record("StopUnit");
                           m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
                           return sd_bus_reply_method_return(call, "o", JOB_PATH);
                       });
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("active");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::stopUnit("cron.service", stop.get_token(), QUICK, /*allowInteractive=*/true);
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_EQ(calls(), (std::vector<std::string>{"StopUnit"}));
}

// #2402: closing the panel or exiting while the prompt is up ends the wait at once, not after the
// prompt's long bound.
TEST_F(SystemdActionsFakeTest, AStopRequestEndsTheWaitOnThePrompt)
{
    m_Bus.addRawMethod(SYSTEMD_PATH,
                       MANAGER,
                       "StartUnit",
                       [](sd_bus_message* call) -> int
                       {
                           std::this_thread::sleep_for(std::chrono::seconds(2)); // the prompt still up
                           return sd_bus_reply_method_return(call, "o", JOB_PATH);
                       });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    std::stop_source stop;
    const std::jthread stopper(
        [&stop]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            (void) stop.request_stop();
        });
    const auto started = std::chrono::steady_clock::now();
    const ServiceActionResult result = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK, /*allowInteractive=*/true);
    const auto waited = std::chrono::steady_clock::now() - started;
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled) << result.message;
    EXPECT_LT(waited, std::chrono::milliseconds(1500));
}

TEST_F(SystemdActionsFakeTest, AnAuthorizedRetryWithNoAgentToPromptSaysSo)
{
    // No polkit agent in the session: still "interactive authorization required", even when allowed.
    m_Bus.addRawMethod(SYSTEMD_PATH,
                       MANAGER,
                       "StartUnit",
                       [](sd_bus_message* call) -> int
                       {
                           return sd_bus_reply_method_errorf(
                               call, "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired", "Interactive authentication required.");
                       });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult retried = SystemdBus::startUnit("cron.service", stop.get_token(), QUICK, /*allowInteractive=*/true);
    EXPECT_EQ(retried.message, "No authentication agent is running to ask for the password");
    EXPECT_TRUE(retried.authorizationUnavailable);
    EXPECT_FALSE(retried.needsAuthorization);
}

TEST_F(SystemdActionsFakeTest, AnUnknownServiceIsNoSuchService)
{
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    EXPECT_EQ(SystemdBus::startUnit("nope.service", stop.get_token(), QUICK).message, "No such service");
}

TEST_F(SystemdActionsFakeTest, AStopWithARunningDependentIsRefusedNamingIt)
{
    // web.service Requires= cron and is running: systemd would stop it too, so nothing is sent (#2006).
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("active");
    m_Bus.properties()[CRON_PATH][UNIT]["RequiredBy"] = std::vector<std::string>{"web.service"};
    m_Bus.properties()[CRON_PATH][UNIT]["BoundBy"] = std::vector<std::string>{};
    m_Bus.properties()[WEB_PATH][UNIT]["ActiveState"] = std::string("active");
    serveJob("StopUnit", "inactive");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::stopUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "Stop the services that depend on it first: web.service");
    EXPECT_TRUE(calls().empty());
}

// #2124: systemd also stops the units that are PartOf= cron (its ConsistsOf) and those that are
// Requisite= of it (its RequisiteOf), so a running one of those refuses the stop as well.
TEST_F(SystemdActionsFakeTest, AStopWithARunningPartOfMemberIsRefusedNamingIt)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("active");
    m_Bus.properties()[CRON_PATH][UNIT]["ConsistsOf"] = std::vector<std::string>{"web.service"};
    m_Bus.properties()[WEB_PATH][UNIT]["ActiveState"] = std::string("active");
    serveJob("StopUnit", "inactive");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::stopUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "Stop the services that depend on it first: web.service");
    EXPECT_TRUE(calls().empty());
}

TEST_F(SystemdActionsFakeTest, AStopWithARunningRequisiteDependentIsRefusedNamingIt)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("active");
    m_Bus.properties()[CRON_PATH][UNIT]["RequisiteOf"] = std::vector<std::string>{"web.service"};
    m_Bus.properties()[WEB_PATH][UNIT]["ActiveState"] = std::string("active");
    serveJob("StopUnit", "inactive");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::stopUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "Stop the services that depend on it first: web.service");
    EXPECT_TRUE(calls().empty());
}

TEST_F(SystemdActionsFakeTest, AStopWithOnlyStoppedDependentsStopsIt)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("active");
    m_Bus.properties()[CRON_PATH][UNIT]["RequiredBy"] = std::vector<std::string>{"web.service"};
    m_Bus.properties()[CRON_PATH][UNIT]["BoundBy"] = std::vector<std::string>{};
    m_Bus.properties()[WEB_PATH][UNIT]["ActiveState"] = std::string("inactive");
    serveJob("StopUnit", "inactive");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::stopUnit("cron.service", stop.get_token(), QUICK);
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_EQ(calls(), (std::vector<std::string>{"StopUnit cron.service replace"}));
}

TEST_F(SystemdActionsFakeTest, EachStartTypeSendsItsUnitFileCallsThenReloads)
{
    for (const char* method : {"UnmaskUnitFiles", "EnableUnitFiles", "DisableUnitFiles", "MaskUnitFiles"})
    {
        serveUnitFileCall(method);
    }
    m_Bus.addRawMethod(SYSTEMD_PATH,
                       MANAGER,
                       "Reload",
                       [this](sd_bus_message* call) -> int
                       {
                           record("Reload");
                           return sd_bus_reply_method_return(call, "");
                       });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();

    EXPECT_TRUE(SystemdBus::setUnitStartType("cron.service", ServiceStartType::Automatic).ok);
    EXPECT_EQ(calls(), (std::vector<std::string>{"UnmaskUnitFiles cron.service", "EnableUnitFiles cron.service", "Reload"}));

    EXPECT_TRUE(SystemdBus::setUnitStartType("cron.service", ServiceStartType::Manual).ok);
    EXPECT_TRUE(SystemdBus::setUnitStartType("cron.service", ServiceStartType::Disabled).ok);
    const std::vector<std::string> all = calls();
    const std::vector<std::string> expected{"UnmaskUnitFiles cron.service",
                                            "EnableUnitFiles cron.service",
                                            "Reload",
                                            "UnmaskUnitFiles cron.service",
                                            "DisableUnitFiles cron.service",
                                            "Reload",
                                            "MaskUnitFiles cron.service",
                                            "Reload"};
    EXPECT_EQ(all, expected);

    // No delayed start on systemd: refused before anything is sent.
    const ServiceActionResult delayed = SystemdBus::setUnitStartType("cron.service", ServiceStartType::AutomaticDelayed);
    EXPECT_FALSE(delayed.ok);
    EXPECT_EQ(calls().size(), expected.size());
}

TEST_F(SystemdActionsFakeTest, AStopOfAnUnknownServiceIsNoSuchService)
{
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    EXPECT_EQ(SystemdBus::stopUnit("nope.service", stop.get_token(), QUICK).message, "No such service");
}

TEST_F(SystemdActionsFakeTest, AUnitWhoseStateCantBeReadDidntStart)
{
    // web.service's object answers no ActiveState, and its start job is done: what it ended up as is
    // unknown, so the start isn't reported as a success.
    serveJob("StartUnit", "active"); // moves cron, not web
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    const ServiceActionResult result = SystemdBus::startUnit("web.service", stop.get_token(), QUICK);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "It didn't start (state unknown)");
}

TEST_F(SystemdActionsFakeTest, AStopRequestedWhileWaitingEndsTheWait)
{
    // Still activating, its job still queued: a stop request (TaskSmack closing) ends the wait at the
    // next poll, as a cancellation, without waiting out the timeout.
    m_Bus.properties()[JOB_PATH]["org.freedesktop.systemd1.Job"]["State"] = std::string("running");
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
    serveJob("StartUnit", "activating");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    std::stop_source stop;
    std::thread canceller(
        [&stop]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            stop.request_stop();
        });
    const ServiceActionResult result = SystemdBus::startUnit(
        "cron.service", stop.get_token(), {.timeout = std::chrono::seconds(10), .poll = std::chrono::milliseconds(10)});
    canceller.join();
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(calls(), (std::vector<std::string>{"StartUnit cron.service replace"})); // the request already sent stands
}

// #2472: LinuxServiceActions' "Authenticate and retry" sends each step allowing polkit to prompt, a restart as
// a stop then a start.
TEST_F(SystemdActionsFakeTest, AnAuthorizedActionLetsEachOfItsStepsPrompt)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
    for (const char* method : {"StartUnit", "StopUnit"})
    {
        const std::string state = std::string_view(method) == "StartUnit" ? "active" : "inactive";
        m_Bus.addRawMethod(SYSTEMD_PATH,
                           MANAGER,
                           method,
                           // NOLINTNEXTLINE(bugprone-exception-escape) - a test fake: an allocation failure here fails the test
                           [this, method, state](sd_bus_message* call) -> int
                           {
                               record(std::string(method) +
                                      (sd_bus_message_get_allow_interactive_authorization(call) > 0 ? " prompt" : ""));
                               m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = state;
                               return sd_bus_reply_method_return(call, "o", JOB_PATH);
                           });
    }
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const TestSupport::ScopedTempDir root("ts_service_actions_authorized");
    std::filesystem::create_directories(root.path / "run" / "systemd" / "system");
    LinuxServiceActions actions(root.path, /*built=*/true, QUICK);
    const std::stop_source stop;

    EXPECT_TRUE(actions.runAuthorized(ServiceAction::Start, "cron.service", ServiceStartType::Unknown, stop.get_token()).ok);
    EXPECT_TRUE(actions.runAuthorized(ServiceAction::Restart, "cron.service", ServiceStartType::Unknown, stop.get_token()).ok);
    EXPECT_TRUE(actions.runAuthorized(ServiceAction::Stop, "cron.service", ServiceStartType::Unknown, stop.get_token()).ok);
    EXPECT_EQ(calls(), (std::vector<std::string>{"StartUnit prompt", "StopUnit prompt", "StartUnit prompt", "StopUnit prompt"}));
}

// #2472: TaskSmack closing between a restart's stop and its start skips the start, plain or authorized.
TEST_F(SystemdActionsFakeTest, AStopRequestAfterARestartsStopSkipsItsStart)
{
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("active");
    std::stop_source stop;
    m_Bus.addRawMethod(SYSTEMD_PATH,
                       MANAGER,
                       "StopUnit",
                       // NOLINTNEXTLINE(bugprone-exception-escape) - a test fake: an allocation failure here fails the test
                       [this, &stop](sd_bus_message* call) -> int
                       {
                           record("StopUnit");
                           m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("inactive");
                           stop.request_stop(); // TaskSmack closing while the stop is answered
                           return sd_bus_reply_method_return(call, "o", JOB_PATH);
                       });
    serveJob("StartUnit", "active");
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const TestSupport::ScopedTempDir root("ts_service_actions_restart_stop");
    std::filesystem::create_directories(root.path / "run" / "systemd" / "system");
    LinuxServiceActions actions(root.path, /*built=*/true, QUICK);

    const ServiceActionResult plain = actions.restart("cron.service", stop.get_token());
    EXPECT_TRUE(plain.cancelled) << plain.message;

    stop = std::stop_source{};
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("active");
    const ServiceActionResult authorized =
        actions.runAuthorized(ServiceAction::Restart, "cron.service", ServiceStartType::Unknown, stop.get_token());
    EXPECT_TRUE(authorized.cancelled) << authorized.message;
    EXPECT_EQ(calls(), (std::vector<std::string>{"StopUnit", "StopUnit"})) << "no start after either";
}

TEST_F(SystemdActionsFakeTest, ALoadUnitAnswerThatIsntAnObjectPathFailsTheAction)
{
    m_Bus.addRawMethod(
        SYSTEMD_PATH, MANAGER, "LoadUnit", [](sd_bus_message* call) -> int { return sd_bus_reply_method_return(call, "s", "not a path"); });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();
    const std::stop_source stop;
    EXPECT_EQ(SystemdBus::startUnit("cron.service", stop.get_token(), QUICK).message, "systemd's answer couldn't be read");
}

TEST_F(SystemdActionsFakeTest, EachRefusedUnitFileStepStopsTheChange)
{
    // Unmask refused: Automatic stops there. Mask refused: Disabled fails. Reload refused: the change
    // was made but systemd wasn't told, which is reported too.
    const auto refuse = [](sd_bus_message* call) -> int
    {
        return sd_bus_reply_method_errorf(
            call, "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired", "Interactive authentication required.");
    };
    m_Bus.addRawMethod(SYSTEMD_PATH, MANAGER, "UnmaskUnitFiles", refuse);
    m_Bus.addRawMethod(SYSTEMD_PATH, MANAGER, "MaskUnitFiles", refuse);
    serveUnitFileCall("DisableUnitFiles");
    m_Bus.addRawMethod(SYSTEMD_PATH, MANAGER, "Reload", refuse);
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();

    EXPECT_EQ(SystemdBus::setUnitStartType("cron.service", ServiceStartType::Automatic).message, "Requires administrator");
    EXPECT_EQ(SystemdBus::setUnitStartType("cron.service", ServiceStartType::Disabled).message, "Requires administrator");
    EXPECT_TRUE(calls().empty()) << "nothing past the refused step";
}

// #2146: Automatic on a unit file without an [Install] section (static, generated, transient) enables
// nothing (EnableUnitFiles answers carries_install_info=false): a failure that says why, not a success,
// and no Reload after it.
TEST_F(SystemdActionsFakeTest, AutomaticOnAUnitWithoutAnInstallSectionSaysWhy)
{
    serveUnitFileCall("UnmaskUnitFiles");
    serveUnitFileCall("EnableUnitFiles", /*carriesInstallInfo=*/false);
    m_Bus.addRawMethod(SYSTEMD_PATH,
                       MANAGER,
                       "Reload",
                       [this](sd_bus_message* call) -> int
                       {
                           record("Reload");
                           return sd_bus_reply_method_return(call, "");
                       });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();

    const ServiceActionResult automatic = SystemdBus::setUnitStartType("cron.service", ServiceStartType::Automatic);
    EXPECT_FALSE(automatic.ok);
    EXPECT_EQ(automatic.message, SystemdActions::NO_INSTALL_SECTION_MESSAGE);
    // The unmask changed the unit files, so systemd is reloaded anyway (#2426).
    EXPECT_EQ(calls(), (std::vector<std::string>{"UnmaskUnitFiles cron.service", "EnableUnitFiles cron.service", "Reload"}));
}

// #1891: the unit files changed but systemd refused the Reload that tells it so: reported as a
// failure, after both unit-file calls went through.
TEST_F(SystemdActionsFakeTest, ARefusedReloadAfterTheChangeIsReported)
{
    serveUnitFileCall("UnmaskUnitFiles");
    serveUnitFileCall("DisableUnitFiles");
    m_Bus.addRawMethod(SYSTEMD_PATH,
                       MANAGER,
                       "Reload",
                       [](sd_bus_message* call) -> int
                       {
                           return sd_bus_reply_method_errorf(
                               call, "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired", "Interactive authentication required.");
                       });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();

    const ServiceActionResult manual = SystemdBus::setUnitStartType("cron.service", ServiceStartType::Manual);
    EXPECT_FALSE(manual.ok);
    EXPECT_EQ(manual.message, "Requires administrator");
    EXPECT_EQ(calls(), (std::vector<std::string>{"UnmaskUnitFiles cron.service", "DisableUnitFiles cron.service"}));
}

TEST_F(SystemdActionsFakeTest, LinuxServiceActionsActThroughSystemdWhereItRuns)
{
    // With a root where systemd runs, each action goes to systemd; restart is stop, then start.
    m_Bus.properties()[CRON_PATH][UNIT]["ActiveState"] = std::string("active");
    m_Bus.properties()[CRON_PATH][UNIT]["RequiredBy"] = std::vector<std::string>{};
    m_Bus.properties()[CRON_PATH][UNIT]["BoundBy"] = std::vector<std::string>{};
    serveJob("StopUnit", "inactive");
    serveJob("StartUnit", "active");
    serveUnitFileCall("UnmaskUnitFiles");
    serveUnitFileCall("DisableUnitFiles");
    m_Bus.addRawMethod(SYSTEMD_PATH, MANAGER, "Reload", [](sd_bus_message* call) -> int { return sd_bus_reply_method_return(call, ""); });
    ASSERT_TRUE(m_Bus.start(SYSTEMD)) << m_Bus.error();

    const TestSupport::ScopedTempDir root("ts_service_actions_live");
    std::filesystem::create_directories(root.path / "run" / "systemd" / "system");
    LinuxServiceActions actions(root.path, /*built=*/true, QUICK);
    const std::stop_source stop;
    EXPECT_TRUE(actions.stop("cron.service", stop.get_token()).ok);
    EXPECT_TRUE(actions.start("cron.service", stop.get_token()).ok);
    EXPECT_TRUE(actions.restart("cron.service", stop.get_token()).ok);
    EXPECT_TRUE(actions.setStartType("cron.service", ServiceStartType::Manual).ok);
    const std::vector<std::string> expected{"StopUnit cron.service replace",
                                            "StartUnit cron.service replace",
                                            "StopUnit cron.service replace",
                                            "StartUnit cron.service replace",
                                            "UnmaskUnitFiles cron.service",
                                            "DisableUnitFiles cron.service"};
    EXPECT_EQ(calls(), expected);

    // A stop request before the restart begins: nothing is sent, and it reports the cancellation.
    std::stop_source stopNow;
    stopNow.request_stop();
    EXPECT_TRUE(actions.restart("cron.service", stopNow.get_token()).cancelled);
    EXPECT_EQ(calls().size(), expected.size());
}

TEST(SystemdActionsNoBusTest, WithoutASystemBusEveryActionSaysSo)
{
    const char* saved = std::getenv("DBUS_SYSTEM_BUS_ADDRESS"); // NOLINT(concurrency-mt-unsafe) - single-threaded test
    const std::optional<std::string> savedAddress = saved != nullptr ? std::optional<std::string>(saved) : std::nullopt;
    ASSERT_EQ(setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/tasksmack-test-bus", 1), 0); // NOLINT(concurrency-mt-unsafe)

    const std::string unreachable = "The system bus couldn't be reached";
    const std::stop_source stop;
    EXPECT_TRUE(SystemdBus::startUnit("cron.service", stop.get_token(), QUICK).message.starts_with(unreachable));
    EXPECT_TRUE(SystemdBus::stopUnit("cron.service", stop.get_token(), QUICK).message.starts_with(unreachable));
    EXPECT_TRUE(SystemdBus::setUnitStartType("cron.service", ServiceStartType::Manual).message.starts_with(unreachable));

    if (savedAddress.has_value())
    {
        setenv("DBUS_SYSTEM_BUS_ADDRESS", savedAddress->c_str(), 1); // NOLINT(concurrency-mt-unsafe)
    }
    else
    {
        unsetenv("DBUS_SYSTEM_BUS_ADDRESS"); // NOLINT(concurrency-mt-unsafe)
    }
}

#endif // TASKSMACK_HAS_SDBUS

} // namespace
} // namespace Platform
