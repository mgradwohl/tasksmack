/// @file test_LogindSessions.cpp
/// @brief The Users tab's sessions on Linux (#3076): logind's ListSessions and each session's properties, read
/// against a fake logind on a private bus (FakeSystemBus.h), the session TaskSmack runs in marked, and Lock and
/// Sign out (LockSession, TerminateSession) with polkit's refusal when no password prompt is allowed.

#include "Platform/IUserSessions.h"
#include "Platform/Linux/LinuxUserSessions.h"
#include "Platform/Linux/SystemdBus.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#ifdef TASKSMACK_HAS_SDBUS

#include "FakeSystemBus.h"

#include <mutex>
#include <ranges>
#include <utility>

#include <stdlib.h>                  // NOLINT(modernize-deprecated-headers) - POSIX setenv()/unsetenv(), which <cstdlib> doesn't declare
#include <systemd/sd-bus-protocol.h> // NOLINT(misc-include-cleaner) - sd_bus_message here on newer libsystemd, sd-bus.h on older
#include <systemd/sd-bus.h>

namespace Platform
{
namespace
{

using TestSupport::FakeSystemBus;

constexpr const char* LOGIN1 = "org.freedesktop.login1";
constexpr const char* LOGIN1_PATH = "/org/freedesktop/login1";
constexpr const char* MANAGER = "org.freedesktop.login1.Manager";
constexpr const char* SESSION = "org.freedesktop.login1.Session";
constexpr const char* DESKTOP_PATH = "/org/freedesktop/login1/session/_32";
constexpr const char* SSH_PATH = "/org/freedesktop/login1/session/_35";

class LogindSessionsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!FakeSystemBus::available())
        {
            GTEST_SKIP() << "dbus-daemon isn't installed";
        }
    }

    /// Two sessions: alice's Wayland desktop on seat0 (active), and bob's ssh login from 10.0.0.9 (idle).
    void serveLogind()
    {
        m_Bus.addRawMethod(LOGIN1_PATH,
                           MANAGER,
                           "ListSessions",
                           [](sd_bus_message* call) -> int
                           {
                               sd_bus_message* reply = nullptr;
                               int result = sd_bus_message_new_method_return(call, &reply);
                               if (result >= 0)
                               {
                                   result = sd_bus_message_append(
                                       reply, "a(susso)", 2, "2", 1000U, "alice", "seat0", DESKTOP_PATH, "5", 1001U, "bob", "", SSH_PATH);
                               }
                               if (result >= 0)
                               {
                                   result = sd_bus_send(nullptr, reply, nullptr);
                               }
                               sd_bus_message_unref(reply);
                               return result;
                           });
        auto& props = m_Bus.properties();
        props[DESKTOP_PATH][SESSION] = {{"TTY", std::string("tty2")},
                                        {"Type", std::string("wayland")},
                                        {"Class", std::string("user")},
                                        {"State", std::string("active")},
                                        {"Service", std::string("gdm-password")},
                                        {"RemoteHost", std::string()},
                                        {"Remote", false},
                                        {"IdleHint", false},
                                        {"IdleSinceHint", std::uint64_t{0}},
                                        {"Leader", std::uint32_t{1234}}};
        props[SSH_PATH][SESSION] = {{"TTY", std::string("pts/0")},
                                    {"Type", std::string("tty")},
                                    {"Class", std::string("user")},
                                    {"State", std::string("online")},
                                    {"Service", std::string("sshd")},
                                    {"RemoteHost", std::string("10.0.0.9")},
                                    {"Remote", true},
                                    {"IdleHint", true},
                                    {"IdleSinceHint", std::uint64_t{1'700'000'000'000'000}},
                                    {"Leader", std::uint32_t{5678}}};
        for (const char* method : {"LockSession", "TerminateSession"})
        {
            m_Bus.addRawMethod(
                LOGIN1_PATH,
                MANAGER,
                method,
                // NOLINTNEXTLINE(bugprone-exception-escape) - a test fake: an allocation failure here fails the test
                [this, method](sd_bus_message* call) -> int
                {
                    const char* id = nullptr;
                    if (sd_bus_message_read(call, "s", &id) < 0 || id == nullptr)
                    {
                        return sd_bus_reply_method_errorf(call, "org.freedesktop.DBus.Error.InvalidArgs", "bad");
                    }
                    const std::string session(id);
                    // Another user's session needs an administrator: polkit asks, if the call allows it.
                    if (session == "5" && sd_bus_message_get_allow_interactive_authorization(call) <= 0)
                    {
                        return sd_bus_reply_method_errorf(
                            call, "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired", "Interactive authentication required.");
                    }
                    if (session != "2" && session != "5")
                    {
                        return sd_bus_reply_method_errorf(call, "org.freedesktop.login1.NoSuchSession", "No session '%s' known", id);
                    }
                    {
                        const std::scoped_lock lock(m_Mutex);
                        m_Calls.push_back(std::string(method) + " " + session);
                    }
                    return sd_bus_reply_method_return(call, "");
                });
        }
    }

    [[nodiscard]] std::vector<std::string> calls()
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Calls;
    }

    FakeSystemBus m_Bus;
    std::mutex m_Mutex;
    std::vector<std::string> m_Calls;
};

TEST_F(LogindSessionsTest, EverySessionIsReadWithItsPropertiesAndTheCurrentOneMarked)
{
    serveLogind();
    ASSERT_TRUE(m_Bus.start(LOGIN1)) << m_Bus.error();

    const SystemdBus::LoginSessionsRead read = SystemdBus::readLoginSessions("2");
    ASSERT_TRUE(read.ok) << read.error;
    ASSERT_EQ(read.sessions.size(), 2U);
    const UserSession& desktop = read.sessions[0];
    EXPECT_EQ(desktop.id, "2");
    EXPECT_EQ(desktop.uid, 1000U);
    EXPECT_EQ(desktop.user, "alice");
    EXPECT_EQ(desktop.seat, "seat0");
    EXPECT_EQ(desktop.tty, "tty2");
    EXPECT_EQ(desktop.type, "wayland");
    EXPECT_EQ(desktop.sessionClass, "user");
    EXPECT_EQ(desktop.state, "active");
    EXPECT_EQ(desktop.service, "gdm-password");
    EXPECT_FALSE(desktop.remote);
    EXPECT_FALSE(desktop.idle);
    EXPECT_EQ(desktop.idleSinceUsec, 0U);
    EXPECT_EQ(desktop.leaderPid, 1234);
    EXPECT_TRUE(desktop.current);

    const UserSession& ssh = read.sessions[1];
    EXPECT_EQ(ssh.id, "5");
    EXPECT_EQ(ssh.user, "bob");
    EXPECT_EQ(ssh.seat, "");
    EXPECT_EQ(ssh.tty, "pts/0");
    EXPECT_TRUE(ssh.remote);
    EXPECT_EQ(ssh.remoteHost, "10.0.0.9");
    EXPECT_TRUE(ssh.idle);
    EXPECT_EQ(ssh.idleSinceUsec, 1'700'000'000'000'000U);
    EXPECT_FALSE(ssh.current);
}

TEST_F(LogindSessionsTest, LockAndSignOutGoToLogindAndAnotherUsersSessionNeedsTheRetry)
{
    serveLogind();
    ASSERT_TRUE(m_Bus.start(LOGIN1)) << m_Bus.error();
    const std::stop_source stop;

    EXPECT_TRUE(SystemdBus::sessionAction("2", SessionAction::Lock, false, stop.get_token()).ok);
    // bob's session: refused without a prompt, with the retry offered; allowed to prompt, it goes through.
    const ServiceActionResult refused = SystemdBus::sessionAction("5", SessionAction::SignOut, false, stop.get_token());
    EXPECT_FALSE(refused.ok);
    EXPECT_TRUE(refused.needsAuthorization);
    EXPECT_EQ(refused.message, "Requires administrator");
    EXPECT_TRUE(SystemdBus::sessionAction("5", SessionAction::SignOut, true, stop.get_token()).ok);
    // A session that has gone since the list was read.
    const ServiceActionResult gone = SystemdBus::sessionAction("9", SessionAction::Lock, false, stop.get_token());
    EXPECT_FALSE(gone.ok);
    EXPECT_FALSE(gone.needsAuthorization);
    EXPECT_EQ(calls(), (std::vector<std::string>{"LockSession 2", "TerminateSession 5"}));
}

TEST_F(LogindSessionsTest, TheLinuxSessionsReadThroughTheBusAndSayWhyWhenLogindIsAway)
{
    serveLogind();
    ASSERT_TRUE(m_Bus.start(LOGIN1)) << m_Bus.error();
    setenv("XDG_SESSION_ID", "5", 1); // NOLINT(concurrency-mt-unsafe) - before the sessions object reads it
    LinuxUserSessions sessions;
    unsetenv("XDG_SESSION_ID"); // NOLINT(concurrency-mt-unsafe)
    EXPECT_TRUE(sessions.hasSessions());
    EXPECT_TRUE(sessions.canAct(SessionAction::Lock));
    const UserSessionsRead read = sessions.read();
    ASSERT_EQ(read.status, UserSessionsStatus::Ok) << read.detail;
    ASSERT_EQ(read.sessions.size(), 2U);
    EXPECT_FALSE(read.sessions[0].current);
    EXPECT_TRUE(read.sessions[1].current);
    EXPECT_TRUE(sessions.act("2", SessionAction::SignOut, false, {}).ok);
}

TEST_F(LogindSessionsTest, ABusWithoutLogindIsAFailedReadThatSaysSo)
{
    ASSERT_TRUE(m_Bus.start("")) << m_Bus.error(); // nobody serves login1
    LinuxUserSessions sessions;
    const UserSessionsRead read = sessions.read();
    EXPECT_EQ(read.status, UserSessionsStatus::Failed);
    EXPECT_EQ(read.detail, "systemd-logind isn't running");
    EXPECT_FALSE(sessions.act("2", SessionAction::Lock, false, {}).ok);
}

} // namespace
} // namespace Platform

#endif

namespace Platform
{
namespace
{

TEST(UserSessionsTest, AnUnsupportedRunListsAndDoesNothing)
{
    UnsupportedUserSessions sessions;
    EXPECT_FALSE(sessions.hasSessions());
    EXPECT_EQ(sessions.read().status, UserSessionsStatus::Unsupported);
    EXPECT_FALSE(sessions.canAct(SessionAction::SignOut));
    const ServiceActionResult result = sessions.act("2", SessionAction::Lock, false, {});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "Not supported on this platform");
}

} // namespace
} // namespace Platform
