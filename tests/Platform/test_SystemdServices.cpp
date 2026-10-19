/// @file test_SystemdServices.cpp
/// @brief The Services tab on Linux (#2006): SystemdServices' mapping of systemd's units and unit files
/// to ServiceInfos, LinuxServiceProbe over an injected read, and the real read where systemd runs.

#include "Platform/IServiceProbe.h"
#include "Platform/Linux/LinuxServiceProbe.h"
#include "Platform/Linux/SystemdActions.h"
#include "Platform/Linux/SystemdBus.h"
#include "Platform/Linux/SystemdServices.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

using TestSupport::ScopedTempDir;

[[nodiscard]] const ServiceInfo* find(const std::vector<ServiceInfo>& services, const std::string& name)
{
    const auto it = std::ranges::find(services, name, &ServiceInfo::name);
    return it == services.end() ? nullptr : &*it;
}

TEST(SystemdServicesTest, ActiveStatesAreServiceStates)
{
    EXPECT_EQ(SystemdServices::stateFrom("active"), ServiceState::Running);
    EXPECT_EQ(SystemdServices::stateFrom("reloading"), ServiceState::Running);
    EXPECT_EQ(SystemdServices::stateFrom("activating"), ServiceState::StartPending);
    EXPECT_EQ(SystemdServices::stateFrom("deactivating"), ServiceState::StopPending);
    EXPECT_EQ(SystemdServices::stateFrom("inactive"), ServiceState::Stopped);
    EXPECT_EQ(SystemdServices::stateFrom("failed"), ServiceState::Stopped);
    EXPECT_EQ(SystemdServices::stateFrom("something-new"), ServiceState::Unknown);
}

TEST(SystemdServicesTest, UnitFileStatesAreStartTypes)
{
    EXPECT_EQ(SystemdServices::startTypeFrom("enabled"), ServiceStartType::Automatic);
    EXPECT_EQ(SystemdServices::startTypeFrom("enabled-runtime"), ServiceStartType::Automatic);
    EXPECT_EQ(SystemdServices::startTypeFrom("disabled"), ServiceStartType::Manual); // can still be started by hand
    EXPECT_EQ(SystemdServices::startTypeFrom("masked"), ServiceStartType::Disabled);
    EXPECT_EQ(SystemdServices::startTypeFrom("masked-runtime"), ServiceStartType::Disabled);
    EXPECT_EQ(SystemdServices::startTypeFrom("static"), ServiceStartType::Manual);
    EXPECT_EQ(SystemdServices::startTypeFrom("indirect"), ServiceStartType::Manual);
    EXPECT_EQ(SystemdServices::startTypeFrom("generated"), ServiceStartType::Manual);
    EXPECT_EQ(SystemdServices::startTypeFrom("bad"), ServiceStartType::Unknown);
}

// #2114: each start type the Services menu sets reads back as itself once systemd has made the change
// (EnableUnitFiles -> "enabled", DisableUnitFiles -> "disabled", MaskUnitFiles -> "masked").
TEST(SystemdServicesTest, StartTypesRoundTripThroughTheUnitFileStateTheyWrite)
{
    const auto stateAfter = [](SystemdActions::FileChange change) -> std::string_view
    {
        switch (change)
        {
        case SystemdActions::FileChange::Enable:
            return "enabled";
        case SystemdActions::FileChange::Disable:
            return "disabled";
        case SystemdActions::FileChange::Mask:
            return "masked";
        }
        return {};
    };
    for (const auto type : {ServiceStartType::Automatic, ServiceStartType::Manual, ServiceStartType::Disabled})
    {
        const auto steps = SystemdActions::startTypeSteps(type);
        if (!steps.has_value())
        {
            ADD_FAILURE() << "no steps for start type " << static_cast<int>(type);
            continue;
        }
        EXPECT_EQ(SystemdServices::startTypeFrom(stateAfter(steps->change)), type);
    }
}

TEST(SystemdServicesTest, OnlyServicesAndNotTemplatesAreServices)
{
    EXPECT_TRUE(SystemdServices::isService("cron.service"));
    EXPECT_TRUE(SystemdServices::isService("getty@tty1.service")); // An instance
    EXPECT_FALSE(SystemdServices::isService("getty@.service"));    // Its template
    EXPECT_FALSE(SystemdServices::isService("dbus.socket"));
    EXPECT_FALSE(SystemdServices::isService(".service"));
    EXPECT_EQ(SystemdServices::templateOf("getty@tty1.service"), "getty@.service");
    EXPECT_EQ(SystemdServices::templateOf("cron.service"), "cron.service");
}

TEST(SystemdServicesTest, LoadedServicesAndInstalledOnesBecomeTheList)
{
    SystemdServices::Listing listing;
    listing.units = {
        {.name = "cron.service",
         .description = "Regular background program processing daemon",
         .activeState = "active",
         .subState = "running",
         .objectPath = "/org/freedesktop/systemd1/unit/cron_2eservice"},
        {.name = "getty@tty1.service",
         .description = "Getty on tty1",
         .activeState = "active",
         .subState = "running",
         .objectPath = "/u/2"},
        {.name = "apt-daily.service", .description = "", .activeState = "inactive", .subState = "dead", .objectPath = "/u/3"},
        {.name = "dbus.socket", .description = "D-Bus Socket", .activeState = "active", .subState = "running", .objectPath = "/u/4"},
        {.name = "cron.service", .description = "listed twice", .activeState = "failed", .subState = "failed", .objectPath = "/u/5"},
    };
    listing.unitFiles = {
        {"cron.service", "enabled"},
        {"getty@.service", "enabled"}, // The template: its instances take it
        {"apt-daily.service", "static"},
        {"ssh.service", "disabled"}, // Installed, not loaded: listed stopped
        {"masked-thing.service", "masked"},
    };
    listing.mainPids = {{"cron.service", 161}, {"getty@tty1.service", 208}};

    const std::vector<ServiceInfo> services = SystemdServices::services(listing);
    ASSERT_EQ(services.size(), 5U) << "four services and one installed-only, not the socket or the template";
    EXPECT_TRUE(std::ranges::is_sorted(services, {}, &ServiceInfo::name));

    const ServiceInfo* cron = find(services, "cron.service");
    ASSERT_NE(cron, nullptr);
    EXPECT_EQ(cron->displayName, "Regular background program processing daemon");
    EXPECT_EQ(cron->state, ServiceState::Running); // The first listing of it
    EXPECT_EQ(cron->startType, ServiceStartType::Automatic);
    EXPECT_EQ(cron->pid, 161U);

    const ServiceInfo* getty = find(services, "getty@tty1.service");
    ASSERT_NE(getty, nullptr);
    EXPECT_EQ(getty->startType, ServiceStartType::Automatic) << "from its template";
    EXPECT_EQ(getty->pid, 208U);

    const ServiceInfo* aptDaily = find(services, "apt-daily.service");
    ASSERT_NE(aptDaily, nullptr);
    EXPECT_EQ(aptDaily->displayName, "apt-daily.service") << "no description: its name";
    EXPECT_EQ(aptDaily->state, ServiceState::Stopped);
    EXPECT_EQ(aptDaily->startType, ServiceStartType::Manual);
    EXPECT_EQ(aptDaily->pid, 0U);

    const ServiceInfo* ssh = find(services, "ssh.service");
    ASSERT_NE(ssh, nullptr);
    EXPECT_EQ(ssh->state, ServiceState::Stopped);
    EXPECT_EQ(ssh->startType, ServiceStartType::Manual); // disabled: started only by hand
    const ServiceInfo* masked = find(services, "masked-thing.service");
    ASSERT_NE(masked, nullptr);
    EXPECT_EQ(masked->startType, ServiceStartType::Disabled);
    EXPECT_EQ(find(services, "dbus.socket"), nullptr);
    EXPECT_EQ(find(services, "getty@.service"), nullptr);
}

// #2425: an instance can be enabled or not on its own; its own unit file state beats its template's.
TEST(SystemdServicesTest, AnInstancesOwnStartTypeBeatsItsTemplates)
{
    SystemdServices::Listing listing;
    listing.units = {
        {.name = "getty@tty2.service",
         .description = "Getty on tty2",
         .activeState = "active",
         .subState = "running",
         .objectPath = "/u/1"},
        {.name = "wg-quick@wg0.service", .description = "WireGuard", .activeState = "active", .subState = "exited", .objectPath = "/u/2"},
        {.name = "getty@tty1.service",
         .description = "Getty on tty1",
         .activeState = "active",
         .subState = "running",
         .objectPath = "/u/3"},
    };
    listing.unitFiles = {{"getty@.service", "enabled"}, {"wg-quick@.service", "disabled"}};
    listing.instanceFileStates = {{"getty@tty2.service", "disabled"}, {"wg-quick@wg0.service", "enabled"}, {"getty@tty1.service", ""}};

    const std::vector<ServiceInfo> services = SystemdServices::services(listing);
    const ServiceInfo* tty2 = find(services, "getty@tty2.service");
    ASSERT_NE(tty2, nullptr);
    EXPECT_EQ(tty2->startType, ServiceStartType::Manual) << "disabled itself, though its template is enabled";
    const ServiceInfo* wg0 = find(services, "wg-quick@wg0.service");
    ASSERT_NE(wg0, nullptr);
    EXPECT_EQ(wg0->startType, ServiceStartType::Automatic) << "enabled itself, though its template isn't";
    const ServiceInfo* tty1 = find(services, "getty@tty1.service");
    ASSERT_NE(tty1, nullptr);
    EXPECT_EQ(tty1->startType, ServiceStartType::Automatic) << "its own state unknown: the template's";
}

// #2424: ListUnitFiles includes alias symlinks; ListUnits reports only each unit's real name. An alias
// is the same unit, so no row of its own (it would always read Stopped beside the running real one).
TEST(SystemdServicesTest, AnAliasIsNotAServiceOfItsOwn)
{
    SystemdServices::Listing listing;
    listing.units = {
        {.name = "rsyslog.service",
         .description = "System Logging Service",
         .activeState = "active",
         .subState = "running",
         .objectPath = "/u/1"},
    };
    listing.unitFiles = {
        {"rsyslog.service", "enabled"},
        {"syslog.service", "alias"},
        {"dbus-org.freedesktop.login1.service", "alias"}, // its unit not loaded either: still no row
    };

    const std::vector<ServiceInfo> services = SystemdServices::services(listing);
    ASSERT_EQ(services.size(), 1U);
    EXPECT_EQ(services.front().name, "rsyslog.service");
    EXPECT_EQ(services.front().state, ServiceState::Running);
    EXPECT_EQ(find(services, "syslog.service"), nullptr);
}

/// A root with or without /run/systemd/system, as a system booted with systemd has it.
[[nodiscard]] std::filesystem::path systemdRoot(const ScopedTempDir& dir, bool running)
{
    if (running)
    {
        std::filesystem::create_directories(dir.path / "run/systemd/system");
    }
    return dir.path;
}

// #2590: a loaded service's User, ExecStart and Type fill its Account, Command and Type.
TEST(SystemdServicesTest, AServicesConfigFillsItsAccountCommandAndType)
{
    SystemdServices::Listing listing;
    listing.units = {
        {.name = "nginx.service", .description = "nginx", .activeState = "active", .subState = "running", .objectPath = "/u/1"},
        {.name = "cron.service", .description = "cron", .activeState = "active", .subState = "running", .objectPath = "/u/2"},
        {.name = "unread.service", .description = "unread", .activeState = "active", .subState = "running", .objectPath = "/u/3"},
    };
    listing.unitFiles = {{"ssh.service", "disabled"}}; // Installed, not loaded: no configuration to read
    listing.configs["nginx.service"] = {.user = "www-data",
                                        .dynamicUser = false,
                                        .execStart = {{"/usr/sbin/nginx", "-g", "daemon on; master_process on;"}},
                                        .type = "forking"};
    listing.configs["cron.service"] = {.user = "", .dynamicUser = false, .execStart = {{"/usr/sbin/cron", "-f"}}, .type = "exec"};

    const std::vector<ServiceInfo> services = SystemdServices::services(listing);
    const ServiceInfo* nginx = find(services, "nginx.service");
    ASSERT_NE(nginx, nullptr);
    EXPECT_EQ(nginx->account, "www-data");
    EXPECT_EQ(nginx->binaryPath, "/usr/sbin/nginx -g daemon on; master_process on;");
    EXPECT_EQ(nginx->serviceType, "Forking");
    const ServiceInfo* cron = find(services, "cron.service");
    ASSERT_NE(cron, nullptr);
    EXPECT_EQ(cron->account, "root") << "no User=: systemd runs it as root";
    EXPECT_EQ(cron->serviceType, "Exec");
    for (const char* name : {"unread.service", "ssh.service"})
    {
        const ServiceInfo* bare = find(services, name);
        ASSERT_NE(bare, nullptr) << name;
        EXPECT_TRUE(bare->account.empty()) << name;
        EXPECT_TRUE(bare->binaryPath.empty()) << name;
        EXPECT_TRUE(bare->serviceType.empty()) << name;
    }
}

TEST(SystemdServicesTest, AnEmptyUserIsRootUnlessTheUserIsDynamic)
{
    SystemdServices::UnitConfig config;
    EXPECT_EQ(SystemdServices::accountFrom("cron.service", config), "") << "User not read: not guessed";
    config.user = "";
    EXPECT_EQ(SystemdServices::accountFrom("cron.service", config), "root");
    config.dynamicUser = true;
    EXPECT_EQ(SystemdServices::accountFrom("systemd-timesyncd.service", config), "systemd-timesyncd");
    config.user = "ntp";
    EXPECT_EQ(SystemdServices::accountFrom("systemd-timesyncd.service", config), "ntp") << "User= wins";
}

TEST(SystemdServicesTest, ACommandJoinsEachArgvWithSpacesAndEachLineWithASemicolon)
{
    EXPECT_EQ(SystemdServices::commandLineFrom({}), "");
    EXPECT_EQ(SystemdServices::commandLineFrom({{"/usr/bin/true"}}), "/usr/bin/true");
    EXPECT_EQ(SystemdServices::commandLineFrom({{"/usr/sbin/sshd", "-D", "-o", "LogLevel=INFO"}}), "/usr/sbin/sshd -D -o LogLevel=INFO");
    EXPECT_EQ(SystemdServices::commandLineFrom({{"/bin/mkdir", "-p", "/run/x"}, {}, {"/usr/bin/x", "--start"}}),
              "/bin/mkdir -p /run/x; /usr/bin/x --start")
        << "an empty argv is skipped";
}

TEST(SystemdServicesTest, TypesAreLabelledAsWindowsLabelsItsTypes)
{
    EXPECT_EQ(SystemdServices::serviceTypeText("simple"), "Simple");
    EXPECT_EQ(SystemdServices::serviceTypeText("exec"), "Exec");
    EXPECT_EQ(SystemdServices::serviceTypeText("forking"), "Forking");
    EXPECT_EQ(SystemdServices::serviceTypeText("oneshot"), "Oneshot");
    EXPECT_EQ(SystemdServices::serviceTypeText("dbus"), "D-Bus");
    EXPECT_EQ(SystemdServices::serviceTypeText("notify"), "Notify");
    EXPECT_EQ(SystemdServices::serviceTypeText("notify-reload"), "Notify (reload)");
    EXPECT_EQ(SystemdServices::serviceTypeText("idle"), "Idle");
    EXPECT_EQ(SystemdServices::serviceTypeText("something-new"), "something-new") << "as systemd gave it";
    EXPECT_EQ(SystemdServices::serviceTypeText(""), "");
}

TEST(LinuxServiceProbeTest, ListsTheReadsServices)
{
    const ScopedTempDir dir("ts_test_services_running");
    int reads = 0;
    LinuxServiceProbe probe(
        [&reads]
        {
            ++reads;
            SystemdBus::ServicesRead read;
            read.ok = true;
            read.listing.units = {
                {.name = "cron.service", .description = "Cron", .activeState = "active", .subState = "running", .objectPath = "/u"}};
            read.listing.unitFiles = {{"cron.service", "enabled"}};
            return read;
        },
        systemdRoot(dir, true),
        true);
    const ServiceCapabilities caps = probe.capabilities();
    EXPECT_TRUE(caps.canEnumerate);
    EXPECT_TRUE(caps.unavailableReason.empty());
    EXPECT_TRUE(caps.hasDisplayName);
    EXPECT_TRUE(caps.hasStartType);
    EXPECT_TRUE(caps.hasPid);
    EXPECT_TRUE(caps.hasBinaryPath);  // ExecStart (#2590)
    EXPECT_TRUE(caps.hasAccount);     // User
    EXPECT_TRUE(caps.hasServiceType); // Type
    EXPECT_FALSE(caps.hasGroup);
    const ServiceEnumeration list = probe.enumerate();
    ASSERT_TRUE(list.ok) << list.failureReason;
    ASSERT_EQ(list.services.size(), 1U);
    EXPECT_EQ(list.services.front().name, "cron.service");
    EXPECT_EQ(list.services.front().startType, ServiceStartType::Automatic);
    EXPECT_EQ(reads, 1);
}

TEST(LinuxServiceProbeTest, AFailedReadIsAFailureNotAnEmptyList)
{
    const ScopedTempDir dir("ts_test_services_failed");
    LinuxServiceProbe probe(
        []
        {
            SystemdBus::ServicesRead read;
            read.error = "systemd didn't answer in time";
            return read;
        },
        systemdRoot(dir, true),
        true);
    const ServiceEnumeration list = probe.enumerate();
    EXPECT_FALSE(list.ok);
    EXPECT_EQ(list.failureReason, "systemd didn't answer in time");
}

TEST(LinuxServiceProbeTest, WithoutSystemdOrLibsystemdItSaysWhyAndNeverReads)
{
    int reads = 0;
    const auto reader = [&reads]
    {
        ++reads;
        return SystemdBus::ServicesRead{};
    };
    const ScopedTempDir dir("ts_test_services_none");
    LinuxServiceProbe noSystemd(reader, systemdRoot(dir, false), true);
    EXPECT_FALSE(noSystemd.capabilities().canEnumerate);
    EXPECT_EQ(noSystemd.capabilities().unavailableReason, LinuxServiceProbe::NOT_RUNNING_REASON);
    EXPECT_EQ(noSystemd.enumerate().failureReason, LinuxServiceProbe::NOT_RUNNING_REASON);

    const ScopedTempDir running("ts_test_services_unbuilt");
    LinuxServiceProbe unbuilt(reader, systemdRoot(running, true), false);
    EXPECT_FALSE(unbuilt.capabilities().canEnumerate);
    EXPECT_EQ(unbuilt.enumerate().failureReason, LinuxServiceProbe::NOT_BUILT_REASON);
    EXPECT_EQ(reads, 0);
}

// The real read, where systemd runs (CI's runners, a desktop, WSL with systemd): systemd's own journal
// service is loaded and running, with a main process.
TEST(LinuxServiceProbeTest, ReadsThisMachinesServicesOrSaysWhyNot)
{
    LinuxServiceProbe probe;
    if (!probe.capabilities().canEnumerate)
    {
        GTEST_SKIP() << probe.capabilities().unavailableReason;
    }
    const ServiceEnumeration list = probe.enumerate();
    if (!list.ok)
    {
        GTEST_SKIP() << "systemd isn't reachable here: " << list.failureReason;
    }
    const ServiceInfo* journald = find(list.services, "systemd-journald.service");
    ASSERT_NE(journald, nullptr) << list.services.size() << " services";
    EXPECT_EQ(journald->state, ServiceState::Running);
    EXPECT_NE(journald->pid, 0U);
    EXPECT_FALSE(journald->displayName.empty());
}

} // namespace
} // namespace Platform
