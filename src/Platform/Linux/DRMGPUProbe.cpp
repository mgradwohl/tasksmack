#include "DRMGPUProbe.h"

#include "AmdApu.h"
#include "PciRuntimePm.h"
#include "Platform/GPUTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Kernel UAPI for the DRM memory-region queries (#1283). xe_drm.h ships with the kernel headers
// since Linux 6.8 (Ubuntu 24.04's linux-libc-dev); without it the VRAM query is simply unavailable.
#if __has_include(<drm/xe_drm.h>) && __has_include(<drm/i915_drm.h>)
#include <drm/i915_drm.h>
#include <drm/xe_drm.h>
#define TASKSMACK_HAS_DRM_QUERY_UAPI 1 // NOLINT(cppcoreguidelines-macro-usage) -- tested by #if
#else
#define TASKSMACK_HAS_DRM_QUERY_UAPI 0 // NOLINT(cppcoreguidelines-macro-usage) -- tested by #if
#endif

namespace Platform
{

namespace Fs = std::filesystem;

namespace
{
// PCI class codes: full 24-bit value (class | subclass | prog-if), mask off prog-if with 0xFFFF00
// to get the class+subclass pair for comparison.
constexpr uint32_t PCI_CLASS_SUBCLASS_MASK = 0xFFFF00U;

// Display/3D controller PCI class+subclass values (prog-if bits cleared)
constexpr uint32_t PCI_CLASS_VGA_COMPATIBLE = 0x030000U;     // VGA compatible controller
constexpr uint32_t PCI_CLASS_3D_CONTROLLER = 0x030200U;      // 3D controller (compute-only, no display)
constexpr uint32_t PCI_CLASS_DISPLAY_CONTROLLER = 0x038000U; // Display controller (non-VGA)

// PCI vendor IDs
constexpr uint32_t PCI_VENDOR_INTEL = 0x8086U;
constexpr uint32_t PCI_VENDOR_NVIDIA = 0x10DEU;
constexpr uint32_t PCI_VENDOR_AMD = 0x1002U;

/// A directory's entries, in iteration order; empty (not a throw) if it can't be listed (#1165).
[[nodiscard]] std::vector<Fs::path> listDirectory(const Fs::path& dir)
{
    std::vector<Fs::path> entries;
    std::error_code fsErr;
    Fs::directory_iterator it(dir, fsErr);
    for (const Fs::directory_iterator end; !fsErr && it != end; it.increment(fsErr))
    {
        entries.push_back(it->path());
    }
    if (fsErr)
    {
        spdlog::debug("DRMGPUProbe: failed to list {}: {}", dir.string(), fsErr.message());
    }
    return entries;
}

[[nodiscard]] bool pathExists(const Fs::path& path)
{
    std::error_code fsErr;
    return Fs::exists(path, fsErr) && !fsErr;
}

/// The filename a symlink points to ("i915" for .../drivers/i915), or nullopt if `link` isn't a
/// readable symlink.
[[nodiscard]] std::optional<std::string> symlinkTargetName(const Fs::path& link)
{
    std::error_code fsErr;
    if (!Fs::is_symlink(link, fsErr) || fsErr)
    {
        return std::nullopt;
    }
    const auto target = Fs::read_symlink(link, fsErr);
    if (fsErr)
    {
        return std::nullopt;
    }
    return target.filename().string();
}

/// `text` without leading and trailing spaces, tabs and CRs.
[[nodiscard]] std::string_view trimmed(std::string_view text)
{
    constexpr std::string_view SPACE = " \t\r";
    const auto first = text.find_first_not_of(SPACE);
    if (first == std::string_view::npos)
    {
        return {};
    }
    return text.substr(first, text.find_last_not_of(SPACE) - first + 1);
}

/// The leading unsigned number of an fdinfo value (" 1234 ns" -> 1234), or nullopt if it has none or
/// the number runs straight into other text.
[[nodiscard]] std::optional<std::uint64_t> leadingUint64(std::string_view value)
{
    value = trimmed(value);
    std::uint64_t number = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (ec != std::errc{} || ptr == value.data() || (ptr != value.data() + value.size() && *ptr != ' ' && *ptr != '\t'))
    {
        return std::nullopt;
    }
    return number;
}

/// The index (GPUEngineClass) of the engine class an fdinfo key names after its prefix: i915's
/// render/copy/video/video-enhance/compute or xe's rcs/bcs/vcs/vecs/ccs (#1267).
[[nodiscard]] std::optional<std::size_t> engineClassIndex(std::string_view name)
{
    struct EngineName
    {
        std::string_view i915;
        std::string_view xe;
        GPUEngineClass engineClass;
    };
    static constexpr std::array<EngineName, GPU_ENGINE_CLASS_COUNT> NAMES{{
        {.i915 = "render", .xe = "rcs", .engineClass = GPUEngineClass::Render},
        {.i915 = "copy", .xe = "bcs", .engineClass = GPUEngineClass::Copy},
        {.i915 = "video", .xe = "vcs", .engineClass = GPUEngineClass::Video},
        {.i915 = "video-enhance", .xe = "vecs", .engineClass = GPUEngineClass::VideoEnhance},
        {.i915 = "compute", .xe = "ccs", .engineClass = GPUEngineClass::Compute},
    }};
    const auto* const match =
        std::ranges::find_if(NAMES, [name](const EngineName& entry) { return name == entry.i915 || name == entry.xe; });
    if (match == NAMES.end())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(match->engineClass);
}

#if TASKSMACK_HAS_DRM_QUERY_UAPI
/// An ioctl retried on EINTR/EAGAIN, as libdrm's drmIoctl() does.
[[nodiscard]] int drmIoctl(int fd, unsigned long request, void* arg)
{
    while (true)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- ioctl(2) is variadic
        const int ret = ::ioctl(fd, request, arg);
        if (ret != -1 || (errno != EINTR && errno != EAGAIN))
        {
            return ret;
        }
    }
}

/// Closes a file descriptor on scope exit.
class FdGuard
{
  public:
    explicit FdGuard(int fd) : m_Fd(fd)
    {}
    ~FdGuard()
    {
        if (m_Fd >= 0)
        {
            ::close(m_Fd);
        }
    }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&&) = delete;
    FdGuard& operator=(FdGuard&&) = delete;

    [[nodiscard]] int get() const
    {
        return m_Fd;
    }

  private:
    int m_Fd;
};

/// A reply buffer for a DRM query, 8-byte aligned for the kernel's __u64 fields.
[[nodiscard]] std::vector<uint64_t> makeReplyBuffer(std::size_t bytes)
{
    std::vector<uint64_t> buffer((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0); // Not braces: a size, not elements
    return buffer;
}

[[nodiscard]] std::span<const std::byte> replyBytes(const std::vector<uint64_t>& buffer, std::size_t bytes)
{
    return std::as_bytes(std::span(buffer)).first(bytes);
}

/// Reads a trivially-copyable T at `offset` of `bytes`; the caller checked the bounds.
template<typename T> [[nodiscard]] T readAt(std::span<const std::byte> bytes, std::size_t offset)
{
    T value{};
    std::memcpy(&value, bytes.subspan(offset, sizeof(T)).data(), sizeof(T));
    return value;
}
#endif
} // namespace

DRMGPUProbe::DRMGPUProbe(std::string drmBasePath, VramQuery vramQuery, std::string procRoot, MonotonicClock clock)
    : m_DrmBasePath(std::move(drmBasePath)), m_VramQuery(std::move(vramQuery)), m_ProcRoot(std::move(procRoot)), m_Clock(std::move(clock))
{
    if (!m_VramQuery)
    {
        m_VramQuery = &DRMGPUProbe::queryVramByIoctl;
    }
    if (!m_Clock)
    {
        m_Clock = []
        {
            const auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count());
        };
    }

    // Must call initialize() in the body, not the initializer list,
    // because initialize() uses m_Cards which must be constructed first
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    m_Available = initialize();

    if (m_Available)
    {
        spdlog::debug("DRMGPUProbe: Initialized successfully, found {} DRM card(s)", m_Cards.size());
    }
    else
    {
        spdlog::debug("DRMGPUProbe: No compatible DRM cards found");
    }
}

bool DRMGPUProbe::initialize()
{
    m_Cards = discoverIntelCards();
    for (const auto& card : m_Cards)
    {
        spdlog::debug("DRMGPUProbe: Found Intel GPU at {}", card.cardPath);
    }
    return !m_Cards.empty();
}

std::vector<DRMGPUProbe::DRMCard> DRMGPUProbe::discoverIntelCards() const
{
    // Filter to only Intel GPUs (i915, xe drivers)
    auto cards = discoverDRMCards();
    std::erase_if(cards, [](const DRMCard& card) { return !isIntelGPU(card); });
    // directory_iterator order is unspecified; sorted, two scans of the same cards compare equal.
    std::ranges::sort(cards, {}, &DRMCard::cardPath);
    return cards;
}

bool DRMGPUProbe::rescanGPUs(GPURescan depth)
{
    // A VRAM total the last awake sample's query learned changes the card's GPUInfo (it classifies the
    // card as discrete), and GPUModel only re-enumerates when told to: any rescan reports it (#1283).
    const bool infoStale = std::exchange(m_GPUInfoStale, false);

    // A card's sensors come from which sysfs files it has, not from querying it, so a card asleep at
    // enumeration needs no quick re-check (#1289): only a full rescan looks for changes.
    if (depth != GPURescan::Full)
    {
        return infoStale;
    }

    // Hot-plugged, removed or rebound cards, or a card whose hwmon appeared after the driver bound
    // (#1116). Directory listings and symlinks only: nothing here wakes a sleeping card.
    auto cards = discoverIntelCards();
    const auto sameCard = [](const DRMCard& lhs, const DRMCard& rhs)
    {
        // The energy counter and render node are found at discovery too: one that appears late (a
        // render node registered after the card) is a change, or the card would never get it (#1269, #1283).
        return lhs.gpuId == rhs.gpuId && lhs.cardPath == rhs.cardPath && lhs.hwmonPath == rhs.hwmonPath && lhs.driver == rhs.driver &&
               lhs.energyPath == rhs.energyPath && lhs.temperaturePath == rhs.temperaturePath && lhs.renderNodePath == rhs.renderNodePath;
    };
    if (std::ranges::equal(cards, m_Cards, sameCard))
    {
        discoverDrmClients(); // New DRM clients are found at the full-rescan rate (#1267)
        return infoStale;
    }

    // A card that persists keeps its last-known VRAM total for while it sleeps, and its DRM query
    // results (#1283), so a rescan neither reopens its render node for a total that can't change nor
    // forgets a total it can't re-query while asleep. The query results carry over only while the
    // query would go to the same render node through the same driver; otherwise it's re-issued, and
    // a last-known total that came from the query is dropped with them, so a card that sleeps
    // across the change doesn't report the old query target's capacity.
    for (auto& card : cards)
    {
        const auto previous = std::ranges::find(m_Cards, card.gpuId, &DRMCard::gpuId);
        if (previous == m_Cards.end())
        {
            continue;
        }
        const bool sameQueryTarget = previous->driver == card.driver && previous->renderNodePath == card.renderNodePath;
        if (sameQueryTarget || !previous->lastMemoryTotalQueried)
        {
            card.lastMemoryTotalBytes = previous->lastMemoryTotalBytes;
            card.lastMemoryTotalQueried = previous->lastMemoryTotalQueried;
        }
        if (sameQueryTarget)
        {
            card.vramQueried = previous->vramQueried;
            card.queriedVramTotalBytes = previous->queriedVramTotalBytes;
            card.queriedVramUsedBytes = previous->queriedVramUsedBytes;
        }
    }
    spdlog::info("DRMGPUProbe: Intel DRM cards changed, now {}", cards.size());
    m_Cards = std::move(cards);
    m_Available = !m_Cards.empty();
    discoverDrmClients();
    return true;
}

std::vector<DRMGPUProbe::DRMCard> DRMGPUProbe::discoverDRMCards() const
{
    std::vector<DRMCard> cards;

    std::error_code fsErr;
    if (!Fs::is_directory(m_DrmBasePath, fsErr) || fsErr)
    {
        spdlog::debug("DRMGPUProbe: {} is not a directory or not accessible", m_DrmBasePath);
        return cards;
    }

    // Helper to validate DRM card entry names
    const auto isValidCardName = [](const std::string& name) -> bool
    {
        // Only process card* entries, skip cardX-* connectors and renderD* nodes
        return name.starts_with("card") && !name.contains('-') && !name.starts_with("renderD");
    };

    // Iterate over DRM card entries. Every filesystem call below uses the error_code overload: a
    // sandbox (Snap, Flatpak, AppArmor) can deny parts of /sys, and a throw here would escape the
    // LinuxGPUProbe constructor and abort startup (#1165).
    for (const auto& entryPath : listDirectory(m_DrmBasePath))
    {
        const std::string cardName = entryPath.filename().string();

        // Only process card* entries (skip cardX-* connectors and renderD* for now)
        if (!isValidCardName(cardName))
        {
            continue;
        }

        DRMCard card;
        card.cardPath = entryPath.string();
        card.devicePath = card.cardPath + "/device";

        // Extract card index (card0 -> 0, card1 -> 1)
        try
        {
            card.cardIndex = static_cast<uint32_t>(std::stoul(cardName.substr(4)));
        }
        catch (...)
        {
            continue; // Invalid card name format
        }

        // Check if device symlink exists
        if (!pathExists(card.devicePath))
        {
            spdlog::debug("DRMGPUProbe: Skipping {} - no device symlink (or not accessible)", cardName);
            continue;
        }

        // Read driver name from /sys/class/drm/cardX/device/driver
        card.driver = symlinkTargetName(card.devicePath + "/driver").value_or("");

        // Find hwmon directory for temperature and energy sensors, and the render node for the VRAM query
        card.hwmonPath = findHwmonPath(card.devicePath);
        card.energyPath = findEnergyPath(card.hwmonPath);
        card.temperaturePath = findTemperaturePath(card.hwmonPath);
        card.renderNodePath = findRenderNodePath(card.devicePath);

        // Generate unique GPU ID (use PCI address if available, e.g. 0000:00:02.0), else cardX
        card.gpuId = symlinkTargetName(card.devicePath).value_or(cardName);

        cards.push_back(card);
    }

    return cards;
}

bool DRMGPUProbe::isIntelGPU(const DRMCard& card)
{
    // Intel GPUs use i915 (legacy/current) or xe (future) drivers
    return card.driver == "i915" || card.driver == "xe";
}

std::string DRMGPUProbe::readSysfsString(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return "";
    }

    std::string value;
    std::getline(file, value);

    // Trim whitespace
    const auto start = value.find_first_not_of(" \t\n\r");
    const auto end = value.find_last_not_of(" \t\n\r");

    if (start == std::string::npos)
    {
        return "";
    }

    return value.substr(start, end - start + 1);
}

uint64_t DRMGPUProbe::readSysfsUint64(const std::string& path)
{
    const std::string valueStr = readSysfsString(path);
    if (valueStr.empty())
    {
        return 0;
    }

    try
    {
        return std::stoull(valueStr);
    }
    catch (...)
    {
        return 0;
    }
}

std::optional<uint64_t> DRMGPUProbe::readSysfsOptionalUint64(const std::string& path)
{
    const std::string valueStr = readSysfsString(path);
    if (valueStr.empty() || !std::ranges::all_of(valueStr, [](char c) { return c >= '0' && c <= '9'; }))
    {
        return std::nullopt;
    }
    try
    {
        return std::stoull(valueStr);
    }
    catch (...)
    {
        return std::nullopt; // Out of range
    }
}

std::string DRMGPUProbe::findHwmonPath(const std::string& devicePath)
{
    const std::string hwmonDir = devicePath + "/hwmon";
    std::error_code fsErr;
    if (!Fs::is_directory(hwmonDir, fsErr) || fsErr)
    {
        return "";
    }

    // Find first hwmonX directory
    for (const auto& entryPath : listDirectory(hwmonDir))
    {
        if (entryPath.filename().string().starts_with("hwmon"))
        {
            return entryPath.string();
        }
    }

    return "";
}

std::string DRMGPUProbe::findRenderNodePath(const std::string& devicePath)
{
    // device/drm lists the card's DRM minors (cardN, renderDN) by their /dev/dri names.
    const std::string drmDir = devicePath + "/drm";
    std::error_code fsErr;
    if (!Fs::is_directory(drmDir, fsErr) || fsErr)
    {
        return "";
    }
    for (const auto& entryPath : listDirectory(drmDir))
    {
        const std::string name = entryPath.filename().string();
        if (name.starts_with("renderD"))
        {
            return "/dev/dri/" + name;
        }
    }
    return "";
}

std::string DRMGPUProbe::findEnergyPath(const std::string& hwmonPath)
{
    // Neither i915_hwmon.c nor xe_hwmon.c exposes power1_input (instantaneous power); both expose an
    // accumulating energy counter in µJ. i915 has energy1_input; xe has energy1_input for the card
    // and energy2_input for the package, and on DG2/PVC only the package one (#1269).
    if (hwmonPath.empty())
    {
        return "";
    }
    for (const char* name : {"/energy1_input", "/energy2_input"})
    {
        if (pathExists(hwmonPath + name))
        {
            return hwmonPath + name;
        }
    }
    return "";
}

std::string DRMGPUProbe::findTemperaturePath(const std::string& hwmonPath)
{
    // i915_hwmon.c exposes one unlabelled temp1_input. xe_hwmon.c labels its channels and numbers
    // them from 1 with no temp1_input: the package temperature is temp2_input (label "pkg"), VRAM
    // temp3_input ("vram") where present (#1314). Prefer the package sensor by its label, so a
    // driver that renumbers its channels still gets the right one; otherwise the lowest-numbered
    // input that exists.
    if (hwmonPath.empty())
    {
        return "";
    }
    std::optional<std::uint32_t> lowest;
    for (const auto& entryPath : listDirectory(hwmonPath))
    {
        const std::string name = entryPath.filename().string();
        constexpr std::string_view PREFIX = "temp";
        constexpr std::string_view SUFFIX = "_input";
        if (!name.starts_with(PREFIX) || !name.ends_with(SUFFIX) || name.size() <= PREFIX.size() + SUFFIX.size())
        {
            continue;
        }
        const std::string_view digits = std::string_view(name).substr(PREFIX.size(), name.size() - PREFIX.size() - SUFFIX.size());
        std::uint32_t channel = 0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), channel);
        if (ec != std::errc{} || ptr != digits.data() + digits.size())
        {
            continue;
        }
        if (readSysfsString(std::format("{}/temp{}_label", hwmonPath, channel)) == "pkg")
        {
            return entryPath.string();
        }
        if (!lowest.has_value() || channel < *lowest)
        {
            lowest = channel;
        }
    }
    return lowest.has_value() ? std::format("{}/temp{}_input", hwmonPath, *lowest) : "";
}

std::string DRMGPUProbe::clockPath(const DRMCard& card)
{
    if (card.driver == "xe")
    {
        return card.devicePath + "/tile0/gt0/freq0/cur_freq";
    }
    return card.cardPath + "/gt_cur_freq_mhz";
}

std::string DRMGPUProbe::getVendorName(const std::string& vendorId)
{
    const uint32_t id = parseHexUint32(vendorId);
    switch (id)
    {
    case PCI_VENDOR_INTEL:
        return "Intel";
    case PCI_VENDOR_NVIDIA:
        return "NVIDIA";
    case PCI_VENDOR_AMD:
        return "AMD";
    default:
        return "Unknown";
    }
}

uint32_t DRMGPUProbe::parseHexUint32(const std::string& hexStr)
{
    if (hexStr.empty())
    {
        return 0;
    }
    try
    {
        // std::stoul handles the "0x" prefix automatically with base 16.
        // PCI class codes are 24-bit and vendor IDs are 16-bit, so the result
        // always fits in uint32_t and the static_cast is safe.
        return static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16));
    }
    catch (...)
    {
        // Malformed or unexpected sysfs content: treat as unknown.
        spdlog::debug("DRMGPUProbe: failed to parse hex value '{}'", hexStr);
        return 0;
    }
}

std::optional<uint32_t> DRMGPUProbe::pciBusFromAddress(std::string_view address)
{
    // "DDDD:BB:DD.F": a hex domain (four or more digits), then two-digit bus and device, then the function.
    const auto firstColon = address.find(':');
    if (firstColon == std::string_view::npos || firstColon < 4)
    {
        return std::nullopt;
    }
    const std::string_view rest = address.substr(firstColon + 1);
    constexpr std::size_t BUS_DEVICE_FUNCTION_LENGTH = 7; // "BB:DD.F"
    if (rest.size() != BUS_DEVICE_FUNCTION_LENGTH || rest[2] != ':' || rest[5] != '.')
    {
        return std::nullopt;
    }
    const auto isHex = [](char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    if (!std::ranges::all_of(address.substr(0, firstColon), isHex) || !isHex(rest[0]) || !isHex(rest[1]) || !isHex(rest[3]) ||
        !isHex(rest[4]))
    {
        return std::nullopt;
    }
    return parseHexUint32(std::string(rest.substr(0, 2)));
}

uint64_t DRMGPUProbe::readVramTotal(const DRMCard& card)
{
    // amdgpu-style mem_info_vram_total. Neither i915 nor xe exposes VRAM size in sysfs; they report it
    // only through the DRM memory-region query ioctl (#1283), whose cached total is used instead. Until
    // the first awake sample has queried it, an Intel dGPU is told apart by its PCI bus (detectIsIntegrated).
    const uint64_t sysfsTotal = readSysfsUint64(card.devicePath + "/mem_info_vram_total");
    return sysfsTotal > 0 ? sysfsTotal : card.queriedVramTotalBytes;
}

std::optional<DRMGPUProbe::VramInfo> DRMGPUProbe::summarizeXeMemRegions([[maybe_unused]] std::span<const std::byte> reply)
{
#if TASKSMACK_HAS_DRM_QUERY_UAPI
    // struct drm_xe_query_mem_regions { __u32 num_mem_regions; __u32 pad; struct drm_xe_mem_region mem_regions[]; }
    constexpr std::size_t FIRST_REGION = offsetof(drm_xe_query_mem_regions, mem_regions);
    if (reply.size() < FIRST_REGION)
    {
        return std::nullopt;
    }
    const auto count = readAt<uint32_t>(reply, offsetof(drm_xe_query_mem_regions, num_mem_regions));
    if (count > (reply.size() - FIRST_REGION) / sizeof(drm_xe_mem_region))
    {
        return std::nullopt;
    }
    VramInfo info;
    uint64_t used = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto region = readAt<drm_xe_mem_region>(reply, FIRST_REGION + (i * sizeof(drm_xe_mem_region)));
        if (region.mem_class == DRM_XE_MEM_REGION_CLASS_VRAM)
        {
            info.totalBytes += region.total_size;
            used += region.used;
        }
    }
    // Kernels that gate `used` on CAP_PERFMON report 0 without it; VRAM holding nothing at all (not
    // even the driver's own buffers) doesn't happen, so 0 means "not reported".
    if (info.totalBytes > 0 && used > 0)
    {
        info.usedBytes = used;
    }
    return info;
#else
    return std::nullopt;
#endif
}

std::optional<DRMGPUProbe::VramInfo> DRMGPUProbe::summarizeI915MemRegions([[maybe_unused]] std::span<const std::byte> reply)
{
#if TASKSMACK_HAS_DRM_QUERY_UAPI
    // struct drm_i915_query_memory_regions { __u32 num_regions; __u32 rsvd[3]; struct drm_i915_memory_region_info regions[]; }
    constexpr std::size_t FIRST_REGION = offsetof(drm_i915_query_memory_regions, regions);
    if (reply.size() < FIRST_REGION)
    {
        return std::nullopt;
    }
    const auto count = readAt<uint32_t>(reply, offsetof(drm_i915_query_memory_regions, num_regions));
    if (count > (reply.size() - FIRST_REGION) / sizeof(drm_i915_memory_region_info))
    {
        return std::nullopt;
    }
    VramInfo info;
    uint64_t used = 0;
    bool usedReported = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto region = readAt<drm_i915_memory_region_info>(reply, FIRST_REGION + (i * sizeof(drm_i915_memory_region_info)));
        if (region.region.memory_class != I915_MEMORY_CLASS_DEVICE || region.unallocated_size > region.probed_size)
        {
            continue;
        }
        info.totalBytes += region.probed_size;
        used += region.probed_size - region.unallocated_size;
        // Without CAP_PERFMON (or on an older kernel) unallocated_size always equals probed_size.
        usedReported = usedReported || region.unallocated_size != region.probed_size;
    }
    if (info.totalBytes > 0 && usedReported)
    {
        info.usedBytes = used;
    }
    return info;
#else
    return std::nullopt;
#endif
}

std::optional<DRMGPUProbe::VramInfo> DRMGPUProbe::queryVramByIoctl([[maybe_unused]] const std::string& renderNodePath,
                                                                   [[maybe_unused]] const std::string& driver)
{
#if TASKSMACK_HAS_DRM_QUERY_UAPI
    if (renderNodePath.empty() || (driver != "xe" && driver != "i915"))
    {
        return std::nullopt;
    }
    // Read-only is enough: DRM ioctls don't check the file mode, and both queries are DRM_RENDER_ALLOW.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- open(2) is variadic
    const FdGuard fd(::open(renderNodePath.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        spdlog::debug(
            "DRMGPUProbe: can't open {} for the VRAM query: {}", renderNodePath, std::error_code(errno, std::generic_category()).message());
        return std::nullopt;
    }

    if (driver == "xe")
    {
        // Two calls: size 0 asks for the reply size, then the kernel fills a buffer of exactly that size.
        drm_xe_device_query query{};
        query.query = DRM_XE_DEVICE_QUERY_MEM_REGIONS;
        if (drmIoctl(fd.get(), DRM_IOCTL_XE_DEVICE_QUERY, &query) != 0 || query.size == 0)
        {
            return std::nullopt;
        }
        const std::size_t size = query.size;
        auto buffer = makeReplyBuffer(size);
        query.data = reinterpret_cast<uintptr_t>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        if (drmIoctl(fd.get(), DRM_IOCTL_XE_DEVICE_QUERY, &query) != 0)
        {
            return std::nullopt;
        }
        return summarizeXeMemRegions(replyBytes(buffer, size));
    }

    // i915: the same two-step protocol through a drm_i915_query_item; a negative length is an error.
    drm_i915_query_item item{};
    item.query_id = DRM_I915_QUERY_MEMORY_REGIONS;
    drm_i915_query query{};
    query.num_items = 1;
    query.items_ptr = reinterpret_cast<uintptr_t>(&item); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (drmIoctl(fd.get(), DRM_IOCTL_I915_QUERY, &query) != 0 || item.length <= 0)
    {
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(item.length);
    auto buffer = makeReplyBuffer(size);
    item.data_ptr = reinterpret_cast<uintptr_t>(buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (drmIoctl(fd.get(), DRM_IOCTL_I915_QUERY, &query) != 0 || item.length <= 0)
    {
        return std::nullopt;
    }
    return summarizeI915MemRegions(replyBytes(buffer, std::min(size, static_cast<std::size_t>(item.length))));
#else
    return std::nullopt;
#endif
}

void DRMGPUProbe::refreshQueriedVram(DRMCard& card)
{
    // Once the total is known the query is repeated only for the used figure, and only while the
    // kernel reports it: otherwise every sample would open the render node for a number that can't change.
    if (card.vramQueried && !card.queriedVramUsedBytes.has_value())
    {
        return;
    }
    card.vramQueried = true;
    if (card.renderNodePath.empty())
    {
        return;
    }
    const auto info = m_VramQuery(card.renderNodePath, card.driver);
    if (!info.has_value())
    {
        card.queriedVramUsedBytes.reset(); // Keep a cached total; stop re-querying
        return;
    }
    if (info->totalBytes > 0 && info->totalBytes != card.queriedVramTotalBytes)
    {
        card.queriedVramTotalBytes = info->totalBytes;
        m_GPUInfoStale = true; // Published GPUInfo was built without it: see rescanGPUs()
    }
    card.queriedVramUsedBytes = info->usedBytes;
}

std::optional<DRMGPUProbe::DrmFdinfo> DRMGPUProbe::parseFdinfo(std::string_view text, std::uint64_t monotonicNs)
{
    constexpr std::string_view CAPACITY_PREFIX = "drm-engine-capacity-";
    constexpr std::string_view ENGINE_PREFIX = "drm-engine-";             // i915: busy nanoseconds ("123 ns")
    constexpr std::string_view CYCLES_PREFIX = "drm-cycles-";             // xe: busy GPU-timestamp cycles
    constexpr std::string_view TOTAL_CYCLES_PREFIX = "drm-total-cycles-"; // xe: the GPU timestamp itself

    DrmFdinfo info;
    bool haveClientId = false;
    std::array<std::optional<std::uint64_t>, GPU_ENGINE_CLASS_COUNT> busyNs{};
    std::array<std::optional<std::uint64_t>, GPU_ENGINE_CLASS_COUNT> cycles{};
    std::array<std::optional<std::uint64_t>, GPU_ENGINE_CLASS_COUNT> totalCycles{};
    std::array<std::uint32_t, GPU_ENGINE_CLASS_COUNT> capacity{};
    capacity.fill(1);

    // "key:\tvalue" lines; the value's leading number is what's wanted, units ("ns") aside.
    const auto perClass = [](std::string_view key, std::string_view prefix, std::string_view value) -> std::optional<std::size_t>
    {
        const auto index = engineClassIndex(key.substr(prefix.size()));
        return (index.has_value() && leadingUint64(value).has_value()) ? index : std::nullopt;
    };
    while (!text.empty())
    {
        const auto eol = text.find('\n');
        const std::string_view line = text.substr(0, eol);
        text = (eol == std::string_view::npos) ? std::string_view{} : text.substr(eol + 1);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = line.substr(0, colon);
        const std::string_view value = line.substr(colon + 1);
        if (key == "drm-client-id")
        {
            if (const auto id = leadingUint64(value))
            {
                info.client.clientId = *id;
                haveClientId = true;
            }
        }
        else if (key == "drm-pdev")
        {
            info.pdev = std::string(trimmed(value));
        }
        else if (key.starts_with(CAPACITY_PREFIX))
        {
            const auto index = perClass(key, CAPACITY_PREFIX, value);
            const auto engines = leadingUint64(value);
            if (index.has_value() && engines.has_value() && *engines > 0 && *engines <= std::numeric_limits<std::uint32_t>::max())
            {
                capacity.at(*index) = static_cast<std::uint32_t>(*engines); // Range-checked just above
            }
        }
        else if (key.starts_with(TOTAL_CYCLES_PREFIX))
        {
            if (const auto index = perClass(key, TOTAL_CYCLES_PREFIX, value))
            {
                totalCycles.at(*index) = leadingUint64(value);
            }
        }
        else if (key.starts_with(CYCLES_PREFIX))
        {
            if (const auto index = perClass(key, CYCLES_PREFIX, value))
            {
                cycles.at(*index) = leadingUint64(value);
            }
        }
        else if (key.starts_with(ENGINE_PREFIX))
        {
            if (const auto index = perClass(key, ENGINE_PREFIX, value))
            {
                busyNs.at(*index) = leadingUint64(value);
            }
        }
    }
    if (!haveClientId)
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < GPU_ENGINE_CLASS_COUNT; ++i)
    {
        auto& engine = info.client.engines.at(i);
        engine.capacity = capacity.at(i);
        if (cycles.at(i).has_value() && totalCycles.at(i).has_value())
        {
            engine.available = true;
            engine.busy = *cycles.at(i);
            engine.total = *totalCycles.at(i);
        }
        else if (busyNs.at(i).has_value())
        {
            engine.available = true;
            engine.busy = *busyNs.at(i);
            engine.total = monotonicNs;
        }
        info.hasEngineStats = info.hasEngineStats || engine.available;
    }
    return info;
}

void DRMGPUProbe::discoverDrmClients()
{
    m_ClientsDiscovered = true;
    if (m_Cards.empty())
    {
        return;
    }

    // Each card's DRM nodes as an fd link names them, and the card they belong to.
    std::vector<std::pair<std::string, std::size_t>> nodes;
    for (std::size_t i = 0; i < m_Cards.size(); ++i)
    {
        nodes.emplace_back(std::format("/dev/dri/card{}", m_Cards[i].cardIndex), i);
        if (!m_Cards[i].renderNodePath.empty())
        {
            nodes.emplace_back(m_Cards[i].renderNodePath, i);
        }
    }

    // Built whole, then committed: a walk cut short by an exception leaves the last lists in place.
    std::vector<std::vector<std::string>> found(m_Cards.size());
    // Whether the walk could see clients at all, apart from whether it found any: an empty list
    // from a /proc that can't be listed, or whose every process's fds are denied (a sandbox), is
    // no evidence the card is idle, so its busyness stays unread (N/A) rather than 0% (#1267).
    bool anyFdDirRead = false;
    std::error_code procErr;
    Fs::directory_iterator procIt(m_ProcRoot, procErr);
    for (const Fs::directory_iterator procEnd; !procErr && procIt != procEnd; procIt.increment(procErr))
    {
        const Fs::path procEntry = procIt->path();
        const std::string pid = procEntry.filename().string();
        if (pid.empty() || !std::ranges::all_of(pid, [](char c) { return c >= '0' && c <= '9'; }))
        {
            continue;
        }
        // Another user's process can't be looked into (EACCES) without privileges, and a process can
        // exit mid-walk: either just has no fds here. Iterated directly, not with listDirectory(), so
        // that every unreadable process isn't logged.
        std::error_code fsErr;
        Fs::directory_iterator fds(procEntry / "fd", fsErr);
        anyFdDirRead = anyFdDirRead || !fsErr;
        for (const Fs::directory_iterator end; !fsErr && fds != end; fds.increment(fsErr))
        {
            std::error_code linkErr;
            const auto target = Fs::read_symlink(fds->path(), linkErr);
            if (linkErr || !target.native().starts_with("/dev/dri/"))
            {
                continue;
            }
            const auto node = std::ranges::find(nodes, target.native(), &std::pair<std::string, std::size_t>::first);
            if (node != nodes.end())
            {
                found[node->second].push_back((procEntry / "fdinfo" / fds->path().filename()).string());
            }
        }
    }
    if (procErr)
    {
        // Unlisted, or cut short mid-walk: either way, the clients found may not be all of them.
        spdlog::debug("DRMGPUProbe: failed to list {}: {}", m_ProcRoot, procErr.message());
    }
    m_ClientScanReliable = !procErr && anyFdDirRead;
    for (std::size_t i = 0; i < m_Cards.size(); ++i)
    {
        // Reconciled with the clients the samples so far have grouped, rather than replacing them: a
        // full rescan every few seconds would otherwise make the next sample read every dup'd and
        // inherited fd again (#1356). A path still found keeps its client and place; a path no longer
        // found is dropped, and with it a client left with none. A new path is its own client until a
        // read gives its drm-client-id: telling dup'd and inherited fds apart needs their fdinfo, which
        // xe can't report without waking the card (#1117), so the next awake sample reads each new path
        // once and readEngineClients() merges those sharing an id with a client already known.
        auto& clients = m_Cards[i].clients;
        // Everything that allocates comes first, so that an exception leaves this card's clients as
        // they were; the reconciliation below only moves and erases.
        const std::unordered_set<std::string_view> foundPaths(found[i].begin(), found[i].end());
        std::unordered_set<std::string_view> knownPaths;
        for (const auto& client : clients)
        {
            knownPaths.insert(client.fdinfoPaths.begin(), client.fdinfoPaths.end());
        }
        std::vector<DrmClientFds> added;
        for (const auto& path : found[i])
        {
            if (!knownPaths.contains(path))
            {
                added.push_back(DrmClientFds{.clientId = std::nullopt, .fdinfoPaths = {path}});
            }
        }
        std::vector<DrmClientFds> reconciled;
        reconciled.reserve(clients.size() + added.size());

        for (auto& client : clients)
        {
            std::erase_if(client.fdinfoPaths, [&foundPaths](const std::string& path) { return !foundPaths.contains(path); });
            if (!client.fdinfoPaths.empty())
            {
                reconciled.push_back(std::move(client));
            }
        }
        std::ranges::move(added, std::back_inserter(reconciled));
        clients = std::move(reconciled);
    }
}

DRMGPUProbe::ClientFdinfoRead DRMGPUProbe::readClientFdinfo(const std::string& path, const DRMCard& card)
{
    using Kind = ClientFdinfoRead::Kind;
    ++m_FdinfoReads;
    std::ifstream file(path);
    if (!file.is_open())
    {
        return {}; // The fd was closed or its process exited
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto info = parseFdinfo(text, m_Clock());
    if (!info.has_value())
    {
        // No drm-client-id: the fd number now names another file, or this is a DRM file on a kernel
        // that prints no drm-* keys at all (i915 before Linux 5.19, whose fdinfo is only pos, flags,
        // mnt_id and ino). Only the fd's link tells them apart (#1361).
        return fdLinksToCard(path, card) ? ClientFdinfoRead{.kind = Kind::WithoutClientId, .info = {}} : ClientFdinfoRead{};
    }
    // drm-pdev can be checked only against a card whose id is its PCI address.
    if (pciBusFromAddress(card.gpuId).has_value() && !info->pdev.empty() && info->pdev != card.gpuId)
    {
        return {}; // The fd number now names another device's DRM file
    }
    return {.kind = Kind::Client, .info = std::move(*info)};
}

bool DRMGPUProbe::fdLinksToCard(const std::string& fdinfoPath, const DRMCard& card)
{
    const Fs::path fdinfo(fdinfoPath);
    std::error_code linkErr;
    const auto target = Fs::read_symlink(fdinfo.parent_path().parent_path() / "fd" / fdinfo.filename(), linkErr);
    if (linkErr)
    {
        return false; // Closed since its fdinfo was read, or its process exited
    }
    const std::string& node = target.native();
    return node == std::format("/dev/dri/card{}", card.cardIndex) || (!card.renderNodePath.empty() && node == card.renderNodePath);
}

void DRMGPUProbe::readEngineClients(DRMCard& card, GPUCounters& counter)
{
    bool anyEngineStats = false;
    std::vector<DrmClientFds> kept;
    kept.reserve(card.clients.size());
    // Files read as client `info`'s: joined to the client already kept under its id -- dup'd and
    // inherited fds share one DRM file, and so one client id, which counts once -- or kept as a new one.
    const auto keep = [&](std::vector<std::string> paths, const DrmFdinfo& info)
    {
        anyEngineStats = anyEngineStats || info.hasEngineStats;
        const auto same = std::ranges::find(kept, std::optional<std::uint64_t>(info.client.clientId), &DrmClientFds::clientId);
        if (same != kept.end())
        {
            same->fdinfoPaths.insert(same->fdinfoPaths.end(), std::make_move_iterator(paths.begin()), std::make_move_iterator(paths.end()));
            return;
        }
        kept.push_back(DrmClientFds{.clientId = info.client.clientId, .fdinfoPaths = std::move(paths)});
        if (info.hasEngineStats)
        {
            counter.engineClients.push_back(info.client);
        }
    };
    for (auto& client : card.clients)
    {
        // The first path that still reads as a DRM file of this card is the client's this sample:
        // normally the first one; an alias only once the paths before it have closed (#1356).
        auto& paths = client.fdinfoPaths;
        std::size_t next = 0;
        while (next < paths.size())
        {
            const auto read = readClientFdinfo(paths[next], card);
            if (read.kind == ClientFdinfoRead::Kind::Gone)
            {
                ++next; // Closed, or no longer this card's DRM file: dropped
                continue;
            }
            if (read.kind == ClientFdinfoRead::Kind::WithoutClientId)
            {
                // A client without usage stats (#1361): without an id it can't be matched to its aliases,
                // so each such path is kept as a client of its own, and the rest of the paths read on.
                kept.push_back(DrmClientFds{.clientId = std::nullopt, .fdinfoPaths = {std::move(paths[next])}});
                ++next;
                continue;
            }
            const auto& info = read.info;
            if (client.clientId.has_value() && client.clientId != info.client.clientId)
            {
                // The fd number was reused for another DRM file: it is that client's now, while the
                // rest of the paths may still name this one.
                keep({std::move(paths[next])}, info);
                ++next;
                continue;
            }
            paths.erase(paths.begin(), paths.begin() + static_cast<std::ptrdiff_t>(next)); // next < size()
            keep(std::move(paths), info);
            break;
        }
    }
    card.clients = std::move(kept);
    // Unread when the card has clients and none reports any busyness: a kernel without fdinfo engine
    // stats (i915 before Linux 5.19, whose clients are kept without an id). A card with no clients at
    // all is idle. Either needs a /proc walk that could see every client: an unlistable /proc, a walk
    // cut short, or every process's fds denied leaves the client set partial, so neither idle nor the
    // busyness of the clients found is published.
    counter.engineBusyAvailable = m_ClientScanReliable && (anyEngineStats || card.clients.empty());
}

bool DRMGPUProbe::detectIsIntegrated(
    const std::string& vendorId, uint32_t pciClass, uint64_t vramTotal, std::optional<uint32_t> pciBus, bool amdApu)
{
    const uint32_t classSubclass = (pciClass & PCI_CLASS_SUBCLASS_MASK);
    const uint32_t vendor = parseHexUint32(vendorId);

    // AMD by the rule the ROCm probe uses (AmdApu::isAmdApu, #1344), before the VRAM test below: an
    // APU reports its BIOS carve-out (512 MiB-2 GiB of system RAM) as mem_info_vram_total, and is a
    // VGA controller like any Radeon card, so neither VRAM nor class tells it from a discrete GPU.
    if (vendor == PCI_VENDOR_AMD)
    {
        return amdApu;
    }

    // Dedicated memory means a discrete GPU, whatever its class or bus.
    if (vramTotal > 0)
    {
        return false;
    }

    // 3D controller (compute-only, no display output) is always discrete.
    // Examples: NVIDIA MX/RTX laptop cards, Intel Arc in compute mode.
    if (classSubclass == PCI_CLASS_3D_CONTROLLER)
    {
        return false;
    }

    // Intel iGPUs are root-complex integrated endpoints on bus 0 (always 00:02.0); Intel discrete
    // GPUs (DG1, Arc A/B) sit behind a PCIe switch on a non-zero bus. Unlike VRAM files, the bus is
    // there under both i915 and xe (#1113).
    if ((vendor == PCI_VENDOR_INTEL || vendor == 0) && pciBus.has_value())
    {
        return *pciBus == 0;
    }

    // VGA-compatible controllers have display output.
    // Intel VGA GPUs are integrated unless they carry dedicated VRAM (e.g., Arc discrete).
    // Other vendors' VGA controllers (NVIDIA) are discrete; AMD was decided above.
    // If the vendor is unknown (e.g., /vendor file missing), fall back conservatively to
    // VRAM presence rather than incorrectly classifying as discrete.
    if (classSubclass == PCI_CLASS_VGA_COMPATIBLE)
    {
        if (vendor == PCI_VENDOR_INTEL || vendor == 0)
        {
            // Intel iGPU (or unknown vendor — conservative): integrated unless VRAM is present.
            return vramTotal == 0;
        }
        return false; // NVIDIA VGA controllers are discrete
    }

    // Display controllers that are not VGA-compatible (e.g., Intel Arc on some platforms).
    // Use VRAM presence as the tiebreaker for Intel; others are discrete.
    // Same conservative fallback for unknown vendor.
    if (classSubclass == PCI_CLASS_DISPLAY_CONTROLLER)
    {
        if (vendor == PCI_VENDOR_INTEL || vendor == 0)
        {
            return vramTotal == 0;
        }
        return false;
    }

    // Unrecognised PCI class: fall back to VRAM presence.
    // No VRAM → assume integrated (conservative default).
    return vramTotal == 0;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
GPUInfo DRMGPUProbe::cardToGPUInfo(const DRMCard& card) const
{
    GPUInfo info{};
    info.id = card.gpuId;

    // Read vendor ID from sysfs
    const std::string vendorPath = card.devicePath + "/vendor";
    const std::string vendorId = readSysfsString(vendorPath);
    info.vendor = getVendorName(vendorId);

    // Read device name from sysfs (PCI device string)
    const std::string devicePath = card.devicePath + "/device";
    const std::string deviceId = readSysfsString(devicePath);

    // Try to read a human-readable name from uevent
    const std::string ueventPath = card.devicePath + "/uevent";
    std::string deviceName = readSysfsString(ueventPath);

    // If uevent doesn't give us a good name, use PCI IDs
    if (deviceName.empty() || !deviceName.contains("PCI_ID"))
    {
        deviceName = info.vendor + " GPU (" + vendorId + ":" + deviceId + ")";
    }
    else
    {
        // Extract device name from uevent if available
        // Format: PCI_ID=8086:XXXX
        const auto pciIdPos = deviceName.find("PCI_ID=");
        if (pciIdPos != std::string::npos)
        {
            const auto idStr = deviceName.substr(pciIdPos + 7, 9); // 8086:XXXX
            deviceName = info.vendor + " GPU (" + idStr + ")";
        }
    }

    info.name = deviceName;

    // Dedicated memory, if the driver reports it (see readVramTotal(); the ioctl's total once queried).
    const uint64_t vramTotal = readVramTotal(card);

    // Read PCI class from sysfs to distinguish integrated from discrete using
    // the PCI class/subclass, with VRAM presence as a secondary signal.
    // /sys/class/drm/cardX/device/class contains the 24-bit PCI class code, e.g. "0x030000".
    const std::string pciClassPath = card.devicePath + "/class";
    const std::string pciClassStr = readSysfsString(pciClassPath);
    const uint32_t pciClass = parseHexUint32(pciClassStr);

    // An AMD GPU's APU signals (ip_discovery GC version, PCI device id) are cached sysfs attributes,
    // read only for AMD: they never wake a sleeping card (#1117).
    const bool amdApu = parseHexUint32(vendorId) == PCI_VENDOR_AMD && AmdApu::isAmdApuDevice(card.devicePath);
    info.isIntegrated = detectIsIntegrated(vendorId, pciClass, vramTotal, pciBusFromAddress(card.gpuId), amdApu);

    // Which sensors this card has (#1112). The probe-wide capabilities are OR'd with NVML's and
    // ROCm's on Linux, so without this an Intel iGPU beside an NVIDIA dGPU drew NVML's Power and Fan
    // series stuck at 0, and a temperature line although i915 iGPUs have no hwmon at all.
    GPUCapabilities sensors = capabilities();
    std::error_code fsErr;
    sensors.hasTemperature = !card.temperaturePath.empty(); // #1314
    sensors.hasClockSpeeds = Fs::exists(clockPath(card), fsErr);
    sensors.hasPowerMetrics = !card.energyPath.empty();
    info.sensorCapabilities = sensors;

    return info;
}

std::vector<GPUInfo> DRMGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;
    gpus.reserve(m_Cards.size());

    for (const auto& card : m_Cards)
    {
        gpus.push_back(cardToGPUInfo(card));
    }

    return gpus;
}

std::vector<GPUCounters> DRMGPUProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;

    // The first read finds the cards' DRM clients; later ones are found at each full rescan (#1267).
    if (!m_ClientsDiscovered)
    {
        discoverDrmClients();
    }

    for (auto& card : m_Cards)
    {
        GPUCounters counter{};
        counter.gpuId = card.gpuId;

        // A runtime-suspended card is left alone (#1117): an i915/xe dGPU's hwmon read takes a
        // runtime-PM reference and would wake it every sample.
        if (PciRuntimePm::isRuntimeSuspended(card.devicePath))
        {
            counter.suspended = true;
            counter.utilizationAvailable = false;
            counter.temperatureAvailable = false;
            counter.powerAvailable = false;
            counter.gpuClockAvailable = false;
            counter.memoryAvailable = false;
            counter.memoryTotalBytes = card.lastMemoryTotalBytes; // Known capacity, not re-read
            counters.push_back(counter);
            continue;
        }

        // Read temperature from hwmon (if available). capabilities() advertises temperature for
        // every card, so a card without hwmon has an unread temperature, not 0 °C (#1111).
        if (card.temperaturePath.empty())
        {
            counter.temperatureAvailable = false;
        }
        else
        {
            // i915's temp1_input or xe's package temp2_input, in millidegrees Celsius (#1314)
            const uint64_t tempMilliC = readSysfsUint64(card.temperaturePath);
            if (tempMilliC > 0)
            {
                counter.temperatureC = static_cast<std::int32_t>(tempMilliC / 1000);
            }
            else
            {
                counter.temperatureAvailable = false; // Unread (0 means the read failed), not 0 °C (#1111)
            }
        }

        // GPU frequency in MHz: i915's cardX/gt_cur_freq_mhz, or xe's device/tile0/gt0/freq0/cur_freq (#1268)
        const uint64_t freqMhz = readSysfsUint64(clockPath(card));
        if (freqMhz > 0)
        {
            counter.gpuClockMHz = static_cast<uint32_t>(freqMhz);
        }
        else
        {
            counter.gpuClockAvailable = false;
        }

        // The hwmon energy counter, which Domain turns into the power draw (#1269); neither i915 nor
        // xe reports instantaneous power. A card without one (i915 iGPUs have no hwmon at all), or a
        // failed read, has no power this sample.
        const auto energy = card.energyPath.empty() ? std::nullopt : readSysfsOptionalUint64(card.energyPath);
        counter.powerAvailable = false;
        counter.energyAvailable = energy.has_value();
        counter.energyMicroJoules = energy.value_or(0);

        // Memory used/total, where the driver reports both (mem_info_vram_used/_total). i915/xe report
        // neither in sysfs; their VRAM comes from the DRM memory-region query ioctl, issued here only
        // because the card is awake (#1283): the total once, used each sample only while the kernel
        // reports it. An iGPU has no dedicated memory, so memory there is not read rather than
        // published as a real-looking 0% (#1115). A known total is remembered for while the card sleeps.
        std::optional<uint64_t> usedBytes = readSysfsOptionalUint64(card.devicePath + "/mem_info_vram_used");
        uint64_t totalBytes = readSysfsUint64(card.devicePath + "/mem_info_vram_total");
        card.lastMemoryTotalQueried = (totalBytes == 0);
        if (card.lastMemoryTotalQueried)
        {
            refreshQueriedVram(card);
            totalBytes = card.queriedVramTotalBytes;
            usedBytes = card.queriedVramUsedBytes;
        }
        counter.memoryTotalBytes = totalBytes;
        card.lastMemoryTotalBytes = totalBytes;
        if (usedBytes.has_value() && totalBytes > 0)
        {
            counter.memoryUsedBytes = *usedBytes;
        }
        else
        {
            counter.memoryAvailable = false;
        }

        // GPU utilization: i915/xe report none of their own, so utilizationPercent is never read; Domain
        // derives it from the engine busyness of the card's DRM clients (#1267), read only while it's awake.
        counter.utilizationAvailable = false;
        readEngineClients(card, counter);

        counters.push_back(counter);
    }

    return counters;
}

std::vector<ProcessGPUCounters> DRMGPUProbe::readProcessGPUCounters()
{
    // The DRM clients' fdinfo is read for the card's utilization (#1267), but not yet attributed to
    // processes: per-process GPU metrics stay unsupported here.
    return {};
}

GPUCapabilities DRMGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    if (!m_Available)
    {
        return caps;
    }

    // DRM probe supports temperature and clock speeds for Intel
    caps.hasTemperature = true;
    caps.hasClockSpeeds = true;

    // Memory metrics available for discrete Intel GPUs only
    // (integrated GPUs use system RAM, not tracked separately)
    caps.supportsMultiGPU = m_Cards.size() > 1;

    // Limited capabilities compared to NVML/ROCm
    caps.hasHotspotTemp = false;
    // Power from the hwmon energy counter, where a card has one (#1269)
    caps.hasPowerMetrics = std::ranges::any_of(m_Cards, [](const DRMCard& card) { return !card.energyPath.empty(); });
    caps.hasFanSpeed = false;
    caps.hasPCIeMetrics = false;
    caps.hasEngineUtilization = false;
    caps.hasPerProcessMetrics = false;
    caps.hasEncoderDecoder = false;

    return caps;
}

} // namespace Platform
