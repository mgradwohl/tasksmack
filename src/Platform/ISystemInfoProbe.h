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
};

} // namespace Platform
