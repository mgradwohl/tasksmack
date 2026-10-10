#include "SystemdBus.h"

#include "Platform/Linux/LinuxDiskSmart.h"

#include <cstdint>
#include <memory>
#include <string>

#ifdef TASKSMACK_HAS_SDBUS
#include <array>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

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

/// The Manager's unsigned 64-bit property @p name into @p value; the error text when it fails.
[[nodiscard]] std::string readManagerUint64(sd_bus* bus, const char* name, std::uint64_t& value)
{
    sd_bus_error error{}; // SD_BUS_ERROR_NULL: all null, written as C++ (the macro is a C compound literal)
    const int result = sd_bus_get_property_trivial(
        bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", name, &error, 't', &value);
    std::string text;
    if (result < 0)
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
    if (const int result = sd_bus_open_system(bus.out()); result < 0)
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
};

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
        smart.attributesError = error.text(result);
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

[[nodiscard]] LinuxDiskSmart::DriveSmartRead readDriveSmart(UDisksSession& session, const std::string& blockName)
{
    LinuxDiskSmart::DriveSmartRead read;
    if (!session.opened)
    {
        session.opened = true;
        session.openResult = sd_bus_open_system(session.bus.out());
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
} // namespace

LinuxDiskSmart::SmartReader makeDriveSmartReader()
{
    return [session = std::make_shared<UDisksSession>()](const std::string& blockName)
    {
        return readDriveSmart(*session, blockName);
    };
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

#endif

} // namespace Platform::SystemdBus
