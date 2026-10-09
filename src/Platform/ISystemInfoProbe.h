#pragma once

// The System Information page's static facts (#1399), read once (and again on Refresh) off the UI
// thread. Each section of the page has a raw struct here and a read*() method on the probe; the App
// layer turns them into labelled rows (App/Panels/SystemInfoSections.h).

#include <cstddef>
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

/// One page file (Windows) or swap device (Linux, /proc/swaps), #1516. A size the OS doesn't give is 0.
struct PageFile
{
    std::string path;            ///< A DOS path ("C:\pagefile.sys") / the swap file or partition
    std::string kind;            ///< Linux: /proc/swaps' Type ("partition", "file"); empty on Windows
    std::uint64_t sizeBytes = 0; ///< Its current size
    std::uint64_t usedBytes = 0;
    std::uint64_t peakBytes = 0; ///< Windows: the most it has held since boot; 0 on Linux
    int priority = 0;            ///< Linux: the swap priority
};

/// One zram device's /sys/block/zramN/mm_stat totals (#1516).
struct ZramDevice
{
    std::string name;                  ///< "zram0"
    std::uint64_t originalBytes = 0;   ///< Data stored, uncompressed
    std::uint64_t compressedBytes = 0; ///< That data compressed
    std::uint64_t memoryUsedBytes = 0; ///< RAM the device uses, overhead included
};

/// Linux's vm.overcommit_memory (0, 1, 2).
enum class OvercommitMode : std::uint8_t
{
    Unknown,
    Heuristic, ///< 0
    Always,    ///< 1
    Strict,    ///< 2: the commit limit is enforced
};

/// The Commit & paging facts (#1516). A size the probe couldn't read is 0 (a commit charge or limit is
/// never 0 for real); a fact that can legitimately be 0 or off is an optional.
struct CommitPagingInfo
{
    bool available = false; ///< The probe read the section at all.
    OsFamily family = OsFamily::Unknown;
    std::uint64_t committedBytes = 0;   ///< Windows CommitTotal / Linux Committed_AS
    std::uint64_t commitLimitBytes = 0; ///< Windows CommitLimit / Linux CommitLimit
    std::uint64_t commitPeakBytes = 0;  ///< Windows only
    std::uint64_t pageSizeBytes = 0;    ///< Windows only
    bool pageFilesRead = false;         ///< The list was read: an empty pageFiles means none configured.
    std::vector<PageFile> pageFiles;
    std::optional<std::uint64_t> compressedBytes; ///< Windows: the Memory Compression process's working set

    OvercommitMode overcommit = OvercommitMode::Unknown; ///< Linux
    bool zramRead = false;                               ///< Linux: /sys/block was listed
    std::vector<ZramDevice> zram;                        ///< Linux
    std::optional<bool> zswapEnabled;                    ///< Linux
    bool hugePagesRead = false;                          ///< Linux: HugePages_Total was in /proc/meminfo
    std::uint64_t hugePagesTotal = 0;
    std::uint64_t hugePagesFree = 0;
    std::uint64_t hugePagesReserved = 0;
    std::uint64_t hugePagesSurplus = 0;
    std::uint64_t hugePageSizeBytes = 0;
    std::string transparentHugePages; ///< Linux: the bracketed mode ("always", "madvise", "never")
};

/// Whether a disk spins (#1517): Windows' seek-penalty property / Linux's queue/rotational.
enum class DiskMedia : std::uint8_t
{
    Unknown,
    Ssd,
    Hdd,
};

/// An NVMe drive's health log page (#1517), the fields the page shows.
struct NvmeHealth
{
    std::uint8_t criticalWarning = 0;       ///< The critical warning bits; 0 is healthy
    std::uint8_t availableSparePercent = 0; ///< Spare capacity left
    std::uint8_t percentageUsed = 0;        ///< Life used, by the vendor's estimate; can pass 100
    std::uint64_t mediaErrors = 0;          ///< Unrecovered data integrity errors (the low 64 bits)
};

/// One physical disk (#1517). A string the probe couldn't read is empty and a size 0.
struct PhysicalDisk
{
    std::string name;  ///< "Disk 0" (\\.\PhysicalDrive0) / "nvme0n1"
    std::string model; ///< "Samsung SSD 980 PRO 1TB"
    std::string bus;   ///< "NVMe", "SATA", "USB", ...
    DiskMedia media = DiskMedia::Unknown;
    std::uint64_t sizeBytes = 0;
    std::string firmware; ///< The firmware revision
    std::string serial;   ///< An identifier
    std::optional<int> temperatureCelsius;
    std::optional<NvmeHealth> health;    ///< Windows: NVMe drives, when the health log could be read
    std::string healthUnavailableReason; ///< Why health is missing; empty when it isn't read for this disk at all
};

/// One mounted volume (#1517): a drive letter or a mount point.
struct Volume
{
    std::string mountPoint; ///< "C:" / "/home"
    std::string label;
    std::string fileSystem; ///< "NTFS" / "ext4"
    std::string device;     ///< Linux: the mount source ("/dev/nvme0n1p2"); empty on Windows
    bool network = false;   ///< A network drive or file system: never queried, as the calls can hang
    bool sizeRead = false;  ///< sizeBytes and freeBytes were read
    std::uint64_t sizeBytes = 0;
    std::uint64_t freeBytes = 0;
};

/// The Storage facts (#1517): physical disks and mounted volumes.
struct StorageInfo
{
    bool available = false; ///< The probe read the section at all.
    OsFamily family = OsFamily::Unknown;
    bool disksRead = false; ///< The disks were enumerated: an empty disks means none were found.
    std::vector<PhysicalDisk> disks;
    bool volumesRead = false; ///< The volumes were enumerated.
    std::vector<Volume> volumes;
};

/// One graphics adapter (#1519): a GPU, or a display-only adapter. A fact the probe couldn't read is
/// empty or 0.
struct GraphicsAdapter
{
    std::string name; ///< DXGI's description / the driver's product name, else the vendor and PCI ids
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedBytes = 0; ///< Video memory of its own
    std::uint64_t sharedBytes = 0;    ///< Windows: system memory it may borrow
    std::string location;             ///< The PCI location, "01:00.0" / "0000:01:00.0"
    std::string driver;               ///< Linux: the kernel driver ("amdgpu", "nvidia")
    std::string driverVersion;        ///< Windows: the driver package's / Linux: the module's, if it has one
    std::string driverDate;           ///< Windows: "2024-09-05"
};

/// One connected monitor (#1519), from its EDID and, on Windows, the DXGI output it is attached to.
struct Monitor
{
    std::string name;          ///< The EDID monitor name, else its manufacturer and product code ("DEL 41A8")
    std::string serial;        ///< The EDID serial: an identifier
    std::uint32_t widthMm = 0; ///< The physical image size; 0 when the EDID doesn't give it
    std::uint32_t heightMm = 0;
    std::string connector;       ///< Linux: the DRM connector ("DP-1", "eDP-1")
    bool hasDesktopRect = false; ///< Windows: where it sits on the desktop, in pixels, to match it to a display
    int desktopX = 0;
    int desktopY = 0;
    int desktopWidth = 0;
    int desktopHeight = 0;
    std::string colorSpace; ///< Windows: "sRGB", "BT.2020 PQ", ...; empty when unknown
    bool hdr = false;       ///< Windows: the output runs in an HDR colour space
    std::uint32_t bitsPerColor = 0;
};

/// The Graphics & displays facts the platform reads (#1519). TaskSmack's own OpenGL context and the
/// displays' modes and scale come from Core instead (Core/GraphicsHostInfo.h).
struct GraphicsInfo
{
    bool available = false; ///< The probe read the section at all.
    OsFamily family = OsFamily::Unknown;
    bool adaptersRead = false; ///< The adapters were enumerated: an empty adapters means none were found.
    std::vector<GraphicsAdapter> adapters;
    bool monitorsRead = false; ///< The monitors were enumerated.
    std::vector<Monitor> monitors;
    std::string displayServer; ///< Linux: "Wayland", "X11", ...; empty without a graphical session
};

/// The state of one platform security feature (#1514).
enum class SecurityFeatureState : std::uint8_t
{
    Unknown,      ///< It couldn't be read.
    On,           ///< Enabled, or present (a TPM).
    Off,          ///< Supported but disabled.
    NotSupported, ///< The machine can't have it (Secure Boot on a legacy BIOS boot, no TPM).
};

/// One CPU vulnerability as the kernel reports it: the file name under
/// /sys/devices/system/cpu/vulnerabilities and its one-line status.
struct CpuVulnerability
{
    std::string name;   ///< "spectre_v2", "meltdown"
    std::string status; ///< "Not affected", "Vulnerable", "Mitigation: Retpolines; ..."
};

/// The Security facts (#1514): read-only status of the platform's security features. A fact that
/// couldn't be read is Unknown, empty or nullopt rather than a guess.
struct PlatformSecurityInfo
{
    bool available = false; ///< The probe read the section at all.
    SecurityFeatureState secureBoot = SecurityFeatureState::Unknown;
    SecurityFeatureState tpm = SecurityFeatureState::Unknown;
    std::uint32_t tpmVersionMajor = 0; ///< 1 or 2 when known; 0 otherwise.

    bool lsmRead = false;                 ///< Linux: /sys/kernel/security/lsm was read.
    std::vector<std::string> lsms;        ///< Linux: the active security modules, in load order.
    std::optional<bool> selinuxEnforcing; ///< Linux: nullopt when SELinux isn't active.
    std::optional<bool> apparmorEnabled;  ///< Linux: nullopt when the AppArmor module isn't loaded.
    std::string lockdown;                 ///< Linux: "none", "integrity" or "confidentiality"; empty when unknown.

    bool vulnerabilitiesRead = false;              ///< The kernel's list was read.
    std::vector<CpuVulnerability> vulnerabilities; ///< Sorted by name.
};

/// What a sensor measures (#1522).
enum class SensorKind : std::uint8_t
{
    Temperature, ///< Degrees Celsius
    Fan,         ///< RPM
    Voltage,     ///< Volts
    Current,     ///< Amperes
    Power,       ///< Watts
};

/// One sensor reading, in the kind's unit, with the driver's label.
struct SensorReading
{
    SensorKind kind = SensorKind::Temperature;
    std::string label;              ///< The driver's label ("Package id 0", "Tctl", "fan1"); never empty.
    double value = 0.0;             ///< In the kind's unit.
    std::optional<double> high;     ///< The driver's high (warning) threshold, when it reports one.
    std::optional<double> critical; ///< The driver's critical threshold, when it reports one.
};

/// One device's sensors: a hwmon chip ("coretemp", "nvme", "k10temp") or the thermal zones.
struct SensorDevice
{
    std::string name; ///< The driver's device name, made unique on the page ("nvme", "nvme #2").
    std::vector<SensorReading> readings;
};

/// The Sensors facts (#1522): every readable sensor at the time of the read, grouped by device.
struct SensorsInfo
{
    bool available = false;            ///< The probe read the section at all.
    bool listed = false;               ///< The sensor devices could be listed: an empty `devices` means none.
    std::vector<SensorDevice> devices; ///< In the OS's device order (Linux: by hwmon index, thermal zones last).
};

/// One device (#1520): a PCI function or a USB device. A fact the probe couldn't read is empty or 0.
struct Device
{
    std::string name;                  ///< Windows' friendly name / pci.ids, the USB product string, usb.ids
    std::string vendor;                ///< Empty when unknown; Windows: only for a device without a name
    std::uint16_t vendorId = 0;        ///< The PCI vendor / USB idVendor; 0 when unknown
    std::uint16_t productId = 0;       ///< The PCI device / USB idProduct
    std::string className;             ///< PCI: the setup class ("Display adapters") / pci.ids ("VGA compatible controller")
    std::string location;              ///< PCI: "01:00.0" / "0000:01:00.0"; Linux USB: the port path ("1-1.2")
    std::string driver;                ///< The bound driver: Windows' service / Linux's module; empty without one
    double speedMbps = 0.0;            ///< Linux USB: the link speed; 0 when unknown
    std::uint32_t depth = 0;           ///< USB: 1 for a device on a root hub, 2 behind one more hub, ...
    std::optional<std::size_t> parent; ///< USB: the hub it is plugged into, as an index into DevicesInfo::usb
    std::string serial;                ///< USB: the device's serial number, an identifier
    std::string problem;               ///< Why it isn't working ("No driver"); empty when it is fine
};

/// Which way an audio endpoint carries sound.
enum class AudioFlow : std::uint8_t
{
    Unknown,
    Output, ///< Speakers, headphones, HDMI (render / playback)
    Input,  ///< Microphones, line in (capture)
};

/// One active audio endpoint (Windows) or ALSA PCM device (Linux).
struct AudioEndpoint
{
    std::string name; ///< "Speakers (Realtek(R) Audio)" / "HDA Intel PCH: ALC892 Analog"
    AudioFlow flow = AudioFlow::Unknown;
};

/// The Devices facts (#1520): PCI and USB devices, audio endpoints and the devices in an error state.
struct DevicesInfo
{
    bool available = false; ///< The probe read the section at all.
    OsFamily family = OsFamily::Unknown;
    bool pciRead = false; ///< The PCI devices were enumerated: an empty pci means none were found.
    std::vector<Device> pci;
    bool usbRead = false;
    std::vector<Device> usb; ///< Parents before their children; root hubs and host controllers left out
    bool audioRead = false;
    std::vector<AudioEndpoint> audio;
    bool problemsRead = false;    ///< Windows: every present device's status was read; Linux: as pciRead
    std::vector<Device> problems; ///< Windows: every present device with a problem; Linux: PCI devices without a driver
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

    /// The Commit & paging facts (#1516); read when hasOs is true.
    [[nodiscard]] virtual CommitPagingInfo readCommitPaging() = 0;

    /// The Storage facts (#1517); read when hasOs is true. A slow disk can make this take a while, which
    /// is fine: reads run off the UI thread.
    [[nodiscard]] virtual StorageInfo readStorage() = 0;

    /// The Graphics & displays facts (#1519); read when hasOs is true.
    [[nodiscard]] virtual GraphicsInfo readGraphics() = 0;

    /// The Security facts (#1514); read when hasOs is true.
    [[nodiscard]] virtual PlatformSecurityInfo readPlatformSecurity() = 0;

    /// The Sensors facts (#1522); read when hasOs is true.
    [[nodiscard]] virtual SensorsInfo readSensors() = 0;

    /// The Devices facts (#1520); read when hasOs is true. Enumeration only: no device state changes.
    [[nodiscard]] virtual DevicesInfo readDevices() = 0;
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

    [[nodiscard]] CommitPagingInfo readCommitPaging() override
    {
        return {};
    }

    [[nodiscard]] StorageInfo readStorage() override
    {
        return {};
    }

    [[nodiscard]] GraphicsInfo readGraphics() override
    {
        return {};
    }

    [[nodiscard]] PlatformSecurityInfo readPlatformSecurity() override
    {
        return {};
    }

    [[nodiscard]] SensorsInfo readSensors() override
    {
        return {};
    }

    [[nodiscard]] DevicesInfo readDevices() override
    {
        return {};
    }
};

} // namespace Platform
