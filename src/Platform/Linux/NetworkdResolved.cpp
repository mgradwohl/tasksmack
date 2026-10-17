// systemd-networkd's DHCP leases and systemd-resolved's per-link DNS, over D-Bus. Split out of SystemdBus.cpp (#2171).

#include "Platform/Linux/LinuxNetworkAdapters.h"
#include "Platform/Linux/SystemdBus.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#ifdef TASKSMACK_HAS_SDBUS
#include "Platform/Linux/NetworkdLinkJson.h"
#include "Platform/Linux/SdBusCall.h"
#include "Platform/ProcessEnvironment.h"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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

constexpr const char* NETWORKD = "org.freedesktop.network1";
constexpr const char* NETWORKD_PATH = "/org/freedesktop/network1";
constexpr const char* NETWORKD_MANAGER = "org.freedesktop.network1.Manager";
constexpr const char* NETWORKD_LINK = "org.freedesktop.network1.Link";

/// One system-bus connection for every adapter of a read, opened on the first.
struct NetworkdSession
{
    Bus bus;
    bool opened = false;
    std::string stopped; ///< Set once networkd is found absent or unresponsive: every later adapter gets it.
};

/// Why networkd couldn't answer for an adapter, and whether that holds for every other adapter too.
[[nodiscard]] std::pair<std::string, bool> networkdError(const BusError& error, int result)
{
    if (timedOut(error.raw(), result))
    {
        return {"systemd-networkd didn't answer in time", true};
    }
    if (error.is("org.freedesktop.DBus.Error.ServiceUnknown") || error.is("org.freedesktop.DBus.Error.NameHasNoOwner"))
    {
        return {"systemd-networkd isn't running", true};
    }
    if (error.is("org.freedesktop.network1.NoSuchLink") || error.is("org.freedesktop.DBus.Error.UnknownObject"))
    {
        return {"systemd-networkd doesn't manage this adapter", false};
    }
    return {"systemd-networkd couldn't be asked: " + error.text(result), false};
}

/// A call to networkd that won't start it if it isn't running: reading the page mustn't start a
/// network manager the user has stopped (no D-Bus auto-start).
[[nodiscard]] int callNetworkd(sd_bus* bus,
                               const char* path,
                               const char* interface,
                               const char* member,
                               const std::optional<int>& ifindex,
                               BusError& error,
                               Message& reply)
{
    Message call;
    int result = sd_bus_message_new_method_call(bus, call.out(), NETWORKD, path, interface, member);
    if (result >= 0)
    {
        result = sd_bus_message_set_auto_start(call.get(), 0);
    }
    if (result >= 0 && ifindex.has_value())
    {
        result = sd_bus_message_append(call.get(), "i", *ifindex);
    }
    if (result >= 0)
    {
        result = sd_bus_call(bus, call.get(), 0, error.get(), reply.out()); // 0: the bus's CALL_TIMEOUT_USEC
    }
    return result;
}

/// CLOCK_BOOTTIME now, in microseconds: networkd's lease times are on it.
[[nodiscard]] std::uint64_t bootTimeUsec() noexcept
{
    timespec now{};
    // CLOCK_BOOTTIME comes from glibc's internal <bits/time.h>, through <ctime>; there's no public header of its own.
    if (::clock_gettime(CLOCK_BOOTTIME, &now) != 0) // NOLINT(misc-include-cleaner)
    {
        return 0;
    }
    return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000U) + (static_cast<std::uint64_t>(now.tv_nsec) / 1'000U);
}

[[nodiscard]] LinuxNetworkAdapters::DhcpLeaseRead readNetworkdLeaseOnce(NetworkdSession& session, int ifindex)
{
    LinuxNetworkAdapters::DhcpLeaseRead read;
    if (!session.opened)
    {
        session.opened = true;
        if (const int result = openSystemBus(session.bus); result < 0)
        {
            session.stopped =
                std::string("The system bus couldn't be reached: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - as above
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
        auto [text, forEveryAdapter] = networkdError(error, result);
        if (forEveryAdapter)
        {
            session.stopped = text;
        }
        read.error = std::move(text);
        return read;
    };

    // The link's object: GetLinkByIndex() returns its name and path.
    BusError error;
    Message reply;
    int result = callNetworkd(bus, NETWORKD_PATH, NETWORKD_MANAGER, "GetLinkByIndex", ifindex, error, reply);
    const char* name = nullptr;
    const char* linkPath = nullptr;
    if (result >= 0)
    {
        result = sd_bus_message_read(reply.get(), "so", &name, &linkPath);
    }
    if (result < 0 || linkPath == nullptr)
    {
        return fail(error, result);
    }
    const std::string link(linkPath);

    BusError describeError;
    Message described;
    result = callNetworkd(bus, link.c_str(), NETWORKD_LINK, "Describe", std::nullopt, describeError, described);
    const char* json = nullptr;
    if (result >= 0)
    {
        result = sd_bus_message_read(described.get(), "s", &json);
    }
    if (result < 0 || json == nullptr)
    {
        return fail(describeError, result);
    }
    const std::optional<NetworkdLinkJson::Lease> lease = NetworkdLinkJson::leaseFromDescription(json);
    const std::time_t now = std::time(nullptr);
    if (lease.has_value())
    {
        read.lease = NetworkdLinkJson::toDhcpLease(*lease, now > 0 ? static_cast<std::uint64_t>(now) : 0, bootTimeUsec());
    }
    if (!read.lease)
    {
        read.error = "Not configured by DHCP";
    }
    return read;
}

} // namespace

LinuxNetworkAdapters::IndexedDhcpLeaseReader makeNetworkdLeaseReader()
{
    return [session = std::make_shared<NetworkdSession>()](int ifindex)
    {
        return readNetworkdLeaseOnce(*session, ifindex);
    };
}

namespace
{

constexpr const char* RESOLVE = "org.freedesktop.resolve1";
constexpr const char* RESOLVE_PATH = "/org/freedesktop/resolve1";
constexpr const char* RESOLVE_MANAGER = "org.freedesktop.resolve1.Manager";
constexpr const char* RESOLVE_LINK = "org.freedesktop.resolve1.Link";

/// One system-bus connection for every adapter of a read, opened on the first.
struct ResolvedSession
{
    Bus bus;
    bool opened = false;
    std::string stopped; ///< Set once resolved is found absent or unresponsive: every later adapter gets it.
};

/// Why resolved couldn't answer for an adapter, and whether that holds for every other adapter too.
[[nodiscard]] std::pair<std::string, bool> resolvedError(const BusError& error, int result)
{
    if (timedOut(error.raw(), result))
    {
        return {"systemd-resolved didn't answer in time", true};
    }
    if (error.is("org.freedesktop.DBus.Error.ServiceUnknown") || error.is("org.freedesktop.DBus.Error.NameHasNoOwner"))
    {
        return {"systemd-resolved isn't running", true};
    }
    if (error.is("org.freedesktop.resolve1.NoSuchLink"))
    {
        return {"systemd-resolved doesn't know this adapter", false};
    }
    return {"systemd-resolved couldn't be asked: " + error.text(result), false};
}

/// The Link's DNS property, a(iay): each server's address family and bytes.
[[nodiscard]] int readLinkServers(sd_bus* bus, const char* link, std::vector<std::string>& servers, BusError& error)
{
    Message reply;
    int result = sd_bus_get_property(bus, RESOLVE, link, RESOLVE_LINK, "DNS", error.get(), reply.out(), "a(iay)");
    if (result >= 0)
    {
        result = sd_bus_message_enter_container(reply.get(), 'a', "(iay)");
    }
    while (result >= 0 && (result = sd_bus_message_enter_container(reply.get(), 'r', "iay")) > 0)
    {
        int family = 0;
        const void* bytes = nullptr;
        std::size_t size = 0;
        result = sd_bus_message_read(reply.get(), "i", &family);
        if (result >= 0)
        {
            result = sd_bus_message_read_array(reply.get(), 'y', &bytes, &size);
        }
        if (result >= 0 && bytes != nullptr)
        {
            if (std::string address = LinuxNetworkAdapters::formatAddressBytes(
                    family, std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(bytes), size));
                !address.empty())
            {
                servers.push_back(std::move(address));
            }
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

/// The Link's Domains property, a(sb): each domain and whether it only routes lookups ("~domain").
[[nodiscard]] int readLinkDomains(sd_bus* bus, const char* link, std::vector<std::string>& domains, BusError& error)
{
    Message reply;
    int result = sd_bus_get_property(bus, RESOLVE, link, RESOLVE_LINK, "Domains", error.get(), reply.out(), "a(sb)");
    if (result >= 0)
    {
        result = sd_bus_message_enter_container(reply.get(), 'a', "(sb)");
    }
    while (result >= 0 && (result = sd_bus_message_enter_container(reply.get(), 'r', "sb")) > 0)
    {
        const char* name = nullptr;
        int routeOnly = 0;
        result = sd_bus_message_read(reply.get(), "sb", &name, &routeOnly);
        if (result >= 0 && name != nullptr && *name != '\0')
        {
            domains.push_back((routeOnly != 0 ? "~" : "") + Environment::escapeForDisplay(name));
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

[[nodiscard]] LinuxNetworkAdapters::LinkDnsRead readLinkDnsOnce(ResolvedSession& session, int ifindex)
{
    LinuxNetworkAdapters::LinkDnsRead read;
    if (!session.opened)
    {
        session.opened = true;
        if (const int result = openSystemBus(session.bus); result < 0)
        {
            session.stopped =
                std::string("The system bus couldn't be reached: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - as above
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
        auto [text, forEveryAdapter] = resolvedError(error, result);
        if (forEveryAdapter)
        {
            session.stopped = text;
        }
        read.servers.clear();
        read.domains.clear();
        read.error = std::move(text);
        return read;
    };

    BusError error;
    Message reply;
    int result = sd_bus_call_method(bus, RESOLVE, RESOLVE_PATH, RESOLVE_MANAGER, "GetLink", error.get(), reply.out(), "i", ifindex);
    const char* linkPath = nullptr;
    if (result >= 0)
    {
        result = sd_bus_message_read(reply.get(), "o", &linkPath);
    }
    if (result < 0 || linkPath == nullptr)
    {
        return fail(error, result);
    }
    const std::string link(linkPath);
    BusError serversError;
    if (result = readLinkServers(bus, link.c_str(), read.servers, serversError); result < 0)
    {
        return fail(serversError, result);
    }
    BusError domainsError;
    if (result = readLinkDomains(bus, link.c_str(), read.domains, domainsError); result < 0)
    {
        return fail(domainsError, result);
    }
    read.read = true;
    return read;
}

} // namespace

LinuxNetworkAdapters::LinkDnsReader makeLinkDnsReader()
{
    return [session = std::make_shared<ResolvedSession>()](int ifindex)
    {
        return readLinkDnsOnce(*session, ifindex);
    };
}

#else

LinuxNetworkAdapters::IndexedDhcpLeaseReader makeNetworkdLeaseReader()
{
    return [](int)
    {
        LinuxNetworkAdapters::DhcpLeaseRead read;
        read.error = "This build has no D-Bus support (it was built without libsystemd), so systemd-networkd can't be asked";
        return read;
    };
}

LinuxNetworkAdapters::LinkDnsReader makeLinkDnsReader()
{
    return [](int)
    {
        LinuxNetworkAdapters::LinkDnsRead read;
        read.error = "This build has no D-Bus support (it was built without libsystemd), so systemd-resolved can't be asked";
        return read;
    };
}

#endif

} // namespace Platform::SystemdBus
