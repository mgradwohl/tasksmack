#include "SystemdBus.h"

#include "Platform/Linux/LinuxDiskSmart.h"
#include "Platform/Linux/LinuxNetworkAdapters.h"
#include "Platform/Linux/SystemdServices.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#ifdef TASKSMACK_HAS_SDBUS
#include "Platform/IServiceProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/NetworkdLinkJson.h"
#include "Platform/ProcessEnvironment.h"

#include <array>
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

bool built() noexcept
{
#ifdef TASKSMACK_HAS_SDBUS
    return true;
#else
    return false;
#endif
}

#ifdef TASKSMACK_HAS_SDBUS

namespace
{

/// Owns an sd_bus connection.
class Bus
{
  public:
    Bus() = default;
    ~Bus()
    {
        if (m_Bus != nullptr)
        {
            sd_bus_flush_close_unref(m_Bus);
        }
    }
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;
    Bus(Bus&&) = delete;
    Bus& operator=(Bus&&) = delete;

    [[nodiscard]] sd_bus** out() noexcept
    {
        return &m_Bus;
    }
    [[nodiscard]] sd_bus* get() const noexcept
    {
        return m_Bus;
    }

  private:
    sd_bus* m_Bus = nullptr;
};

/// Opens @p bus on the system bus with CALL_TIMEOUT_USEC on every call; sd_bus_open_system()'s result.
[[nodiscard]] int openSystemBus(Bus& bus)
{
    const int result = sd_bus_open_system(bus.out());
    if (result >= 0)
    {
        (void) sd_bus_set_method_call_timeout(bus.get(), CALL_TIMEOUT_USEC);
    }
    return result;
}

/// Whether a call failed by running out of time rather than with an answer.
[[nodiscard]] bool timedOut(const sd_bus_error& error, int result) noexcept
{
    const auto named = [&error](std::string_view name)
    {
        return error.name != nullptr && name == error.name;
    };
    return result == -ETIMEDOUT || named("org.freedesktop.DBus.Error.Timeout") || named("org.freedesktop.DBus.Error.NoReply");
}

/// The Manager's unsigned 64-bit property @p name into @p value; the error text when it fails.
[[nodiscard]] std::string readManagerUint64(sd_bus* bus, const char* name, std::uint64_t& value)
{
    sd_bus_error error{}; // SD_BUS_ERROR_NULL: all null, written as C++ (the macro is a C compound literal)
    const int result = sd_bus_get_property_trivial(
        bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", name, &error, 't', &value);
    std::string text;
    if (result < 0 && timedOut(error, result))
    {
        text = "systemd didn't answer in time";
    }
    else if (result < 0)
    {
        text = (error.message != nullptr) ? error.message
                                          : std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - read on one worker thread
    }
    sd_bus_error_free(&error);
    return text;
}

} // namespace

BootTimestampsRead readBootTimestamps()
{
    BootTimestampsRead read;
    Bus bus;
    if (const int result = openSystemBus(bus); result < 0)
    {
        read.error =
            std::string("The system bus couldn't be reached: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - as above
        return read;
    }
    struct Property
    {
        const char* name;
        std::uint64_t* value;
    };
    const std::array properties{
        Property{.name = "FirmwareTimestampMonotonic", .value = &read.firmware},
        Property{.name = "LoaderTimestampMonotonic", .value = &read.loader},
        Property{.name = "InitRDTimestampMonotonic", .value = &read.initrd},
        Property{.name = "UserspaceTimestampMonotonic", .value = &read.userspace},
        Property{.name = "FinishTimestampMonotonic", .value = &read.finish},
    };
    for (const Property& property : properties)
    {
        if (std::string error = readManagerUint64(bus.get(), property.name, *property.value); !error.empty())
        {
            read.error = std::move(error);
            return read;
        }
    }
    read.ok = true;
    return read;
}

namespace
{
constexpr const char* UDISKS = "org.freedesktop.UDisks2";
constexpr const char* UDISKS_BLOCK = "org.freedesktop.UDisks2.Block";
constexpr const char* UDISKS_ATA = "org.freedesktop.UDisks2.Drive.Ata";
constexpr const char* UDISKS_NVME = "org.freedesktop.UDisks2.NVMe.Controller";

/// Owns an sd_bus_error.
class BusError
{
  public:
    BusError() = default;
    ~BusError()
    {
        sd_bus_error_free(&m_Error);
    }
    BusError(const BusError&) = delete;
    BusError& operator=(const BusError&) = delete;
    BusError(BusError&&) = delete;
    BusError& operator=(BusError&&) = delete;

    [[nodiscard]] sd_bus_error* get() noexcept
    {
        return &m_Error;
    }

    [[nodiscard]] const sd_bus_error& raw() const noexcept
    {
        return m_Error;
    }

    [[nodiscard]] bool is(const char* name) const noexcept
    {
        return m_Error.name != nullptr && std::string_view(m_Error.name) == name;
    }

    /// The error's message, or @p result's errno text when the bus gave none.
    [[nodiscard]] std::string text(int result) const
    {
        return (m_Error.message != nullptr) ? std::string(m_Error.message)
                                            : std::string(std::strerror(-result)); // NOLINT(concurrency-mt-unsafe) - one worker thread
    }

  private:
    sd_bus_error m_Error{}; // SD_BUS_ERROR_NULL, written as C++
};

/// Owns an sd_bus_message.
class Message
{
  public:
    Message() = default;
    ~Message()
    {
        sd_bus_message_unref(m_Message);
    }
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    Message(Message&&) = delete;
    Message& operator=(Message&&) = delete;

    [[nodiscard]] sd_bus_message** out() noexcept
    {
        return &m_Message;
    }

    [[nodiscard]] sd_bus_message* get() const noexcept
    {
        return m_Message;
    }

  private:
    sd_bus_message* m_Message = nullptr;
};

/// One system-bus connection for every disk of a read, opened on the first.
struct UDisksSession
{
    Bus bus;
    bool opened = false;
    int openResult = 0;
    bool timedOut = false; ///< A call ran out of time: the rest of this read doesn't ask again (#1689)
};

/// What a disk says once udisks2 has stopped answering.
constexpr const char* UDISKS_TIMED_OUT = "udisks2 didn't answer in time, so SMART status wasn't read";

/// A trivial-typed udisks2 property into @p value.
template<typename T>
[[nodiscard]] int
getTrivial(sd_bus* bus, const std::string& path, const char* interface, const char* name, char type, T& value, BusError& error)
{
    return sd_bus_get_property_trivial(bus, UDISKS, path.c_str(), interface, name, error.get(), type, &value);
}

/// Why udisks2 couldn't answer for a disk, in the user's terms.
[[nodiscard]] std::string udisksError(const BusError& error, int result)
{
    if (timedOut(error.raw(), result))
    {
        return UDISKS_TIMED_OUT;
    }
    if (error.is("org.freedesktop.DBus.Error.ServiceUnknown") || error.is("org.freedesktop.DBus.Error.NameHasNoOwner"))
    {
        return "udisks2 isn't running, so SMART status can't be read";
    }
    if (error.is("org.freedesktop.DBus.Error.UnknownObject") || error.is("org.freedesktop.DBus.Error.UnknownMethod"))
    {
        return "udisks2 doesn't list this disk";
    }
    return "udisks2 couldn't be asked: " + error.text(result);
}

/// The drive's Drive.Ata properties into @p smart; false (and @p error) when one can't be read.
[[nodiscard]] bool readAta(sd_bus* bus, const std::string& drive, LinuxDiskSmart::DriveSmart& smart, std::string& error)
{
    int enabled = 0;
    int failing = 0;
    BusError busError;
    int result = getTrivial(bus, drive, UDISKS_ATA, "SmartEnabled", 'b', enabled, busError);
    if (result >= 0)
    {
        result = getTrivial(bus, drive, UDISKS_ATA, "SmartFailing", 'b', failing, busError);
    }
    if (result >= 0)
    {
        result = getTrivial(bus, drive, UDISKS_ATA, "SmartUpdated", 't', smart.updated, busError);
    }
    if (result >= 0)
    {
        result = getTrivial(bus, drive, UDISKS_ATA, "SmartTemperature", 'd', smart.temperatureKelvin, busError);
    }
    if (result >= 0)
    {
        result = getTrivial(bus, drive, UDISKS_ATA, "SmartPowerOnSeconds", 't', smart.powerOnSeconds, busError);
    }
    if (result >= 0)
    {
        result = getTrivial(bus, drive, UDISKS_ATA, "SmartNumBadSectors", 'x', smart.badSectors, busError);
    }
    if (result < 0)
    {
        error = udisksError(busError, result);
        return false;
    }
    smart.smartEnabled = enabled != 0;
    smart.failing = failing != 0;
    return true;
}

/// The NVMe health log from SmartGetAttributes() into @p smart.
void readNvmeAttributes(sd_bus* bus, const std::string& drive, LinuxDiskSmart::DriveSmart& smart)
{
    BusError error;
    Message reply;
    int result = sd_bus_call_method(bus, UDISKS, drive.c_str(), UDISKS_NVME, "SmartGetAttributes", error.get(), reply.out(), "a{sv}", 0);
    if (result < 0)
    {
        smart.attributesError = timedOut(error.raw(), result) ? std::string("it didn't answer in time") : error.text(result);
        return;
    }
    result = sd_bus_message_enter_container(reply.get(), 'a', "{sv}");
    while (result >= 0 && (result = sd_bus_message_enter_container(reply.get(), 'e', "sv")) > 0)
    {
        const char* key = nullptr;
        result = sd_bus_message_read(reply.get(), "s", &key);
        const std::string_view name = (result >= 0 && key != nullptr) ? std::string_view(key) : std::string_view{};
        if (name == "percent_used")
        {
            result = sd_bus_message_read(reply.get(), "v", "y", &smart.percentUsed);
        }
        else if (name == "avail_spare")
        {
            result = sd_bus_message_read(reply.get(), "v", "y", &smart.availableSpare);
        }
        else if (name == "media_errors")
        {
            result = sd_bus_message_read(reply.get(), "v", "t", &smart.mediaErrors);
        }
        else if (result >= 0)
        {
            result = sd_bus_message_skip(reply.get(), "v");
        }
        if (result >= 0)
        {
            result = sd_bus_message_exit_container(reply.get());
        }
    }
    if (result < 0)
    {
        smart.attributesError =
            std::string("its reply couldn't be read: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - as above
        return;
    }
    smart.attributesRead = true;
}

/// The drive's NVMe.Controller data into @p smart; false when it isn't an NVMe controller.
[[nodiscard]] bool readNvme(sd_bus* bus, const std::string& drive, LinuxDiskSmart::DriveSmart& smart)
{
    BusError error;
    if (getTrivial(bus, drive, UDISKS_NVME, "SmartUpdated", 't', smart.updated, error) < 0)
    {
        return false;
    }
    char** warnings = nullptr;
    BusError warningsError;
    if (sd_bus_get_property_strv(bus, UDISKS, drive.c_str(), UDISKS_NVME, "SmartCriticalWarning", warningsError.get(), &warnings) >= 0 &&
        warnings != nullptr)
    {
        for (char** warning = warnings; *warning != nullptr;
             ++warning) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - a C strv
        {
            smart.criticalWarnings.emplace_back(*warning);
            free(*warning); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory) - sd-bus allocates with malloc
        }
        free(static_cast<void*>(warnings)); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory) - as above
    }
    BusError temperatureError;
    (void) getTrivial(bus, drive, UDISKS_NVME, "SmartTemperature", 'q', smart.nvmeTemperatureKelvin, temperatureError);
    readNvmeAttributes(bus, drive, smart);
    return true;
}

[[nodiscard]] LinuxDiskSmart::DriveSmartRead readDriveSmartOnce(UDisksSession& session, const std::string& blockName)
{
    LinuxDiskSmart::DriveSmartRead read;
    if (!session.opened)
    {
        session.opened = true;
        session.openResult = openSystemBus(session.bus);
    }
    if (session.openResult < 0)
    {
        read.error = std::string("The system bus couldn't be reached: ") +
                     std::strerror(-session.openResult); // NOLINT(concurrency-mt-unsafe) - as above
        return read;
    }
    sd_bus* bus = session.bus.get();

    // The block device's Drive: "/" when it has none.
    BusError error;
    Message reply;
    const std::string block = LinuxDiskSmart::blockObjectPath(blockName);
    int result = sd_bus_get_property(bus, UDISKS, block.c_str(), UDISKS_BLOCK, "Drive", error.get(), reply.out(), "o");
    const char* drivePath = nullptr;
    if (result >= 0)
    {
        result = sd_bus_message_read(reply.get(), "o", &drivePath);
    }
    if (result < 0 || drivePath == nullptr)
    {
        read.error = udisksError(error, result);
        return read;
    }
    LinuxDiskSmart::DriveSmart smart;
    const std::string drive(drivePath);
    if (drive == "/")
    {
        read.smart = smart; // Kind::None
        return read;
    }

    int supported = 0;
    BusError ataError;
    if (getTrivial(bus, drive, UDISKS_ATA, "SmartSupported", 'b', supported, ataError) >= 0)
    {
        smart.kind = LinuxDiskSmart::DriveSmart::Kind::Ata;
        smart.smartSupported = supported != 0;
        if (smart.smartSupported && !readAta(bus, drive, smart, read.error))
        {
            return read;
        }
    }
    else if (readNvme(bus, drive, smart))
    {
        smart.kind = LinuxDiskSmart::DriveSmart::Kind::Nvme;
    }
    read.smart = std::move(smart);
    return read;
}
[[nodiscard]] LinuxDiskSmart::DriveSmartRead readDriveSmart(UDisksSession& session, const std::string& blockName)
{
    if (session.timedOut)
    {
        return {.smart = std::nullopt, .error = UDISKS_TIMED_OUT};
    }
    LinuxDiskSmart::DriveSmartRead read = readDriveSmartOnce(session, blockName);
    session.timedOut = read.error == UDISKS_TIMED_OUT;
    return read;
}
} // namespace

LinuxDiskSmart::SmartReader makeDriveSmartReader()
{
    return [session = std::make_shared<UDisksSession>()](const std::string& blockName)
    {
        return readDriveSmart(*session, blockName);
    };
}

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
        return read;
    }
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

namespace
{

constexpr const char* SYSTEMD = "org.freedesktop.systemd1";
constexpr const char* SYSTEMD_PATH = "/org/freedesktop/systemd1";
constexpr const char* SYSTEMD_MANAGER = "org.freedesktop.systemd1.Manager";
constexpr const char* SYSTEMD_SERVICE = "org.freedesktop.systemd1.Service";

/// Why systemd couldn't answer, in the user's terms.
[[nodiscard]] std::string systemdError(const BusError& error, int result)
{
    if (timedOut(error.raw(), result))
    {
        return "systemd didn't answer in time";
    }
    if (error.is("org.freedesktop.DBus.Error.ServiceUnknown") || error.is("org.freedesktop.DBus.Error.NameHasNoOwner"))
    {
        return "systemd isn't running";
    }
    return "systemd couldn't be asked: " + error.text(result);
}

/// The loaded services from ListUnits' a(ssssssouso) reply into @p units; a negative sd-bus result when it is malformed.
[[nodiscard]] int readUnits(sd_bus_message* reply, std::vector<SystemdServices::Unit>& units)
{
    int result = sd_bus_message_enter_container(reply, 'a', "(ssssssouso)");
    if (result < 0)
    {
        return result;
    }
    for (;;)
    {
        const char* name = nullptr;
        const char* description = nullptr;
        const char* loadState = nullptr;
        const char* activeState = nullptr;
        const char* subState = nullptr;
        const char* following = nullptr;
        const char* path = nullptr;
        std::uint32_t jobId = 0;
        const char* jobType = nullptr;
        const char* jobPath = nullptr;
        result = sd_bus_message_read(
            reply, "(ssssssouso)", &name, &description, &loadState, &activeState, &subState, &following, &path, &jobId, &jobType, &jobPath);
        if (result <= 0)
        {
            break;
        }
        if (name != nullptr && SystemdServices::isService(name))
        {
            units.push_back({.name = name,
                             .description = (description != nullptr) ? description : "",
                             .activeState = (activeState != nullptr) ? activeState : "",
                             .subState = (subState != nullptr) ? subState : "",
                             .objectPath = (path != nullptr) ? path : ""});
        }
    }
    if (result < 0)
    {
        return result;
    }
    return sd_bus_message_exit_container(reply);
}

/// The installed service files from ListUnitFiles' a(ss) reply (path, state) into @p files, by file name.
[[nodiscard]] int readUnitFiles(sd_bus_message* reply, std::vector<std::pair<std::string, std::string>>& files)
{
    int result = sd_bus_message_enter_container(reply, 'a', "(ss)");
    if (result < 0)
    {
        return result;
    }
    for (;;)
    {
        const char* path = nullptr;
        const char* state = nullptr;
        result = sd_bus_message_read(reply, "(ss)", &path, &state);
        if (result <= 0)
        {
            break;
        }
        const std::string_view full = (path != nullptr) ? path : "";
        const std::string_view name = full.substr(full.rfind('/') == std::string_view::npos ? 0 : full.rfind('/') + 1);
        // Templates ("getty@.service") too: their instances take their enablement (SystemdServices::services()).
        if (name.ends_with(SystemdServices::SERVICE_SUFFIX))
        {
            files.emplace_back(std::string(name), (state != nullptr) ? state : "");
        }
    }
    if (result < 0)
    {
        return result;
    }
    return sd_bus_message_exit_container(reply);
}

} // namespace

ServicesRead readServices()
{
    ServicesRead read;
    Bus bus;
    if (const int result = openSystemBus(bus); result < 0)
    {
        read.error = std::string("The system bus couldn't be reached: ") +
                     std::strerror(-result); // NOLINT(concurrency-mt-unsafe) - one worker thread
        return read;
    }
    {
        BusError error;
        Message reply;
        int result = sd_bus_call_method(bus.get(), SYSTEMD, SYSTEMD_PATH, SYSTEMD_MANAGER, "ListUnits", error.get(), reply.out(), "");
        if (result < 0)
        {
            read.error = systemdError(error, result);
            return read;
        }
        if (result = readUnits(reply.get(), read.listing.units); result < 0)
        {
            read.error = std::string("systemd's unit list couldn't be read: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe)
            return read;
        }
    }
    {
        BusError error;
        Message reply;
        int result = sd_bus_call_method(bus.get(), SYSTEMD, SYSTEMD_PATH, SYSTEMD_MANAGER, "ListUnitFiles", error.get(), reply.out(), "");
        if (result < 0)
        {
            read.error = systemdError(error, result);
            return read;
        }
        if (result = readUnitFiles(reply.get(), read.listing.unitFiles); result < 0)
        {
            read.error = std::string("systemd's unit files couldn't be read: ") + std::strerror(-result); // NOLINT(concurrency-mt-unsafe)
            return read;
        }
    }
    // Each running service's main process: one property per service, so only for those that have one.
    for (const SystemdServices::Unit& unit : read.listing.units)
    {
        if (unit.objectPath.empty() || SystemdServices::stateFrom(unit.activeState) == ServiceState::Stopped)
        {
            continue;
        }
        BusError error;
        std::uint32_t pid = 0;
        if (sd_bus_get_property_trivial(bus.get(), SYSTEMD, unit.objectPath.c_str(), SYSTEMD_SERVICE, "MainPID", error.get(), 'u', &pid) >=
                0 &&
            pid != 0)
        {
            read.listing.mainPids.emplace(unit.name, pid);
        }
    }
    read.ok = true;
    return read;
}

#else

BootTimestampsRead readBootTimestamps()
{
    BootTimestampsRead read;
    read.error = "This build has no systemd support (it was built without libsystemd)";
    return read;
}

LinuxDiskSmart::SmartReader makeDriveSmartReader()
{
    return [](const std::string&)
    {
        LinuxDiskSmart::DriveSmartRead read;
        read.error = "This build has no D-Bus support (it was built without libsystemd), so SMART status can't be read";
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

LinuxNetworkAdapters::WifiLinkReader makeWifiLinkReader()
{
    return [](const std::string&)
    {
        LinuxNetworkAdapters::WifiLinkRead read;
        read.error = "This build has no D-Bus support (it was built without libsystemd), so the Wi-Fi link can't be read";
        return read;
    };
}

ServicesRead readServices()
{
    ServicesRead read;
    read.error = "This build has no systemd support (it was built without libsystemd)";
    return read;
}

#endif

} // namespace Platform::SystemdBus
