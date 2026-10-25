// Each disk's SMART data from udisks2 (org.freedesktop.UDisks2). Split out of SystemdBus.cpp (#2171).

#include "Platform/Linux/LinuxDiskSmart.h"
#include "Platform/Linux/SystemdBus.h"

#include <memory>
#include <optional>
#include <string>

#ifdef TASKSMACK_HAS_SDBUS
#include "Platform/Linux/SdBusCall.h"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
constexpr const char* UDISKS = "org.freedesktop.UDisks2";
constexpr const char* UDISKS_BLOCK = "org.freedesktop.UDisks2.Block";
constexpr const char* UDISKS_ATA = "org.freedesktop.UDisks2.Drive.Ata";
constexpr const char* UDISKS_NVME = "org.freedesktop.UDisks2.NVMe.Controller";

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

/// Whether @p error says the drive just hasn't the interface asked about (an ATA drive asked for NVMe
/// data, or the other way round): GDBus answers InvalidArgs "No such interface", sd-bus services
/// UnknownInterface or UnknownProperty. Anything else -- a timeout, AccessDenied -- is a real failure,
/// not "this isn't that kind of drive" (#2126).
[[nodiscard]] bool isNotThisInterface(const BusError& error) noexcept
{
    return error.is("org.freedesktop.DBus.Error.UnknownInterface") || error.is("org.freedesktop.DBus.Error.UnknownProperty") ||
           error.is("org.freedesktop.DBus.Error.InvalidArgs");
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

/// The drive's NVMe.Controller data into @p smart. The first property's result: negative (and @p error)
/// when it couldn't be read -- the drive isn't an NVMe controller, or udisks2 didn't answer.
[[nodiscard]] int readNvme(sd_bus* bus, const std::string& drive, LinuxDiskSmart::DriveSmart& smart, BusError& error)
{
    if (const int result = getTrivial(bus, drive, UDISKS_NVME, "SmartUpdated", 't', smart.updated, error); result < 0)
    {
        return result;
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
    return 0;
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
    if (const int ataResult = getTrivial(bus, drive, UDISKS_ATA, "SmartSupported", 'b', supported, ataError); ataResult >= 0)
    {
        smart.kind = LinuxDiskSmart::DriveSmart::Kind::Ata;
        smart.smartSupported = supported != 0;
        if (smart.smartSupported && !readAta(bus, drive, smart, read.error))
        {
            return read;
        }
    }
    else if (!isNotThisInterface(ataError))
    {
        // Not an answer about the drive: a timeout reads as UDISKS_TIMED_OUT, so the session stops
        // asking (#1689), rather than as "no SMART data" (#2126).
        read.error = udisksError(ataError, ataResult);
        return read;
    }
    else
    {
        BusError nvmeError;
        if (const int nvmeResult = readNvme(bus, drive, smart, nvmeError); nvmeResult >= 0)
        {
            smart.kind = LinuxDiskSmart::DriveSmart::Kind::Nvme;
        }
        else if (!isNotThisInterface(nvmeError))
        {
            read.error = udisksError(nvmeError, nvmeResult);
            return read;
        }
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

#else

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
