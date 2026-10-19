#pragma once

/// @file FakeSystemBus.h
/// @brief A private D-Bus "system bus" for SystemdBus tests (#1395): starts its own dbus-daemon in a
/// temporary directory, points DBUS_SYSTEM_BUS_ADDRESS at it, and serves made-up objects -- methods
/// with a string, int32 or no argument that return a path, a name and path, or a string, and
/// org.freedesktop.DBus.Properties.Get -- from a thread, so the readers'
/// reply parsing runs against known answers instead of whatever this machine (or CI runner) has.
/// Linux with libsystemd only (TASKSMACK_HAS_SDBUS).

#ifdef TASKSMACK_HAS_SDBUS

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <signal.h> // NOLINT(modernize-deprecated-headers) - POSIX kill(), which <csignal> doesn't declare
#include <spawn.h>
#include <stdlib.h> // NOLINT(modernize-deprecated-headers) - POSIX setenv()/unsetenv(), which <cstdlib> doesn't declare
#include <sys/wait.h>
// Newer libsystemd declares sd_bus and sd_bus_error in sd-bus-protocol.h, older releases in sd-bus.h
// (as SystemdBus.cpp notes), so include-cleaner wants this line on one and not the other.
#include <systemd/sd-bus-protocol.h> // NOLINT(misc-include-cleaner) - see above
#include <systemd/sd-bus.h>
#include <unistd.h>

// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables) - POSIX's own, for posix_spawn
extern char** environ;

namespace TestSupport
{

/// An object path value ('o'), told apart from a string ('s').
struct ObjectPath
{
    std::string path;
};

/// A{sv} entry values the fakes need: strings ('s'), and unsigned numbers of 32 ('u'), 8 ('y') and 64
/// ('t') bits.
using DictValue = std::variant<std::string, std::uint32_t, std::uint8_t, std::uint64_t>;

/// systemd-resolved's DNS servers, a(iay): each one's address family and bytes.
using DnsServers = std::vector<std::pair<std::int32_t, std::vector<std::uint8_t>>>;
/// systemd-resolved's search domains, a(sb): each one and whether it only routes lookups.
using DnsDomains = std::vector<std::pair<std::string, bool>>;

/// One line of a systemd Service's ExecStart (#2590): its binary, argv and ignore-errors flag. The run
/// times, PID and exit status the property also carries are served as zeros.
struct ExecCommand
{
    std::string path;
    std::vector<std::string> argv;
    bool ignoreErrors = false;
};
/// A systemd Service's ExecStart, a(sasbttttuii) (#2590).
using ExecCommands = std::vector<ExecCommand>;

/// A property value: 's', 'o', 'u', 'ay', 'a{sv}', 'a(iay)', 'a(sb)', udisks2's 'b', 't', 'd', 'x',
/// 'q' and 'as', and systemd's ExecStart 'a(sasbttttuii)'.
using PropertyValue = std::variant<std::string,
                                   ObjectPath,
                                   std::uint32_t,
                                   std::vector<std::uint8_t>,
                                   std::map<std::string, DictValue>,
                                   DnsServers,
                                   DnsDomains,
                                   bool,
                                   std::uint64_t,
                                   double,
                                   std::int64_t,
                                   std::uint16_t,
                                   std::vector<std::string>,
                                   ExecCommands>;

/// One row of systemd's ListUnits reply, "(ssssssouso)" (#2006).
struct UnitRow
{
    std::string name;
    std::string description;
    std::string loadState = "loaded";
    std::string activeState;
    std::string subState;
    std::string following;
    std::string path; ///< The unit's object path
    std::uint32_t jobId = 0;
    std::string jobType;
    std::string jobPath = "/";
};

/// What a fake method returns: an object path (with a name before it, for an "so" reply), a string
/// (an "s" reply), a dictionary, systemd's unit or unit-file lists, or a D-Bus error by name.
struct MethodAnswer
{
    std::optional<ObjectPath> path;
    std::string errorName;
    std::optional<std::string> name = std::nullopt;                                           ///< with path: reply "so"
    std::optional<std::string> text = std::nullopt;                                           ///< reply "s"
    std::optional<std::map<std::string, DictValue>> dict = std::nullopt;                      ///< reply "a{sv}"
    std::optional<std::vector<UnitRow>> units = std::nullopt;                                 ///< reply "a(ssssssouso)" (ListUnits)
    std::optional<std::vector<std::pair<std::string, std::string>>> unitFiles = std::nullopt; ///< reply "a(ss)" (ListUnitFiles)
};

class FakeSystemBus
{
  public:
    /// object path -> interface -> property name -> value
    using Properties = std::map<std::string, std::map<std::string, std::map<std::string, PropertyValue>>>;
    /// Answers a call to (path, interface, member) with one string argument.
    using StringMethod = std::function<MethodAnswer(const std::string& argument)>;
    /// Answers a call with one int32 argument (an interface index).
    using IntMethod = std::function<MethodAnswer(std::int32_t argument)>;
    /// Answers a call with no arguments.
    using NoArgMethod = std::function<MethodAnswer()>;
    /// Reads the call and replies to it itself (sd_bus_reply_method_return/_errorf), for arguments the
    /// typed methods don't read: systemd's StartUnit (ss) and unit-file calls (as, b...) (#2006). Runs on
    /// the bus thread, which also answers property reads, so it may change properties() as it goes.
    using RawMethod = std::function<int(sd_bus_message* call)>;

    FakeSystemBus() = default;
    FakeSystemBus(const FakeSystemBus&) = delete;
    FakeSystemBus& operator=(const FakeSystemBus&) = delete;
    FakeSystemBus(FakeSystemBus&&) = delete;
    FakeSystemBus& operator=(FakeSystemBus&&) = delete;

    ~FakeSystemBus()
    {
        stop();
    }

    /// Whether this machine can run one (dbus-daemon on PATH).
    [[nodiscard]] static bool available()
    {
        const std::string path = readEnv("PATH").value_or("");
        std::string_view dirs = path;
        while (!dirs.empty())
        {
            const std::size_t colon = dirs.find(':');
            const std::filesystem::path candidate = std::filesystem::path(dirs.substr(0, colon)) / "dbus-daemon";
            if (::access(candidate.c_str(), X_OK) == 0)
            {
                return true;
            }
            dirs = colon == std::string_view::npos ? std::string_view{} : dirs.substr(colon + 1);
        }
        return false;
    }

    /// The properties served, before start().
    Properties& properties()
    {
        return m_Properties;
    }

    /// A method with one string argument returning an object path, before start().
    void addStringMethod(const std::string& path, const std::string& interface, const std::string& member, StringMethod method)
    {
        m_Methods[path + '\n' + interface + '\n' + member] = std::move(method);
    }

    /// A method with one int32 argument, before start().
    void addIntMethod(const std::string& path, const std::string& interface, const std::string& member, IntMethod method)
    {
        m_Methods[path + '\n' + interface + '\n' + member] = std::move(method);
    }

    /// A method with no arguments, before start().
    void addNoArgMethod(const std::string& path, const std::string& interface, const std::string& member, NoArgMethod method)
    {
        m_Methods[path + '\n' + interface + '\n' + member] = std::move(method);
    }

    /// Makes reads of one property fail with the D-Bus error @p errorName (e.g. NoReply, as a service
    /// that doesn't answer in time), before start() (#1891).
    void failProperty(const std::string& path, const std::string& interface, const std::string& name, std::string errorName)
    {
        m_PropertyErrors[path + '\n' + interface + '\n' + name] = std::move(errorName);
    }

    /// How many times a property has been asked for (Properties.Get), answered or failed (#2590), so a
    /// test can tell a cached read from a fresh one. Safe while the bus is serving.
    [[nodiscard]] int propertyReads(const std::string& path, const std::string& interface, const std::string& name) const
    {
        const std::scoped_lock lock(m_ReadsMutex);
        const auto found = m_PropertyReads.find(path + '\n' + interface + '\n' + name);
        return found == m_PropertyReads.end() ? 0 : found->second;
    }

    /// A method that reads and answers the call itself, before start().
    void addRawMethod(const std::string& path, const std::string& interface, const std::string& member, RawMethod method)
    {
        m_Methods[path + '\n' + interface + '\n' + member] = std::move(method);
    }

    /// A method that takes the call and never answers it, as a stalled service (#1689), before start().
    /// The caller gives up only by its own method-call timeout; the bus thread isn't held meanwhile.
    void addUnansweredMethod(const std::string& path, const std::string& interface, const std::string& member)
    {
        m_Methods[path + '\n' + interface + '\n' + member] =
            RawMethod([](sd_bus_message* /*call*/) -> int { return 1; }); // handled, no reply
    }

    /// Starts the daemon, takes @p serviceName on it when given (leave empty for a bus where the service
    /// isn't running), and points DBUS_SYSTEM_BUS_ADDRESS at it. False, with the reason in error(),
    /// when it couldn't.
    [[nodiscard]] bool start(const std::string& serviceName)
    {
        if (!makeTempDir() || !startDaemon() || !connectService(serviceName))
        {
            return false;
        }
        m_SavedAddress = readEnv("DBUS_SYSTEM_BUS_ADDRESS");
        setenv("DBUS_SYSTEM_BUS_ADDRESS", address().c_str(), 1); // NOLINT(concurrency-mt-unsafe) - before the readers run
        m_EnvSet = true;
        m_Thread = std::thread([this] { serve(); });
        return true;
    }

    [[nodiscard]] const std::string& error() const noexcept
    {
        return m_Error;
    }

  private:
    [[nodiscard]] std::string address() const
    {
        return "unix:path=" + (m_Dir / "bus").string();
    }

    static std::optional<std::string> readEnv(const char* name)
    {
        const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe) - test setup only
        return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
    }

    bool makeTempDir()
    {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        std::random_device random;
        for (int attempt = 0; attempt < 100 && m_Dir.empty() && !ec; ++attempt)
        {
            std::filesystem::path candidate = base / ("tasksmack_fake_bus_" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate, ec) && !ec)
            {
                m_Dir = std::move(candidate);
            }
        }
        if (m_Dir.empty())
        {
            m_Error = "no temporary directory";
            return false;
        }
        return true;
    }

    bool startDaemon()
    {
        const std::filesystem::path config = m_Dir / "bus.conf";
        std::ofstream(config) << "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" "
                                 "\"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                                 "<busconfig><type>session</type><listen>"
                              << address()
                              << "</listen><auth>EXTERNAL</auth>"
                                 "<policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/>"
                                 "<allow eavesdrop=\"true\"/><allow own=\"*\"/></policy></busconfig>\n";
        const std::string configArg = "--config-file=" + config.string();
        std::vector<std::string> args{"dbus-daemon", configArg, "--nofork", "--nopidfile"};
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& arg : args)
        {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        if (posix_spawnp(&m_Daemon, "dbus-daemon", nullptr, nullptr, argv.data(), environ) != 0)
        {
            m_Daemon = 0;
            m_Error = "dbus-daemon couldn't be started";
            return false;
        }
        // Its socket appears once it is listening.
        for (int i = 0; i < 200; ++i)
        {
            std::error_code ec;
            if (std::filesystem::exists(m_Dir / "bus", ec))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        m_Error = "dbus-daemon didn't start listening";
        return false;
    }

    bool connectService(const std::string& serviceName)
    {
        // The socket file exists from the daemon's bind(), a moment before its listen(): a connection
        // in between is refused, so it is tried again for a while (a loaded CI runner, #1835).
        bool connected = false;
        for (int attempt = 0; attempt < 200 && !connected; ++attempt)
        {
            if (m_Service != nullptr)
            {
                m_Service = sd_bus_flush_close_unref(m_Service);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            connected = sd_bus_new(&m_Service) >= 0 && sd_bus_set_address(m_Service, address().c_str()) >= 0 &&
                        sd_bus_set_bus_client(m_Service, 1) >= 0 && sd_bus_start(m_Service) >= 0;
        }
        if (!connected)
        {
            m_Error = "couldn't connect the fake service";
            return false;
        }
        if (!serviceName.empty() && sd_bus_request_name(m_Service, serviceName.c_str(), 0) < 0)
        {
            m_Error = "couldn't take the name " + serviceName;
            return false;
        }
        if (sd_bus_add_fallback(m_Service, nullptr, "/", &FakeSystemBus::onMessage, this) < 0)
        {
            m_Error = "couldn't serve objects";
            return false;
        }
        return true;
    }

    void serve()
    {
        while (!m_Stop.load())
        {
            const int processed = sd_bus_process(m_Service, nullptr);
            if (processed < 0)
            {
                return;
            }
            if (processed == 0)
            {
                (void) sd_bus_wait(m_Service, 20'000); // 20 ms, so stop() is noticed promptly
            }
        }
    }

    void stop()
    {
        m_Stop.store(true);
        if (m_Thread.joinable())
        {
            m_Thread.join();
        }
        if (m_Service != nullptr)
        {
            sd_bus_flush_close_unref(m_Service);
            m_Service = nullptr;
        }
        if (m_Daemon > 0)
        {
            kill(m_Daemon, SIGTERM);
            int status = 0;
            waitpid(m_Daemon, &status, 0);
            m_Daemon = 0;
        }
        if (m_EnvSet)
        {
            // NOLINTBEGIN(concurrency-mt-unsafe) - after the readers ran, single-threaded again
            if (m_SavedAddress.has_value())
            {
                setenv("DBUS_SYSTEM_BUS_ADDRESS", m_SavedAddress->c_str(), 1);
            }
            else
            {
                unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
            }
            // NOLINTEND(concurrency-mt-unsafe)
            m_EnvSet = false;
        }
        if (!m_Dir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Dir, ec);
            m_Dir.clear();
        }
    }

    static int appendValue(sd_bus_message* reply, const PropertyValue& value)
    {
        if (const auto* text = std::get_if<std::string>(&value))
        {
            return sd_bus_message_append(reply, "v", "s", text->c_str());
        }
        if (const auto* path = std::get_if<ObjectPath>(&value))
        {
            return sd_bus_message_append(reply, "v", "o", path->path.c_str());
        }
        if (const auto* number = std::get_if<std::uint32_t>(&value))
        {
            return sd_bus_message_append(reply, "v", "u", *number);
        }
        if (const auto* flag = std::get_if<bool>(&value))
        {
            return sd_bus_message_append(reply, "v", "b", static_cast<int>(*flag));
        }
        if (const auto* number = std::get_if<std::uint64_t>(&value))
        {
            return sd_bus_message_append(reply, "v", "t", *number);
        }
        if (const auto* number = std::get_if<double>(&value))
        {
            return sd_bus_message_append(reply, "v", "d", *number);
        }
        if (const auto* number = std::get_if<std::int64_t>(&value))
        {
            return sd_bus_message_append(reply, "v", "x", *number);
        }
        if (const auto* number = std::get_if<std::uint16_t>(&value))
        {
            return sd_bus_message_append(reply, "v", "q", *number);
        }
        if (const auto* strings = std::get_if<std::vector<std::string>>(&value))
        {
            int opened = sd_bus_message_open_container(reply, 'v', "as");
            if (opened >= 0)
            {
                opened = sd_bus_message_open_container(reply, 'a', "s");
            }
            for (auto it = strings->begin(); opened >= 0 && it != strings->end(); ++it)
            {
                opened = sd_bus_message_append(reply, "s", it->c_str());
            }
            if (opened >= 0)
            {
                opened = sd_bus_message_close_container(reply);
            }
            return opened >= 0 ? sd_bus_message_close_container(reply) : opened;
        }
        int result = 0;
        if (const auto* bytes = std::get_if<std::vector<std::uint8_t>>(&value))
        {
            result = sd_bus_message_open_container(reply, 'v', "ay");
            if (result >= 0)
            {
                result = sd_bus_message_append_array(reply, 'y', bytes->data(), bytes->size());
            }
            return result >= 0 ? sd_bus_message_close_container(reply) : result;
        }
        if (const auto* servers = std::get_if<DnsServers>(&value))
        {
            result = sd_bus_message_open_container(reply, 'v', "a(iay)");
            if (result >= 0)
            {
                result = sd_bus_message_open_container(reply, 'a', "(iay)");
            }
            for (auto it = servers->begin(); result >= 0 && it != servers->end(); ++it)
            {
                result = sd_bus_message_open_container(reply, 'r', "iay");
                if (result >= 0)
                {
                    result = sd_bus_message_append(reply, "i", it->first);
                }
                if (result >= 0)
                {
                    result = sd_bus_message_append_array(reply, 'y', it->second.data(), it->second.size());
                }
                if (result >= 0)
                {
                    result = sd_bus_message_close_container(reply);
                }
            }
            if (result >= 0)
            {
                result = sd_bus_message_close_container(reply);
            }
            return result >= 0 ? sd_bus_message_close_container(reply) : result;
        }
        if (const auto* domains = std::get_if<DnsDomains>(&value))
        {
            result = sd_bus_message_open_container(reply, 'v', "a(sb)");
            if (result >= 0)
            {
                result = sd_bus_message_open_container(reply, 'a', "(sb)");
            }
            for (auto it = domains->begin(); result >= 0 && it != domains->end(); ++it)
            {
                result = sd_bus_message_append(reply, "(sb)", it->first.c_str(), static_cast<int>(it->second));
            }
            if (result >= 0)
            {
                result = sd_bus_message_close_container(reply);
            }
            return result >= 0 ? sd_bus_message_close_container(reply) : result;
        }
        if (const auto* commands = std::get_if<ExecCommands>(&value))
        {
            result = sd_bus_message_open_container(reply, 'v', "a(sasbttttuii)");
            if (result >= 0)
            {
                result = appendExecCommands(reply, *commands);
            }
            return result >= 0 ? sd_bus_message_close_container(reply) : result;
        }
        result = sd_bus_message_open_container(reply, 'v', "a{sv}");
        if (result >= 0)
        {
            result = appendDict(reply, std::get<std::map<std::string, DictValue>>(value));
        }
        return result >= 0 ? sd_bus_message_close_container(reply) : result;
    }

    /// systemd's ExecStart, a(sasbttttuii), into @p reply (#2590).
    static int appendExecCommands(sd_bus_message* reply, const ExecCommands& commands)
    {
        int result = sd_bus_message_open_container(reply, 'a', "(sasbttttuii)");
        for (auto it = commands.begin(); result >= 0 && it != commands.end(); ++it)
        {
            result = sd_bus_message_open_container(reply, 'r', "sasbttttuii");
            if (result >= 0)
            {
                result = sd_bus_message_append(reply, "s", it->path.c_str());
            }
            if (result >= 0)
            {
                result = sd_bus_message_open_container(reply, 'a', "s");
            }
            for (auto arg = it->argv.begin(); result >= 0 && arg != it->argv.end(); ++arg)
            {
                result = sd_bus_message_append(reply, "s", arg->c_str());
            }
            if (result >= 0)
            {
                result = sd_bus_message_close_container(reply);
            }
            if (result >= 0)
            {
                const std::uint64_t never = 0;
                result = sd_bus_message_append(reply,
                                               "bttttuii",
                                               static_cast<int>(it->ignoreErrors),
                                               never,
                                               never,
                                               never,
                                               never,
                                               std::uint32_t{0},
                                               std::int32_t{0},
                                               std::int32_t{0});
            }
            if (result >= 0)
            {
                result = sd_bus_message_close_container(reply);
            }
        }
        return result >= 0 ? sd_bus_message_close_container(reply) : result;
    }

    /// An a{sv} into @p reply.
    static int appendUnits(sd_bus_message* reply, const std::vector<UnitRow>& units)
    {
        int result = sd_bus_message_open_container(reply, 'a', "(ssssssouso)");
        for (const UnitRow& unit : units)
        {
            if (result < 0)
            {
                break;
            }
            result = sd_bus_message_append(reply,
                                           "(ssssssouso)",
                                           unit.name.c_str(),
                                           unit.description.c_str(),
                                           unit.loadState.c_str(),
                                           unit.activeState.c_str(),
                                           unit.subState.c_str(),
                                           unit.following.c_str(),
                                           unit.path.c_str(),
                                           unit.jobId,
                                           unit.jobType.c_str(),
                                           unit.jobPath.c_str());
        }
        return result < 0 ? result : sd_bus_message_close_container(reply);
    }

    static int appendUnitFiles(sd_bus_message* reply, const std::vector<std::pair<std::string, std::string>>& files)
    {
        int result = sd_bus_message_open_container(reply, 'a', "(ss)");
        for (const auto& [path, state] : files)
        {
            if (result < 0)
            {
                break;
            }
            result = sd_bus_message_append(reply, "(ss)", path.c_str(), state.c_str());
        }
        return result < 0 ? result : sd_bus_message_close_container(reply);
    }

    static int appendDict(sd_bus_message* reply, const std::map<std::string, DictValue>& dict)
    {
        int result = sd_bus_message_open_container(reply, 'a', "{sv}");
        for (auto it = dict.begin(); result >= 0 && it != dict.end(); ++it)
        {
            const char* key = it->first.c_str();
            if (const auto* text = std::get_if<std::string>(&it->second))
            {
                result = sd_bus_message_append(reply, "{sv}", key, "s", text->c_str());
            }
            else if (const auto* u32 = std::get_if<std::uint32_t>(&it->second))
            {
                result = sd_bus_message_append(reply, "{sv}", key, "u", *u32);
            }
            else if (const auto* u8 = std::get_if<std::uint8_t>(&it->second))
            {
                result = sd_bus_message_append(reply, "{sv}", key, "y", *u8);
            }
            else
            {
                result = sd_bus_message_append(reply, "{sv}", key, "t", std::get<std::uint64_t>(it->second));
            }
        }
        return result >= 0 ? sd_bus_message_close_container(reply) : result;
    }

    static int onMessage(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
    {
        auto* self = static_cast<FakeSystemBus*>(userdata);
        const char* path = sd_bus_message_get_path(message);
        const char* interface = sd_bus_message_get_interface(message);
        const char* member = sd_bus_message_get_member(message);
        if (path == nullptr || interface == nullptr || member == nullptr)
        {
            return 0;
        }
        if (std::string_view(interface) == "org.freedesktop.DBus.Properties" && std::string_view(member) == "Get")
        {
            const char* iface = nullptr;
            const char* name = nullptr;
            if (sd_bus_message_read(message, "ss", &iface, &name) < 0)
            {
                return 0;
            }
            {
                const std::scoped_lock lock(self->m_ReadsMutex);
                ++self->m_PropertyReads[std::string(path) + '\n' + iface + '\n' + name];
            }
            if (const auto failure = self->m_PropertyErrors.find(std::string(path) + '\n' + iface + '\n' + name);
                failure != self->m_PropertyErrors.end())
            {
                return sd_bus_reply_method_errorf(message, failure->second.c_str(), "Fake error");
            }
            const auto object = self->m_Properties.find(path);
            if (object != self->m_Properties.end())
            {
                const auto properties = object->second.find(iface);
                if (properties != object->second.end())
                {
                    const auto property = properties->second.find(name);
                    if (property != properties->second.end())
                    {
                        sd_bus_message* reply = nullptr;
                        int result = sd_bus_message_new_method_return(message, &reply);
                        if (result >= 0)
                        {
                            result = appendValue(reply, property->second);
                        }
                        if (result >= 0)
                        {
                            result = sd_bus_send(nullptr, reply, nullptr);
                        }
                        sd_bus_message_unref(reply);
                        return result >= 0 ? 1 : result;
                    }
                }
            }
            // A path the fake serves nothing on is an unknown object, as a real service answers.
            const std::string prefix = std::string(path) + '\n';
            const bool known =
                object != self->m_Properties.end() ||
                std::ranges::any_of(self->m_Methods, [&prefix](const auto& entry) { return entry.first.starts_with(prefix); });
            return known ? sd_bus_reply_method_errorf(message, "org.freedesktop.DBus.Error.UnknownProperty", "No such property")
                         : sd_bus_reply_method_errorf(message, "org.freedesktop.DBus.Error.UnknownObject", "No such object");
        }
        const auto method = self->m_Methods.find(std::string(path) + '\n' + interface + '\n' + member);
        if (method == self->m_Methods.end())
        {
            return 0; // sd-bus answers UnknownMethod/UnknownObject
        }
        if (const auto* raw = std::get_if<RawMethod>(&method->second))
        {
            return (*raw)(message);
        }
        MethodAnswer answer;
        if (const auto* byString = std::get_if<StringMethod>(&method->second))
        {
            const char* argument = nullptr;
            if (sd_bus_message_read(message, "s", &argument) < 0)
            {
                return 0;
            }
            answer = (*byString)(argument != nullptr ? argument : "");
        }
        else if (const auto* byInt = std::get_if<IntMethod>(&method->second))
        {
            std::int32_t argument = 0;
            if (sd_bus_message_read(message, "i", &argument) < 0)
            {
                return 0;
            }
            answer = (*byInt)(argument);
        }
        else
        {
            answer = std::get<NoArgMethod>(method->second)();
        }
        if (answer.dict.has_value())
        {
            sd_bus_message* reply = nullptr;
            int result = sd_bus_message_new_method_return(message, &reply);
            if (result >= 0)
            {
                result = appendDict(reply, *answer.dict);
            }
            if (result >= 0)
            {
                result = sd_bus_send(nullptr, reply, nullptr);
            }
            sd_bus_message_unref(reply);
            return result >= 0 ? 1 : result;
        }
        if (answer.units.has_value() || answer.unitFiles.has_value())
        {
            sd_bus_message* reply = nullptr;
            int result = sd_bus_message_new_method_return(message, &reply);
            if (result >= 0)
            {
                result = answer.units.has_value() ? appendUnits(reply, *answer.units) : appendUnitFiles(reply, *answer.unitFiles);
            }
            if (result >= 0)
            {
                result = sd_bus_send(nullptr, reply, nullptr);
            }
            sd_bus_message_unref(reply);
            return result >= 0 ? 1 : result;
        }
        if (answer.text.has_value())
        {
            return sd_bus_reply_method_return(message, "s", answer.text->c_str());
        }
        if (answer.path.has_value() && answer.name.has_value())
        {
            return sd_bus_reply_method_return(message, "so", answer.name->c_str(), answer.path->path.c_str());
        }
        if (answer.path.has_value())
        {
            return sd_bus_reply_method_return(message, "o", answer.path->path.c_str());
        }
        return sd_bus_reply_method_errorf(message, answer.errorName.c_str(), "Fake error");
    }

    Properties m_Properties;
    std::map<std::string, std::string> m_PropertyErrors; ///< path\ninterface\nname -> the error its reads fail with
    mutable std::mutex m_ReadsMutex;                     ///< Guards m_PropertyReads: the bus thread counts, a test reads
    std::map<std::string, int> m_PropertyReads;          ///< path\ninterface\nname -> how often it was asked for
    std::map<std::string, std::variant<StringMethod, IntMethod, NoArgMethod, RawMethod>> m_Methods;
    std::filesystem::path m_Dir;
    pid_t m_Daemon = 0; // NOLINT(misc-include-cleaner) - pid_t comes with <spawn.h> and <unistd.h>
    sd_bus* m_Service = nullptr;
    std::thread m_Thread;
    std::atomic<bool> m_Stop{false};
    std::optional<std::string> m_SavedAddress;
    bool m_EnvSet = false;
    std::string m_Error;
};

} // namespace TestSupport

#endif // TASKSMACK_HAS_SDBUS
