#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

/// Linux DRM (Direct Rendering Manager) GPU probe for Intel GPUs.
/// Uses sysfs (/sys/class/drm) for GPU enumeration and basic metrics.
/// Supports Intel integrated and discrete GPUs via i915/xe drivers.
/// An optional custom DRM base path can be provided for unit-testing with
/// a synthetic sysfs directory tree instead of the real /sys filesystem.
/// Filesystem errors (a sandbox's unreadable /sys) leave a card or a sensor out; they never throw (#1165).
/// The card list is rebuilt by a full rescanGPUs() when cards appear, disappear or change (#1116).
class DRMGPUProbe : public IGPUProbe
{
  public:
    /// Device-local memory as the DRM query ioctl reports it (#1283).
    struct VramInfo
    {
        uint64_t totalBytes{0};            // Sum of the device-memory (VRAM) regions' sizes; 0 for an iGPU
        std::optional<uint64_t> usedBytes; // nullopt when the kernel withholds it (i915 without CAP_PERFMON)
    };
    /// Queries a card's memory regions given its render node ("/dev/dri/renderD128") and driver
    /// ("xe" or "i915"); nullopt when the query fails. Injectable so tests can script the replies.
    using VramQuery = std::function<std::optional<VramInfo>(const std::string& renderNodePath, const std::string& driver)>;
    /// CLOCK_MONOTONIC in nanoseconds, the time base i915's busy nanoseconds are measured against (#1267).
    using MonotonicClock = std::function<std::uint64_t()>;

    /// One DRM client's fdinfo, as parseFdinfo() reads it (#1267).
    struct DrmFdinfo
    {
        GPUEngineClientCounters client;
        std::string pdev;           // drm-pdev: the PCI address of the device the file is open on; "" if absent
        bool hasEngineStats{false}; // Any engine class had busyness (a kernel without fdinfo stats has none)
    };

    /// @param vramQuery  empty for the real ioctl (queryVramByIoctl)
    /// @param procRoot   where DRM clients' fds and fdinfo are looked for (a fake tree in tests)
    /// @param clock      empty for std::chrono::steady_clock (CLOCK_MONOTONIC)
    explicit DRMGPUProbe(std::string drmBasePath = "/sys/class/drm",
                         VramQuery vramQuery = {},
                         std::string procRoot = "/proc",
                         MonotonicClock clock = {});
    ~DRMGPUProbe() override = default;

    // Rule of 5
    DRMGPUProbe(const DRMGPUProbe&) = delete;
    DRMGPUProbe& operator=(const DRMGPUProbe&) = delete;
    DRMGPUProbe(DRMGPUProbe&&) = delete;
    DRMGPUProbe& operator=(DRMGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;
    [[nodiscard]] bool rescanGPUs(GPURescan depth) override;

    [[nodiscard]] bool isAvailable() const
    {
        return m_Available;
    }

    /// The real VramQuery: opens the render node read-only and issues DRM_IOCTL_XE_DEVICE_QUERY
    /// (DRM_XE_DEVICE_QUERY_MEM_REGIONS) for xe or DRM_IOCTL_I915_QUERY (DRM_I915_QUERY_MEMORY_REGIONS)
    /// for i915. The ioctl takes a runtime-PM reference, so callers only issue it while the card is awake.
    [[nodiscard]] static std::optional<VramInfo> queryVramByIoctl(const std::string& renderNodePath, const std::string& driver);
    /// Sums the VRAM-class regions of an xe struct drm_xe_query_mem_regions reply; nullopt if malformed.
    /// `used` counts only when non-zero (older kernels report 0 without CAP_PERFMON).
    [[nodiscard]] static std::optional<VramInfo> summarizeXeMemRegions(std::span<const std::byte> reply);
    /// Sums the device-class regions of an i915 struct drm_i915_query_memory_regions reply; nullopt if
    /// malformed. Without CAP_PERFMON i915 reports unallocated == probed, so used is then nullopt.
    [[nodiscard]] static std::optional<VramInfo> summarizeI915MemRegions(std::span<const std::byte> reply);
    /// Parses a DRM file's /proc/<pid>/fdinfo/<fd> text (the kernel's drm-usage-stats format): i915's
    /// drm-engine-<class> busy nanoseconds, stamped with `monotonicNs` as their total, or xe's
    /// drm-cycles-<class> with drm-total-cycles-<class>, and drm-engine-capacity-<class> for either.
    /// nullopt when the text has no drm-client-id: the fd isn't, or is no longer, a DRM file, or its
    /// kernel prints no drm-* keys (i915 before Linux 5.19), which only the fd's link tells apart.
    [[nodiscard]] static std::optional<DrmFdinfo> parseFdinfo(std::string_view text, std::uint64_t monotonicNs);

  private:
    // Lets tests build a GPUInfo for a card the Intel-only discovery would skip (an amdgpu card, #1344).
    friend struct DRMGPUProbeTestAccessor;

    /// One DRM file open on a card, and the fds that name it. Dup'd and inherited fds share the file
    /// and so its drm-client-id and engine counters: reading any one of them reads the client (#1356).
    struct DrmClientFds
    {
        // The client's drm-client-id, once one of its fdinfo files has been read; nullopt for a path
        // discoverDrmClients() has found but no sample has read yet, or for a DRM file whose kernel
        // prints no drm-client-id (i915 before 5.19), which is kept with that one path (#1361).
        std::optional<std::uint64_t> clientId;
        // The client's fdinfo paths, never empty: the first is the one read each sample, the rest are
        // aliases, read in turn only once the first has closed or names another file.
        std::vector<std::string> fdinfoPaths;
    };

    struct DRMCard
    {
        std::string cardPath;     // e.g., /sys/class/drm/card0
        std::string devicePath;   // e.g., /sys/class/drm/card0/device
        std::string hwmonPath;    // e.g., /sys/class/drm/card0/device/hwmon/hwmon0
        uint32_t cardIndex{0};    // card0 -> 0, card1 -> 1
        bool isRenderOnly{false}; // renderD* nodes are compute-only
        std::string driver;       // i915, xe, amdgpu, nouveau, etc.
        std::string gpuId;        // Unique ID for tracking
        // VRAM capacity from the last read while awake, reported while the card is runtime-suspended
        // (and so not read), as NVML/ROCm do: capacity doesn't change when a card sleeps (#1117).
        uint64_t lastMemoryTotalBytes{0};
        // Whether lastMemoryTotalBytes came from the DRM query rather than sysfs: a rescan then keeps
        // it only while the query would still go to the same render node through the same driver.
        bool lastMemoryTotalQueried{false};
        // Render node for the DRM query ioctl (/dev/dri/renderD128), from device/drm/renderD*; empty if none.
        std::string renderNodePath;
        // hwmon energy counter (µJ) Domain derives the power draw from (#1269): energy1_input (card),
        // or energy2_input (package) where xe exposes only that (DG2, PVC). Empty if neither exists.
        std::string energyPath;
        // hwmon temperature input (millidegrees C) the card's temperature is read from (#1314): the
        // temp*_input labelled "pkg" (xe: temp2_input; its temp1 has a label but no input), else the
        // lowest-numbered temp*_input (i915: temp1_input, unlabelled). Empty if the hwmon has none.
        std::string temperaturePath;
        // The DRM query ioctl's VRAM figures (#1283): the total is cached, as it doesn't change;
        // the query is repeated each awake sample only while the kernel reports used memory.
        bool vramQueried{false};
        uint64_t queriedVramTotalBytes{0};
        std::optional<uint64_t> queriedVramUsedBytes;
        // The card's DRM clients: the fdinfo files (/proc/<pid>/fdinfo/<fd>) of the fds open on its DRM
        // nodes, found by discoverDrmClients() and grouped by drm-client-id as they are read (#1267).
        // Each awake sample reads one file per client for its engine busyness (#1356). A file that can
        // no longer be read, or is no longer this card's DRM file, is dropped until rediscovered.
        std::vector<DrmClientFds> clients;
    };

    bool initialize();
    [[nodiscard]] std::vector<DRMCard> discoverDRMCards() const;
    /// discoverDRMCards() filtered to Intel (i915/xe) cards, sorted by card path.
    [[nodiscard]] std::vector<DRMCard> discoverIntelCards() const;
    [[nodiscard]] static bool isIntelGPU(const DRMCard& card);
    [[nodiscard]] static std::string readSysfsString(const std::string& path);
    [[nodiscard]] static uint64_t readSysfsUint64(const std::string& path);
    /// The file's unsigned value, or nullopt if it can't be read or parsed (where 0 is a real value).
    [[nodiscard]] static std::optional<uint64_t> readSysfsOptionalUint64(const std::string& path);
    [[nodiscard]] static std::string findHwmonPath(const std::string& devicePath);
    /// "/dev/dri/renderDN" for the card's device/drm/renderDN entry, or "" if it has none.
    [[nodiscard]] static std::string findRenderNodePath(const std::string& devicePath);
    /// The hwmon energy counter file the power draw is derived from, or "" (see DRMCard::energyPath).
    [[nodiscard]] static std::string findEnergyPath(const std::string& hwmonPath);
    /// The hwmon temperature input the card's temperature is read from, or "" (see DRMCard::temperaturePath).
    [[nodiscard]] static std::string findTemperaturePath(const std::string& hwmonPath);
    /// The current-frequency file (MHz) for the card's driver: i915's cardN/gt_cur_freq_mhz, or xe's
    /// device/tile0/gt0/freq0/cur_freq (xe_gt_freq.c; GT0 of the root tile is the primary GT) (#1268).
    [[nodiscard]] static std::string clockPath(const DRMCard& card);
    [[nodiscard]] static std::string getVendorName(const std::string& vendorId);
    [[nodiscard]] static uint32_t parseHexUint32(const std::string& hexStr);
    /// Integrated vs discrete (#1113). An AMD GPU is integrated exactly when `amdApu` (the caller's
    /// AmdApu::isAmdApuDevice() for it), whatever its VRAM or class: an APU reports its carve-out as
    /// VRAM (#1344). Otherwise dedicated memory, or a 3D-controller class, means discrete; then an
    /// Intel GPU's PCI bus decides when known -- every Intel iGPU is a root-complex integrated
    /// endpoint on bus 0 (00:02.0), while Arc/discrete cards sit behind a PCIe port on a non-zero
    /// bus. i915 exposes no dedicated-memory file at all, so before this an Arc on i915 classified
    /// as integrated. Without a bus, the class/VRAM fallback applies as before.
    [[nodiscard]] static bool
    detectIsIntegrated(const std::string& vendorId, uint32_t pciClass, uint64_t vramTotal, std::optional<uint32_t> pciBus, bool amdApu);
    /// The bus number of a sysfs PCI address ("0000:03:00.0" -> 3), or nullopt if `address` isn't one.
    [[nodiscard]] static std::optional<uint32_t> pciBusFromAddress(std::string_view address);
    /// Dedicated (device-local) memory, in bytes, from whichever driver file the card has; 0 if none.
    [[nodiscard]] static uint64_t readVramTotal(const DRMCard& card);
    [[nodiscard]] GPUInfo cardToGPUInfo(const DRMCard& card) const;
    /// Issues the VRAM query for an awake card when its total isn't cached yet, or to refresh used (#1283).
    /// A total that changes (first becomes known) marks the enumeration stale: it classifies the card.
    void refreshQueriedVram(DRMCard& card);
    /// Finds the fds open on each card's DRM nodes (/dev/dri/cardN, renderDN) under m_ProcRoot: a walk
    /// of every visible process's fd links, so only at the first read and each full rescan (#1267).
    /// Link targets only: nothing here touches a GPU, so it never wakes a sleeping card (#1117).
    void discoverDrmClients();
    /// Reads the card's clients' fdinfo into `counter`'s engine fields. Only for an awake card: xe takes
    /// a runtime-PM reference to report its cycles (#1117).
    /// One fdinfo file is read per client: a client's aliases only once the file read for it has
    /// closed or names another file. Clients found to share a drm-client-id are merged (#1356).
    void readEngineClients(DRMCard& card, GPUCounters& counter);
    /// What readClientFdinfo() found at a client's fdinfo path.
    struct ClientFdinfoRead
    {
        enum class Kind : std::uint8_t
        {
            Gone,            // Closed, or no longer a DRM file of the card: dropped
            WithoutClientId, // A DRM file of the card whose kernel prints no drm-* keys (i915 before 5.19)
            Client,          // A DRM client of the card: `info` holds its fdinfo
        };
        Kind kind{Kind::Gone};
        DrmFdinfo info;
    };
    /// Reads the fdinfo at `path`: a client if it is still a DRM file of `card`'s device. An fdinfo
    /// that has no drm-client-id is a DRM file without usage stats when its fd still links to one
    /// of the card's DRM nodes (#1361), and gone otherwise.
    [[nodiscard]] ClientFdinfoRead readClientFdinfo(const std::string& path, const DRMCard& card);
    /// Whether the fd whose fdinfo is at `fdinfoPath` (<proc>/<pid>/fdinfo/<n>) links to one of `card`'s
    /// DRM nodes (/dev/dri/cardN or its render node).
    [[nodiscard]] static bool fdLinksToCard(const std::string& fdinfoPath, const DRMCard& card);

    bool m_Available{false};
    // Set when a card's GPUInfo would differ from the last enumerateGPUs() -- its queried VRAM total
    // became known or changed, and with it the integrated/discrete classification -- and reported
    // (and cleared) by the next rescanGPUs(), so GPUModel re-enumerates and publishes it.
    bool m_GPUInfoStale{false};
    std::vector<DRMCard> m_Cards;
    std::string m_DrmBasePath; // Injectable base path for testing
    VramQuery m_VramQuery;
    std::string m_ProcRoot; // Injectable /proc for testing
    MonotonicClock m_Clock;
    // Whether discoverDrmClients() has run: the first counter read does, rather than the constructor,
    // so a probe that is never sampled never walks /proc (#1267).
    bool m_ClientsDiscovered{false};
    // Whether the last discoverDrmClients() walk could see clients: m_ProcRoot was listed in full and
    // at least one process's fd directory was readable. Without that, "no clients found" says
    // nothing about the card, and its engine busyness is left unread (N/A) rather than idle (#1267).
    bool m_ClientScanReliable{false};
    // How many client fdinfo files readEngineClients() has opened or tried to, for the tests (#1356).
    std::uint64_t m_FdinfoReads{0};
};

} // namespace Platform
