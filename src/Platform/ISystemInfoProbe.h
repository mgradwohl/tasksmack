#pragma once

// The System Information page's static facts (#1399), read once (and again on Refresh) off the UI
// thread. Each section of the page has a raw struct here and a read*() method on the probe; the App
// layer turns them into labelled rows (App/Panels/SystemInfoSections.h).

#include "Platform/IServiceProbe.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
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
    std::string homeDirectory;           ///< The user's profile folder: Windows FOLDERID_Profile, Linux pw_dir
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

/// An ATA drive's SMART health: Linux through udisks2 (#1631), Windows through ATA pass-through (#2588).
struct AtaHealth
{
    bool failing = false;                      ///< The drive's own SMART assessment predicts failure
    std::optional<std::uint64_t> badSectors;   ///< Reallocated plus pending sectors, when reported
    std::optional<std::uint64_t> powerOnHours; ///< When reported
};

/// How a disk is partitioned (#1632).
enum class PartitionStyle : std::uint8_t
{
    Unknown, ///< Not read (Linux without udev's database)
    Mbr,
    Gpt,
    Raw, ///< No partition table
};

/// One partition (#1632). A string the probe couldn't read is empty.
struct Partition
{
    std::uint32_t number = 0; ///< 1-based, as the OS numbers it
    std::string device;       ///< Linux: the kernel name ("nvme0n1p2"); empty on Windows
    std::string typeId;       ///< The GPT type GUID ("C12A7328-F81F-11D2-BA4B-00A0C93EC93B") or MBR type ("0x07")
    std::string typeName;     ///< typeId's common name ("EFI System"); empty for one not in the table
    std::uint64_t offsetBytes = 0;
    std::uint64_t sizeBytes = 0;
    std::string mountPoint; ///< The drive letter ("C:") or mount point ("/boot/efi") on it; empty when none
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
    std::optional<NvmeHealth> health; ///< NVMe drives, when the health log could be read (Linux: through udisks2, #1631)
    std::optional<AtaHealth>
        ataHealth; ///< ATA drives' SMART status: Linux through udisks2 (#1631), Windows through ATA pass-through (#2588)
    std::string healthUnavailableReason;                     ///< Why health is missing; empty when it isn't read for this disk at all
    PartitionStyle partitionStyle = PartitionStyle::Unknown; ///< #1632
    bool partitionsRead = false;                             ///< The layout was read: an empty partitions means none
    std::vector<Partition> partitions;                       ///< In the OS's order (by number)
    std::string partitionsUnavailableReason;                 ///< Why the layout wasn't read
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
    std::string d3dFeatureLevel;      ///< Windows: the highest Direct3D 12 feature level, "12_2" (#1636)
    std::string vulkanApiVersion;     ///< The Vulkan version its driver supports, "1.4.303"; empty without one (#1635)
    std::string vulkanDriverVersion;  ///< That Vulkan driver's version, decoded the vendor's way ("101.6881")
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
    bool vulkanRead = false;   ///< The probe looked for a Vulkan loader (Windows so far, #1635)
    std::string vulkanVersion; ///< The loader's instance version, "1.4.304"; empty when no loader is installed
};

/// The state of one platform security feature (#1514).
enum class SecurityFeatureState : std::uint8_t
{
    Unknown,      ///< It couldn't be read.
    On,           ///< Enabled, or present (a TPM).
    Off,          ///< Supported but disabled.
    NotSupported, ///< The machine can't have it (Secure Boot on a legacy BIOS boot, no TPM).
};

/// A volume's BitLocker protection status (#2584), as Explorer reads and shows it: the shell's
/// System.Volume.BitLockerProtection property, which needs no administrator.
enum class BitLockerState : std::uint8_t
{
    Unknown,              ///< Windows didn't report it for this volume.
    On,                   ///< Encrypted and protected.
    Off,                  ///< Not encrypted.
    Encrypting,           ///< Encryption is under way.
    Decrypting,           ///< Decryption is under way.
    Suspended,            ///< Encrypted, but protection is suspended (the key is stored in the clear).
    Locked,               ///< Encrypted and locked: not unlocked yet.
    NotEncryptable,       ///< BitLocker can't encrypt this volume.
    WaitingForActivation, ///< Encrypted with a clear key, waiting for a protector to be added (device encryption).
};

/// One fixed volume's BitLocker state (#2584).
struct BitLockerVolume
{
    std::string volume; ///< The drive, "C:".
    BitLockerState state = BitLockerState::Unknown;
};

/// One open (unlocked) dm-crypt volume (#2585), the Linux counterpart of a BitLocker volume: a
/// /sys/block/dm-N whose dm/uuid starts CRYPT-LUKS1-, CRYPT-LUKS2- or CRYPT-PLAIN-.
struct EncryptedVolume
{
    std::string name;   ///< The device-mapper name, as under /dev/mapper: "dm_crypt-0", "luks-<uuid>".
    std::string type;   ///< "LUKS2", "LUKS1" or "Plain".
    std::string device; ///< What it encrypts: "nvme0n1p3", or an LV's mapper name; ", "-joined; empty when unknown.
    /// The file systems on it, directly or on an LVM volume on top of it (Ubuntu's encrypted install: / on
    /// an LV on a LUKS physical volume), in mountinfo order. Empty when nothing on it is mounted.
    std::vector<std::string> mountPoints;
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
    bool available = false;              ///< The probe read the section at all.
    OsFamily family = OsFamily::Unknown; ///< Picks the rows: Windows, else the Linux ones.
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

    std::optional<bool> memoryIntegrity;     ///< Windows: HVCI (kernel-mode code integrity enforced by the hypervisor) is on.
    std::optional<bool> kernelDmaProtection; ///< Windows: the DMA guard policy (Kernel DMA Protection) is on.
    bool bitLockerRead = false;              ///< Windows: the fixed volumes were listed.
    std::vector<BitLockerVolume> bitLocker;  ///< Windows: one per fixed volume, in drive-letter order.

    bool encryptedVolumesRead = false;             ///< Linux: /sys/block was listed (#2585).
    std::vector<EncryptedVolume> encryptedVolumes; ///< Linux: the open dm-crypt volumes, in dm-N order.
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
    std::string label; ///< The driver's label ("Package id 0", "Tctl", "fan1"); never empty.
    /// The channel's identity within its device, stable across reads and unique in it even where labels
    /// repeat ("temp1", "in1", "curr1", "thermal_zone3", "\_TZ.TZ00"): what its range and history are
    /// kept by (#2340), never shown. Empty only from a source that has none; the label stands in then.
    std::string id;
    double value = 0.0;             ///< In the kind's unit.
    std::optional<double> high;     ///< The driver's high (warning) threshold, when it reports one.
    std::optional<double> critical; ///< The driver's critical threshold, when it reports one.
};

/// One device's sensors: a hwmon chip ("coretemp", "nvme", "k10temp") or the thermal zones.
struct SensorDevice
{
    std::string name; ///< The driver's device name, made unique on the page ("nvme", "nvme #2").
    /// The device's identity, stable across reads whatever other devices come and go ("hwmon3",
    /// "thermal"): what its sensors' ranges and histories are kept by (#2340), never shown. Empty only from
    /// a source that has none; the name stands in then.
    std::string id;
    std::vector<SensorReading> readings;
};

/// The Sensors facts (#1522): every readable sensor at the time of the read, grouped by device.
struct SensorsInfo
{
    bool available = false;              ///< The probe read the section at all.
    OsFamily family = OsFamily::Unknown; ///< Picks the empty-state text and notes: Windows, else Linux.
    bool listed = false;                 ///< The sensor devices could be listed: an empty `devices` means none.
    std::vector<SensorDevice> devices;   ///< In the OS's device order (Linux: by hwmon index, thermal zones last).
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
    double speedMbps = 0.0;            ///< USB: the link speed; 0 when unknown
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

/// A Windows driver image's signature (#1661), from WinVerifyTrust: embedded in the file, or its hash in a
/// signed catalog. Checked without the network: no revocation check, cached URLs only.
enum class DriverSignature : std::uint8_t
{
    NotChecked, ///< Not read: Linux, no image path, or the check was skipped
    Embedded,   ///< A trusted signature in the file itself
    Catalog,    ///< A trusted catalog holds the file's hash (most inbox drivers)
    Unsigned,   ///< No signature, embedded or in a catalog
    Untrusted,  ///< Signed, but the signature doesn't verify (bad digest, distrusted, expired...)
    Unknown,    ///< The check itself failed; signatureNote says why
};

/// One kernel driver (#1521): a Windows driver service that is running (or, #1661, one that should have
/// started and didn't), or a loaded Linux kernel module.
/// A fact the probe couldn't read, or the other platform's, is empty, 0 or Unknown.
struct KernelDriver
{
    std::string name;                                       ///< The driver service's name ("ACPI") / the module's ("snd_hda_intel")
    std::string displayName;                                ///< Windows: the service's display name ("Microsoft ACPI Driver")
    std::string path;                                       ///< Windows: the image file ("C:\Windows\System32\drivers\ACPI.sys")
    std::string version;                                    ///< Windows: the image's file version; Linux: /sys/module/<name>/version
    std::string company;                                    ///< Windows: the image's CompanyName
    bool fileSystem = false;                                ///< Windows: a file system driver (SERVICE_FILE_SYSTEM_DRIVER)
    ServiceState state = ServiceState::Unknown;             ///< Windows
    ServiceStartType startType = ServiceStartType::Unknown; ///< Windows
    std::string moduleState;                                ///< Linux: "Live", "Loading" or "Unloading"
    std::uint64_t sizeBytes = 0;                            ///< Linux: the module's memory size
    std::optional<std::uint32_t> useCount;                  ///< Linux: the reference count
    std::vector<std::string> usedBy;                        ///< Linux: the modules that depend on it
    bool permanent = false;                                 ///< Linux: it can't be unloaded ("[permanent]")
    std::string taints;                                     ///< Linux: its taint flags ("POE"); empty when none
    std::string startError; ///< Windows: a Boot, System or Automatic start driver stopped with an error: the error's text (#1661)
    DriverSignature signature = DriverSignature::NotChecked; ///< Windows: the image's signature (#1661)
    std::string signatureNote;                               ///< Windows: why it is Untrusted or Unknown
};

/// The Drivers facts (#1521): Windows' running driver services, or Linux's loaded kernel modules.
struct DriversInfo
{
    bool available = false; ///< The probe read the section at all.
    OsFamily family = OsFamily::Unknown;
    bool listed = false; ///< The list was read: an empty drivers means none were found.
    std::vector<KernelDriver> drivers;
};

/// How far back the Recent crashes & hangs section looks (#1524), and the most it lists (the newest).
inline constexpr std::uint32_t CRASH_HISTORY_DAYS = 14;
inline constexpr std::size_t CRASH_LIST_MAX = 200;

/// One application crash or hang (#1524): a Windows Application Error (1000) or Application Hang (1002)
/// event, or a Linux systemd-coredump core file. A fact the source doesn't carry is empty or nullopt.
struct CrashEvent
{
    std::uint64_t unixSeconds = 0; ///< When it happened; 0 when unknown
    bool hang = false;             ///< Windows: an Application Hang (1002) rather than a crash
    std::string application;       ///< "notepad.exe" / the core's comm ("python3")
    std::string appVersion;        ///< Windows
    std::string module;            ///< Windows 1000: the faulting module ("ntdll.dll")
    std::string moduleVersion;     ///< Windows 1000
    std::string exceptionCode;     ///< Windows 1000: "0xC0000005"
    std::string hangType;          ///< Windows 1002: "Quiesce", "Cross-thread", ...
    std::optional<std::uint32_t> pid;
    std::optional<std::uint32_t> uid; ///< Linux: the crashed process's user id
    std::uint64_t coreBytes = 0;      ///< Linux: the core file's size on disk, when it could be read
    bool coreKept = false;            ///< Linux: systemd-coredump kept a core file
    std::string signal;               ///< Linux: the signal that killed it ("SIGSEGV"), from the journal (#1674)
    std::string executable;           ///< Linux: its executable's path, from the journal (#1674)
};

/// The Recent crashes & hangs facts (#1524), newest first, at most CRASH_LIST_MAX of the last
/// CRASH_HISTORY_DAYS days.
struct CrashesInfo
{
    bool available = false; ///< The probe read the section at all.
    OsFamily family = OsFamily::Unknown;
    bool listed = false;                  ///< The source was read: an empty events means none were found.
    bool accessDenied = false;            ///< Not listed because reading it needs more permission.
    std::string unavailableReason;        ///< Why not listed, for the UI.
    bool capped = false;                  ///< More than CRASH_LIST_MAX matched; the newest are kept.
    bool journalRead = false;             ///< Linux: systemd-coredump's journal entries were read too (#1674)
    std::string journalUnavailableReason; ///< Linux: why not, when a journal read was tried and failed
    /// Linux: other users' journal entries are hidden from this user (not root, nor in the systemd-journal,
    /// adm or wheel group), so only its own crashes have their details (#2310)
    bool otherUsersHidden = false;
    std::vector<CrashEvent> events;
};

/// One IP address on an adapter (#1518).
struct AdapterAddress
{
    std::string address;      ///< "192.168.1.20", "fe80::1c2a:3bff:fe4d:5e6f"
    std::uint32_t prefix = 0; ///< The prefix length ("/24", "/64")
    bool v6 = false;

    [[nodiscard]] friend bool operator==(const AdapterAddress&, const AdapterAddress&) = default;
};

/// A connected Wi-Fi adapter's link (#1518): the network it has joined and how fast.
struct WifiLink
{
    std::string ssid;               ///< The network's name, made safe to draw; an identifier (it can place the machine)
    std::uint32_t frequencyMhz = 0; ///< The channel's centre frequency; 0 when unknown
    std::uint32_t bitrateKbps = 0;  ///< The current transmit rate in kbit/s; 0 when unknown

    /// 0-100 %: Windows' own measure of the signal; Linux: NetworkManager's strength of the access point (#2591).
    std::optional<std::uint32_t> signalQuality;
    std::string phy;                 ///< The standard: "802.11ax (Wi-Fi 6)"; Linux: from nl80211's rates (#2591)
    std::uint32_t channel = 0;       ///< Windows: the channel when frequencyMhz isn't known; 0 when unknown
    std::uint32_t rxBitrateKbps = 0; ///< The current receive rate in kbit/s; 0 when unknown; Linux: nl80211
};

/// An adapter's IPv4 DHCP lease (#1518): who gave the address, for how long, and until when.
struct DhcpLease
{
    std::string server;             ///< The DHCP server's address; empty when unknown
    std::uint32_t leaseSeconds = 0; ///< The lease's length; 0 when unknown
    std::uint64_t expiresUnix = 0;  ///< When it runs out, in Unix seconds; 0 when unknown
};

/// One network adapter's configuration (#1518). A fact the probe couldn't read is empty or 0.
struct NetworkAdapter
{
    std::string name; ///< "eth0", "wlp3s0"; Windows: the connection's name, "Ethernet", "Wi-Fi"
    std::string mac;  ///< "a4:5e:60:12:34:56"; an identifier
    std::uint32_t mtu = 0;
    std::string driver; ///< "e1000e", "iwlwifi"
    bool up = false;    ///< The OS reports the link up.
    bool wireless = false;
    std::vector<AdapterAddress> addresses;
    std::optional<int> wifiSignalDbm; ///< Wi-Fi: the signal level, when the driver reports one.
    std::optional<WifiLink> wifiLink; ///< Wi-Fi: the link, when it is connected and could be read.
    std::string wifiLinkUnavailable;  ///< Wi-Fi: why wifiLink is empty, for the UI; empty when unknown.
    /// Wi-Fi, Windows: connected, but the details that need location access (the network name, signal
    /// quality, standard, band and rates) were left out of this read because they weren't asked for
    /// (#1782). The RSSI and channel number don't need location access and are still read.
    bool wifiDetailsWithheld = false;
    /// The DNS servers set for this adapter alone; empty when there are none or they couldn't be read.
    /// Linux: systemd-resolved's per-link servers (which NetworkManager and systemd-networkd hand it).
    /// Windows: the adapter's DNS servers from GetAdaptersAddresses.
    std::vector<std::string> linkDnsServers;
    /// This adapter's own search domains; they can name the organisation. Linux: a routing-only one is
    /// written "~corp.example", as resolvectl writes it. Windows: the connection-specific DNS suffix,
    /// then the adapter's DNS suffix search list.
    std::vector<std::string> linkSearchDomains;
    /// The IPv4 DHCP lease, when the address came from one. Linux: the one NetworkManager holds for this
    /// adapter (none when NetworkManager doesn't manage it). Windows: the server and the expiry from
    /// GetAdaptersAddresses (the lease's length isn't reported, so leaseSeconds stays 0).
    std::optional<DhcpLease> dhcpLease;

    /// The adapter's hardware, "Intel(R) Wi-Fi 6E AX211 160MHz"; Linux: udev's hwdata vendor and model (#2591).
    std::string description;
    /// This adapter's default gateways; Linux: the default routes that leave by it, IPv4 then IPv6 (#2591).
    std::vector<std::string> gateways;
    /// Whether IPv4 is configured by DHCP; unset when not known. Says "static" for an adapter without
    /// a dhcpLease, and "no lease yet" for one that asks DHCP but holds none. Linux: NetworkManager's
    /// DHCP4 config for a device it manages, or a systemd-networkd lease (#2591).
    std::optional<bool> dhcpEnabled;
};

/// The Network adapters facts (#1518): each adapter's addresses and settings, the default gateways
/// and the DNS servers.
struct NetworkAdaptersInfo
{
    bool available = false; ///< The probe read the section at all.
    OsFamily family = OsFamily::Unknown;
    bool listed = false;                  ///< The adapters could be listed: an empty `adapters` means none.
    std::vector<NetworkAdapter> adapters; ///< By name; the loopback adapter is left out.
    std::string gatewayV4;                ///< The IPv4 default gateway; empty when there is none or it is unknown.
    std::string gatewayV4Adapter;
    std::string gatewayV6; ///< The IPv6 default gateway.
    std::string gatewayV6Adapter;
    bool dnsRead = false; ///< The resolver configuration was read.
    std::vector<std::string> dnsServers;
    std::vector<std::string> searchDomains;
    bool dnsIsLocalStub = false; ///< Linux: only systemd-resolved's local stub (127.0.0.53) was found.
};

/// This machine's public addresses (#1518): what the internet sees it as, asked of a DNS resolver
/// (OpenDNS's myip.opendns.com) only when the user asks, since it sends a query off the machine. Each
/// address is an identifier (it places the machine).
struct PublicIpInfo
{
    bool read = false;         ///< The lookup ran (it was asked for and the platform has one).
    std::string v4;            ///< "203.0.113.7"; empty when there is none or it couldn't be found
    std::string v6;            ///< "2001:db8::7"; empty when there is none or it couldn't be found
    std::string v4Unavailable; ///< Why v4 is empty, for the UI
    std::string v6Unavailable; ///< Why v6 is empty, for the UI
    std::string source;        ///< Who answered: "OpenDNS (myip.opendns.com)"
};

/// How long the last boot took and where the time went (#1525), in microseconds. A phase the
/// platform doesn't measure (firmware and loader without UEFI's boot-time variables, initrd without
/// an initramfs) is nullopt. Windows measures other phases: the total, main path and post-boot times of
/// the boot's Diagnostics-Performance event, with the systemd phases left nullopt.
struct BootPerformanceInfo
{
    bool available = false;        ///< The probe read the section at all.
    bool timingsRead = false;      ///< The timings could be read; else unavailableReason says why.
    std::string unavailableReason; ///< Why they couldn't be, for the UI.
    bool finished = false;         ///< Startup has finished (systemd: FinishTimestamp is set).
    std::optional<std::uint64_t> firmwareUs;
    std::optional<std::uint64_t> loaderUs;
    std::optional<std::uint64_t> kernelUs;
    std::optional<std::uint64_t> initrdUs;
    std::optional<std::uint64_t> userspaceUs;
    std::optional<std::uint64_t> totalUs; ///< The sum of the phases measured; nullopt until startup finishes.

    OsFamily family = OsFamily::Unknown;     ///< Picks the phase rows: Windows, else the Linux (systemd) ones.
    std::optional<std::uint64_t> mainPathUs; ///< Windows: from the OS loader until the desktop shows (MainPathBootTime).
    std::optional<std::uint64_t> postBootUs; ///< Windows: from the desktop until the system settles (BootPostBootTime).
    std::uint64_t recordedUnixSeconds = 0;   ///< Windows: when the boot's event was logged; 0 when unknown.
};

/// What the platform can read at all.
struct SystemInfoCapabilities
{
    bool hasOs = false;            ///< readOs() and readFirmware() return facts.
    std::string unavailableReason; ///< Why hasOs is false, for the UI. Empty when it is true.
    /// Reading the Wi-Fi details can ask the user for location access (Windows 11 24H2 and later), so
    /// readNetworkAdapters() reads them only when asked (#1782). False where they are always read (Linux).
    bool wifiDetailsGated = false;
    /// readPublicIp() looks the public addresses up (#1518). False where it isn't implemented.
    bool hasPublicIp = false;
    /// authorize() can ask for an administrator's password to read the root-only facts (#2310)
    bool canReadAuthorized = false;
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

    /// The Drivers facts (#1521); read when hasOs is true. Enumeration only: no driver is started, stopped or changed.
    [[nodiscard]] virtual DriversInfo readDrivers() = 0;

    /// The Recent crashes & hangs facts (#1524); read when hasOs is true. Reads logs only: nothing is cleared or written.
    [[nodiscard]] virtual CrashesInfo readCrashes() = 0;

    /// The Network adapters facts (#1518); read when hasOs is true. Where capabilities().wifiDetailsGated,
    /// the Wi-Fi details that can raise a location prompt are read only with @p includeWifiDetails (#1782);
    /// elsewhere it is ignored and they are always read.
    [[nodiscard]] virtual NetworkAdaptersInfo readNetworkAdapters(bool includeWifiDetails) = 0;

    /// The public addresses (#1518), asked of a DNS resolver off the machine: called only when the user
    /// asked, and where capabilities().hasPublicIp. Takes up to a few seconds. Platforms without a lookup
    /// keep this default, which reports nothing.
    [[nodiscard]] virtual PublicIpInfo readPublicIp()
    {
        return {};
    }

    /// The Boot performance facts (#1525); read when hasOs is true.
    [[nodiscard]] virtual BootPerformanceInfo readBootPerformance() = 0;

    /// Whether authorize() can ask for an administrator's password, so that the facts readable by root
    /// only (FirmwareInfo::identifiersNeedAdmin and smbiosVersionNeedsAdmin, MemoryModulesInfo's
    /// tableNeedsAdmin) can be read through the privileged helper (#2310). False by default.
    [[nodiscard]] virtual bool canReadAuthorized() const
    {
        return false;
    }

    /// Asks for authorization and starts the helper; true once it runs. While it runs, readFirmware()
    /// and readMemoryModules() fill the root-only facts through it. Blocks until the prompt is answered
    /// or @p stopToken is stopped, so call it off the UI thread. Platforms without a helper keep this
    /// default, which does nothing.
    virtual bool authorize(const std::stop_token& stopToken)
    {
        static_cast<void>(stopToken);
        return false;
    }
};

} // namespace Platform
