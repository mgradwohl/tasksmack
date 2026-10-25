// NetworkManager over D-Bus: each Wi-Fi adapter's link and each adapter's IPv4 DHCP lease. Split out of SystemdBus.cpp (#2171).

#include "Platform/Linux/LinuxNetworkAdapters.h"
#include "Platform/Linux/SystemdBus.h"

#include <memory>
#include <optional>
#include <string>

#ifdef TASKSMACK_HAS_SDBUS
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/SdBusCall.h"
#include "Platform/ProcessEnvironment.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>
#include <utility>

// Newer libsystemd (259 here) declares sd_bus and sd_bus_error in sd-bus-protocol.h; older releases
// (Ubuntu 24.04's, in CI) declare them in sd-bus.h, so include-cleaner wants this line on one and not the other.
#include <systemd/sd-bus-protocol.h> // NOLINT(misc-include-cleaner) - see above
#include <systemd/sd-bus.h>
#endif

namespace Platform::SystemdBus
{

#ifdef TASKSMACK_HAS_SDBUS

using Detail::Bus;
using Detail::BusError;
using Detail::Message;
using Detail::openSystemBus;
using Detail::timedOut;

namespace
{

constexpr const char* NM = "org.freedesktop.NetworkManager";
constexpr const char* NM_PATH = "/org/freedesktop/NetworkManager";
constexpr const char* NM_WIRELESS = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* NM_ACCESS_POINT = "org.freedesktop.NetworkManager.AccessPoint";

/// One system-bus connection for every Wi-Fi adapter of a read, opened on the first.
struct NetworkManagerSession
{
    Bus bus;
    bool opened = false;
    int openResult = 0;
    std::string stopped; ///< Set once NetworkManager is found absent or unresponsive: every later adapter gets it.
};

/// Why NetworkManager couldn't answer for an adapter, in the user's terms, and whether that holds for
/// every other adapter too (it isn't running, or stopped answering).
[[nodiscard]] std::pair<std::string, bool> networkManagerError(const BusError& error, int result)
{
    if (timedOut(error.raw(), result))
    {
        return {"NetworkManager didn't answer in time", true};
    }
    if (error.is("org.freedesktop.DBus.Error.ServiceUnknown") || error.is("org.freedesktop.DBus.Error.NameHasNoOwner"))
    {
        return {"NetworkManager isn't running", true};
    }
    if (error.is("org.freedesktop.NetworkManager.UnknownDevice") || error.is("org.freedesktop.DBus.Error.UnknownObject") ||
        error.is("org.freedesktop.DBus.Error.UnknownMethod") || error.is("org.freedesktop.DBus.Error.UnknownProperty") ||
        error.is("org.freedesktop.DBus.Error.UnknownInterface"))
    {
        return {"NetworkManager doesn't manage this adapter", false};
    }
    return {"NetworkManager couldn't be asked: " + error.text(result), false};
}

/// An object-path property into @p path (empty when it can't be read).
[[nodiscard]] int
getObjectPath(sd_bus* bus, const char* object, const char* interface, const char* name, std::string& path, BusError& error)
{
    Message reply;
    int result = sd_bus_get_property(bus, NM, object, interface, name, error.get(), reply.out(), "o");
    const char* value = nullptr;
    if (result >= 0)
    {
        result = sd_bus_message_read(reply.get(), "o", &value);
    }
    if (result >= 0 && value != nullptr)
    {
        path = value;
    }
    return result;
}

[[nodiscard]] LinuxNetworkAdapters::WifiLinkRead readWifiLinkOnce(NetworkManagerSession& session, const std::string& adapter)
{
    LinuxNetworkAdapters::WifiLinkRead read;
    if (!session.opened)
    {
        session.opened = true;
        session.openResult = openSystemBus(session.bus);
        if (session.openResult < 0)
        {
            session.stopped = std::string("The system bus couldn't be reached: ") +
                              std::strerror(-session.openResult); // NOLINT(concurrency-mt-unsafe) - as above
        }
    }
    if (!session.stopped.empty())
    {
        read.error = session.stopped;
        return read;
    }
    sd_bus* bus = session.bus.get();
    const auto fail = [&session, &read](const BusError& error, int result)
    {
        auto [text, forEveryAdapter] = networkManagerError(error, result);
        if (forEveryAdapter)
        {
            session.stopped = text;
        }
        read.error = std::move(text);
        return read;
    };

    // The adapter's device object.
    BusError error;
    Message reply;
    int result = sd_bus_call_method(bus, NM, NM_PATH, NM, "GetDeviceByIpIface", error.get(), reply.out(), "s", adapter.c_str());
    const char* devicePath = nullptr;
    if (result >= 0)
    {
        result = sd_bus_message_read(reply.get(), "o", &devicePath);
    }
    if (result < 0 || devicePath == nullptr)
    {
        return fail(error, result);
    }
    const std::string device(devicePath);

    // The access point it is joined to: "/" when it isn't.
    std::string accessPoint;
    BusError apError;
    if (result = getObjectPath(bus, device.c_str(), NM_WIRELESS, "ActiveAccessPoint", accessPoint, apError); result < 0)
    {
        return fail(apError, result);
    }
    if (accessPoint.empty() || accessPoint == "/")
    {
        read.error = "Not connected";
        return read;
    }

    WifiLink link;
    // Each of these is optional: a value NetworkManager can't give is left at 0 or empty.
    BusError bitrateError;
    (void) sd_bus_get_property_trivial(bus, NM, device.c_str(), NM_WIRELESS, "Bitrate", bitrateError.get(), 'u', &link.bitrateKbps);
    // NetworkManager's 0-100 % strength of the access point's signal, as Windows' signal quality (#2591).
    std::uint8_t strength = 0;
    BusError strengthError;
    if (sd_bus_get_property_trivial(bus, NM, accessPoint.c_str(), NM_ACCESS_POINT, "Strength", strengthError.get(), 'y', &strength) >= 0)
    {
        link.signalQuality = std::min<std::uint32_t>(strength, 100U);
    }
    BusError frequencyError;
    (void) sd_bus_get_property_trivial(
        bus, NM, accessPoint.c_str(), NM_ACCESS_POINT, "Frequency", frequencyError.get(), 'u', &link.frequencyMhz);
    BusError ssidError;
    Message ssidReply;
    if (sd_bus_get_property(bus, NM, accessPoint.c_str(), NM_ACCESS_POINT, "Ssid", ssidError.get(), ssidReply.out(), "ay") >= 0)
    {
        const void* bytes = nullptr;
        std::size_t size = 0;
        if (sd_bus_message_read_array(ssidReply.get(), 'y', &bytes, &size) >= 0 && bytes != nullptr)
        {
            // An SSID is up to 32 arbitrary bytes, not necessarily UTF-8.
            link.ssid = Environment::escapeForDisplay(std::string_view(static_cast<const char*>(bytes), size));
        }
    }
    read.link = std::move(link);
    return read;
}

} // namespace

LinuxNetworkAdapters::WifiLinkReader makeWifiLinkReader()
{
    return [session = std::make_shared<NetworkManagerSession>()](const std::string& adapter)
    {
        return readWifiLinkOnce(*session, adapter);
    };
}

namespace
{

constexpr const char* NM_DEVICE = "org.freedesktop.NetworkManager.Device";
constexpr const char* NM_DHCP4 = "org.freedesktop.NetworkManager.DHCP4Config";

/// The DHCP4Config's Options, a{sv} with string values, into the three this page shows.
[[nodiscard]] int
readDhcpOptions(sd_bus* bus, const char* config, std::string& server, std::string& leaseTime, std::string& expiry, BusError& error)
{
    Message reply;
    int result = sd_bus_get_property(bus, NM, config, NM_DHCP4, "Options", error.get(), reply.out(), "a{sv}");
    if (result >= 0)
    {
        result = sd_bus_message_enter_container(reply.get(), 'a', "{sv}");
    }
    while (result >= 0 && (result = sd_bus_message_enter_container(reply.get(), 'e', "sv")) > 0)
    {
        const char* key = nullptr;
        result = sd_bus_message_read(reply.get(), "s", &key);
        std::string* target = nullptr;
        if (result >= 0 && key != nullptr)
        {
            const std::string_view name(key);
            if (name == "dhcp_server_identifier")
            {
                target = &server;
            }
            else if (name == "dhcp_lease_time")
            {
                target = &leaseTime;
            }
            else if (name == "expiry")
            {
                target = &expiry;
            }
        }
        const char* value = nullptr;
        if (result >= 0 && target != nullptr && sd_bus_message_read(reply.get(), "v", "s", &value) >= 0)
        {
            *target = value != nullptr ? Environment::escapeForDisplay(value) : std::string{};
        }
        else if (result >= 0)
        {
            // Another option, or one that isn't a string: skipped whole.
            result = sd_bus_message_skip(reply.get(), "v");
        }
        if (result >= 0)
        {
            result = sd_bus_message_exit_container(reply.get());
        }
    }
    if (result >= 0)
    {
        result = sd_bus_message_exit_container(reply.get());
    }
    return result;
}

[[nodiscard]] LinuxNetworkAdapters::DhcpLeaseRead readDhcpLeaseOnce(NetworkManagerSession& session, const std::string& adapter)
{
    LinuxNetworkAdapters::DhcpLeaseRead read;
    if (!session.opened)
    {
        session.opened = true;
        session.openResult = openSystemBus(session.bus);
        if (session.openResult < 0)
        {
            session.stopped = std::string("The system bus couldn't be reached: ") +
                              std::strerror(-session.openResult); // NOLINT(concurrency-mt-unsafe) - as above
        }
    }
    if (!session.stopped.empty())
    {
        read.error = session.stopped;
        return read;
    }
    sd_bus* bus = session.bus.get();
    const auto fail = [&session, &read](const BusError& error, int result)
    {
        auto [text, forEveryAdapter] = networkManagerError(error, result);
        if (forEveryAdapter)
        {
            session.stopped = text;
        }
        read.error = std::move(text);
        return read;
    };

    BusError error;
    Message reply;
    int result = sd_bus_call_method(bus, NM, NM_PATH, NM, "GetDeviceByIpIface", error.get(), reply.out(), "s", adapter.c_str());
    const char* devicePath = nullptr;
    if (result >= 0)
    {
        result = sd_bus_message_read(reply.get(), "o", &devicePath);
    }
    if (result < 0 || devicePath == nullptr)
    {
        return fail(error, result);
    }
    const std::string device(devicePath);

    // "/" when the adapter's IPv4 address isn't from DHCP.
    std::string config;
    BusError configError;
    if (result = getObjectPath(bus, device.c_str(), NM_DEVICE, "Dhcp4Config", config, configError); result < 0)
    {
        return fail(configError, result);
    }
    if (config.empty() || config == "/")
    {
        read.error = "Not configured by DHCP";
        read.dhcp = false; // a static address, or not up: readNetworkAdapterFacts() tells the two apart
        return read;
    }
    read.dhcp = true;
    std::string server;
    std::string leaseTime;
    std::string expiry;
    BusError optionsError;
    if (result = readDhcpOptions(bus, config.c_str(), server, leaseTime, expiry, optionsError); result < 0)
    {
        return fail(optionsError, result);
    }
    read.lease = LinuxNetworkAdapters::leaseFromDhcpOptions(server, leaseTime, expiry);
    if (!read.lease)
    {
        read.error = "NetworkManager reported no lease details";
    }
    return read;
}

} // namespace

LinuxNetworkAdapters::DhcpLeaseReader makeDhcpLeaseReader()
{
    return [session = std::make_shared<NetworkManagerSession>()](const std::string& adapter)
    {
        return readDhcpLeaseOnce(*session, adapter);
    };
}

#else

LinuxNetworkAdapters::WifiLinkReader makeWifiLinkReader()
{
    return [](const std::string&)
    {
        LinuxNetworkAdapters::WifiLinkRead read;
        read.error = "This build has no D-Bus support (it was built without libsystemd), so the Wi-Fi link can't be read";
        return read;
    };
}

LinuxNetworkAdapters::DhcpLeaseReader makeDhcpLeaseReader()
{
    return [](const std::string&)
    {
        LinuxNetworkAdapters::DhcpLeaseRead read;
        read.error = "This build has no D-Bus support (it was built without libsystemd), so the DHCP lease can't be read";
        return read;
    };
}

#endif

} // namespace Platform::SystemdBus
