#pragma once

// The System Information page's static facts (#1399), read once (and again on Refresh) off the UI
// thread. Each section of the page has a raw struct here and a read*() method on the probe; the App
// layer turns them into labelled rows (App/Panels/SystemInfoSections.h).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

/// Which OS the facts come from; it picks the rows the Operating system section shows.
enum class OsFamily : std::uint8_t
{
    Unknown,
    Windows,
    Linux,
};

/// The Operating system & session facts (#1512), raw as the OS reports them. A string the probe could
/// not read is left empty and a time 0; the page shows those as unavailable.
struct OsInfo
{
    OsFamily family = OsFamily::Unknown;
    std::string name;                     ///< "Windows 11 Home" / os-release PRETTY_NAME
    std::string version;                  ///< Windows DisplayVersion ("25H2") / os-release VERSION_ID
    std::string build;                    ///< Windows "CurrentBuild.UBR"
    std::string kernel;                   ///< Linux: uname's sysname and release
    std::string architecture;             ///< The machine's native architecture ("x64", "ARM64", "x86_64")
    std::uint64_t installUnixSeconds = 0; ///< Windows InstallDate
    std::uint64_t bootUnixSeconds = 0;
    std::string computerName;            ///< DNS host name
    std::string domainOrWorkgroup;       ///< Windows: the domain or workgroup joined
    bool joinedToDomain = false;         ///< domainOrWorkgroup is a domain, not a workgroup
    std::string userName;                ///< Windows DOMAIN\user, Linux the login name
    std::string locale;                  ///< "en-US" / "en_US.UTF-8"
    std::string timeZone;                ///< "Pacific Standard Time" / "America/Los_Angeles"
    std::optional<int> utcOffsetMinutes; ///< The current offset east of UTC, daylight saving included
    std::string systemDirectory;         ///< Windows
    std::string windowsDirectory;        ///< Windows
    std::string initSystem;              ///< Linux: /proc/1/comm
    std::string desktop;                 ///< Linux: XDG_CURRENT_DESKTOP
    std::string sessionType;             ///< Linux: XDG_SESSION_TYPE ("wayland", "x11", "tty")
    std::string virtualization;          ///< Linux: a container or VM hint, or "None detected"
};

/// How the firmware booted the OS.
enum class FirmwareMode : std::uint8_t
{
    Unknown,
    Uefi,
    Legacy, ///< BIOS / CSM
};

/// The Firmware & board facts (#1513), from SMBIOS types 0-3 (Windows) or /sys/class/dmi/id (Linux).
/// A string the probe could not read is left empty.
struct FirmwareInfo
{
    bool available = false; ///< The probe read the section at all.
    std::string systemManufacturer;
    std::string systemModel;
    std::string systemVersion;
    std::string systemSku;
    std::string systemFamily;
    std::string systemSerial; ///< An identifier
    std::string systemUuid;   ///< An identifier
    std::string biosVendor;
    std::string biosVersion;
    std::string biosReleaseDate; ///< As the firmware writes it ("05/14/2024")
    FirmwareMode firmwareMode = FirmwareMode::Unknown;
    std::string smbiosVersion;             ///< "3.4"
    std::string embeddedControllerVersion; ///< "1.23"; empty without an embedded controller
    std::string boardManufacturer;
    std::string boardProduct;
    std::string boardVersion;
    std::string boardSerial; ///< An identifier
    std::string chassisManufacturer;
    std::string chassisType;              ///< Decoded: "Desktop", "Notebook", ...
    std::string platformRole;             ///< "Desktop", "Mobile", "Server", ...
    bool identifiersNeedAdmin = false;    ///< Linux: the serials and UUID exist but are readable by root only.
    bool smbiosVersionNeedsAdmin = false; ///< Linux: the SMBIOS entry point exists but is readable by root only.
};

/// One populated memory device (SMBIOS type 17, #1515), decoded but unformatted. A fact the table
/// doesn't give is left empty or 0.
struct MemoryModule
{
    std::string locator;                  ///< The slot or device locator ("DIMM A1", "ChannelA-DIMM0")
    std::string bankLocator;              ///< "BANK 0", "P0 CHANNEL A"
    std::uint64_t sizeBytes = 0;          ///< 0 when the module reports an unknown size
    std::string type;                     ///< "DDR5", "LPDDR5", ...; empty when unknown
    std::string formFactor;               ///< "DIMM", "SODIMM", "Row of chips", ...; empty when unknown
    std::uint32_t speedMts = 0;           ///< The rated (maximum) speed in MT/s
    std::uint32_t configuredSpeedMts = 0; ///< The speed it runs at, in MT/s
    std::string manufacturer;
    std::string partNumber;
};

/// The Memory modules facts (#1515): the populated modules and slots from SMBIOS types 16 and 17, and
/// installed against usable memory.
struct MemoryModulesInfo
{
    bool available = false;             ///< The probe read the section at all.
    bool tableRead = false;             ///< The SMBIOS table was read: modules and slotCount are meaningful.
    bool tableNeedsAdmin = false;       ///< Linux: the SMBIOS table exists but is readable by root only.
    std::vector<MemoryModule> modules;  ///< Populated slots only, in table order
    std::uint32_t slotCount = 0;        ///< Every memory device slot, empty ones included; 0 when unknown
    std::uint64_t maxCapacityBytes = 0; ///< The system memory arrays' maximum capacity; 0 when unknown
    std::uint64_t installedBytes = 0;   ///< Physically installed, as the OS reports it (Windows); 0 when unknown
    std::uint64_t usableBytes = 0;      ///< What the OS can use (installed less hardware-reserved); 0 when unknown
};

/// What the platform can read at all.
struct SystemInfoCapabilities
{
    bool hasOs = false;            ///< readOs() and readFirmware() return facts.
    std::string unavailableReason; ///< Why hasOs is false, for the UI. Empty when it is true.
};

/// Reads the System Information page's static facts. Called from one thread at a time, which may
/// differ between calls; each read is a fresh read, nothing is cached.
class ISystemInfoProbe
{
  public:
    virtual ~ISystemInfoProbe() = default;

    ISystemInfoProbe() = default;
    ISystemInfoProbe(const ISystemInfoProbe&) = default;
    ISystemInfoProbe& operator=(const ISystemInfoProbe&) = default;
    ISystemInfoProbe(ISystemInfoProbe&&) = default;
    ISystemInfoProbe& operator=(ISystemInfoProbe&&) = default;

    [[nodiscard]] virtual SystemInfoCapabilities capabilities() const = 0;

    /// The Operating system & session facts; a fact that can't be read is left empty.
    [[nodiscard]] virtual OsInfo readOs() = 0;

    /// The Firmware & board facts (#1513); read when hasOs is true.
    [[nodiscard]] virtual FirmwareInfo readFirmware() = 0;

    /// The Memory modules facts (#1515); read when hasOs is true.
    [[nodiscard]] virtual MemoryModulesInfo readMemoryModules() = 0;
};

/// The probe for a platform without an implementation: no facts, and hasOs false so the UI says so.
class UnsupportedSystemInfoProbe final : public ISystemInfoProbe
{
  public:
    [[nodiscard]] SystemInfoCapabilities capabilities() const override
    {
        return {.hasOs = false, .unavailableReason = "System information isn't available on this platform"};
    }

    [[nodiscard]] OsInfo readOs() override
    {
        return {};
    }

    [[nodiscard]] FirmwareInfo readFirmware() override
    {
        return {};
    }

    [[nodiscard]] MemoryModulesInfo readMemoryModules() override
    {
        return {};
    }
};

} // namespace Platform
