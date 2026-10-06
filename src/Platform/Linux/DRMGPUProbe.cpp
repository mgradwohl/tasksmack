#include "DRMGPUProbe.h"

#include "PciRuntimePm.h"
#include "Platform/GPUTypes.h"
#include "PosixGuards.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>

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

DRMGPUProbe::DRMGPUProbe(std::string drmBasePath, VramQuery vramQuery)
    : m_DrmBasePath(std::move(drmBasePath)), m_VramQuery(std::move(vramQuery))
{
    if (!m_VramQuery)
    {
        m_VramQuery = &DRMGPUProbe::queryVramByIoctl;
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
    const Posix::FdGuard fd(::open(renderNodePath.c_str(), O_RDONLY | O_CLOEXEC));
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

bool DRMGPUProbe::detectIsIntegrated(const std::string& vendorId, uint32_t pciClass, uint64_t vramTotal, std::optional<uint32_t> pciBus)
{
    const uint32_t classSubclass = (pciClass & PCI_CLASS_SUBCLASS_MASK);
    const uint32_t vendor = parseHexUint32(vendorId);

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
    // Non-Intel VGA controllers (NVIDIA/AMD) are discrete.
    // If the vendor is unknown (e.g., /vendor file missing), fall back conservatively to
    // VRAM presence rather than incorrectly classifying as discrete.
    if (classSubclass == PCI_CLASS_VGA_COMPATIBLE)
    {
        if (vendor == PCI_VENDOR_INTEL || vendor == 0)
        {
            // Intel iGPU (or unknown vendor — conservative): integrated unless VRAM is present.
            return vramTotal == 0;
        }
        return false; // NVIDIA/AMD VGA controllers are discrete
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

    info.isIntegrated = detectIsIntegrated(vendorId, pciClass, vramTotal, pciBusFromAddress(card.gpuId));

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

        // GPU utilization: Not directly available via sysfs for Intel
        // Would require reading i915_gem_objects debugfs or using IGT tools (future enhancement, #1115).
        // Never read, so it publishes as a gap / N/A rather than a real-looking 0% (#1111).
        counter.utilizationAvailable = false;

        counters.push_back(counter);
    }

    return counters;
}

std::vector<ProcessGPUCounters> DRMGPUProbe::readProcessGPUCounters()
{
    // Per-process GPU metrics are not exposed via DRM sysfs for Intel
    // Would require fdinfo parsing or DRM client stats (kernel 5.19+)
    // Not in scope for Phase 5
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
