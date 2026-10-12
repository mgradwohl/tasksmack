#pragma once

/// @file FakeSystemBus.h
/// @brief A private D-Bus "system bus" for SystemdBus tests (#1395): starts its own dbus-daemon in a
/// temporary directory, points DBUS_SYSTEM_BUS_ADDRESS at it, and serves made-up objects -- methods
/// that return object paths, and org.freedesktop.DBus.Properties.Get -- from a thread, so the readers'
/// reply parsing runs against known answers instead of whatever this machine (or CI runner) has.
/// Linux with libsystemd only (TASKSMACK_HAS_SDBUS).

#ifdef TASKSMACK_HAS_SDBUS

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
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

/// A{sv} entry values the fakes need: strings and unsigned 32-bit numbers.
using DictValue = std::variant<std::string, std::uint32_t>;

/// A property value: 's', 'o', 'u', 'ay' or 'a{sv}'.
using PropertyValue = std::variant<std::string, ObjectPath, std::uint32_t, std::vector<std::uint8_t>, std::map<std::string, DictValue>>;

/// What a fake method returns: an object path, or a D-Bus error by name.
struct MethodAnswer
{
    std::optional<ObjectPath> path;
    std::string errorName;
};

class FakeSystemBus
{
  public:
    /// object path -> interface -> property name -> value
    using Properties = std::map<std::string, std::map<std::string, std::map<std::string, PropertyValue>>>;
    /// Answers a call to (path, interface, member) with one string argument.
    using StringMethod = std::function<MethodAnswer(const std::string& argument)>;

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
        if (sd_bus_new(&m_Service) < 0 || sd_bus_set_address(m_Service, address().c_str()) < 0 || sd_bus_set_bus_client(m_Service, 1) < 0 ||
            sd_bus_start(m_Service) < 0)
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
        const auto& dict = std::get<std::map<std::string, DictValue>>(value);
        result = sd_bus_message_open_container(reply, 'v', "a{sv}");
        if (result >= 0)
        {
            result = sd_bus_message_open_container(reply, 'a', "{sv}");
        }
        for (auto it = dict.begin(); result >= 0 && it != dict.end(); ++it)
        {
            if (const auto* text = std::get_if<std::string>(&it->second))
            {
                result = sd_bus_message_append(reply, "{sv}", it->first.c_str(), "s", text->c_str());
            }
            else
            {
                result = sd_bus_message_append(reply, "{sv}", it->first.c_str(), "u", std::get<std::uint32_t>(it->second));
            }
        }
        if (result >= 0)
        {
            result = sd_bus_message_close_container(reply);
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
            return sd_bus_reply_method_errorf(message, "org.freedesktop.DBus.Error.UnknownProperty", "No such property");
        }
        const auto method = self->m_Methods.find(std::string(path) + '\n' + interface + '\n' + member);
        if (method == self->m_Methods.end())
        {
            return 0; // sd-bus answers UnknownMethod/UnknownObject
        }
        const char* argument = nullptr;
        if (sd_bus_message_read(message, "s", &argument) < 0)
        {
            return 0;
        }
        const MethodAnswer answer = method->second(argument != nullptr ? argument : "");
        if (answer.path.has_value())
        {
            return sd_bus_reply_method_return(message, "o", answer.path->path.c_str());
        }
        return sd_bus_reply_method_errorf(message, answer.errorName.c_str(), "Fake error");
    }

    Properties m_Properties;
    std::map<std::string, StringMethod> m_Methods;
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
