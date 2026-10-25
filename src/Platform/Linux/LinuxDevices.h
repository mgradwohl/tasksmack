#pragma once

// The Linux Devices facts (#1520), read under an injected root ("/" in the app, a fixture tree in tests),
// all unprivileged and enumeration only:
// - PCI: each /sys/bus/pci/devices/* entry's vendor, device and class, and its uevent's DRIVER (the
//   driver link's name) and PCI_SLOT_NAME. A device no driver is bound to is a problem device, except
//   memory controllers, bridges and system peripherals, which commonly have none.
// - USB: each /sys/bus/usb/devices/* device (not its interfaces, nor the root hubs): idVendor, idProduct,
//   manufacturer, product, speed and serial; its parent hub from its port path ("1-1.2" is behind "1-1").
// - Names for the ids from hwdata's pci.ids / usb.ids (Platform/HwIdsParser.h) where installed;
//   without them the section shows the hex ids.
// - Audio: ALSA's /proc/asound/cards and the playback and capture PCM devices in /proc/asound/pcm.
// Standard library only, so the fixture tests run on every platform.

#include "Platform/HwIdsParser.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxGraphics.h"
#include "Platform/Linux/LinuxOsInfo.h"
#include "Platform/Linux/LinuxStorage.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::LinuxDevices
{

/// A sysfs hex id ("0x8086", "046d", "0x030000"); 0 when unparsable.
[[nodiscard]] inline std::uint32_t hexValue(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.remove_prefix(2);
    }
    std::uint32_t value = 0;
    static_cast<void>(std::from_chars(text.data(), text.data() + text.size(), value, 16)); // stops at the newline
    return value;
}

/// A USB device's speed file in Mbps ("1.5", "12", "480", "5000"); 0 when unparsable. Integer
/// parsing, as libc++'s floating-point from_chars can't be relied on.
[[nodiscard]] inline double usbSpeedMbps(std::string_view text) noexcept
{
    std::uint32_t whole = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), whole);
    if (ec != std::errc{})
    {
        return 0.0;
    }
    const std::string_view rest(end, text.data() + text.size());
    const bool tenth = rest.size() >= 2 && rest[0] == '.' && rest[1] >= '0' && rest[1] <= '9';
    return static_cast<double>(whole) + (tenth ? (rest[1] - '0') / 10.0 : 0.0);
}

/// The first of /usr/share/hwdata, /usr/share/misc and /usr/share that has @p fileName ("pci.ids");
/// empty when none does.
[[nodiscard]] inline std::string readIdsFile(const std::filesystem::path& root, std::string_view fileName)
{
    for (const char* dir : {"usr/share/hwdata", "usr/share/misc", "usr/share"})
    {
        std::string text = LinuxOsInfo::readFile(root / dir / fileName);
        if (!text.empty())
        {
            return text;
        }
    }
    return {};
}

/// A directory's entry names, sorted; ok false when it can't be listed.
[[nodiscard]] inline std::vector<std::string> sortedEntries(const std::filesystem::path& dir, bool& ok)
{
    std::vector<std::string> names;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    ok = !ec;
    for (; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
    {
        names.push_back(it->path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

/// Names each device from @p ids (the text of pci.ids or usb.ids): the vendor and device where the
/// probe found none. Returns the parsed names for further lookups.
inline HwIds::IdNames nameDevices(std::vector<Device>& devices, std::string_view ids)
{
    std::set<std::uint16_t> vendors;
    std::set<std::uint32_t> keys;
    for (const Device& device : devices)
    {
        vendors.insert(device.vendorId);
        keys.insert(HwIds::deviceKey(device.vendorId, device.productId));
    }
    HwIds::IdNames names = HwIds::parseIds(ids, vendors, keys);
    for (Device& device : devices)
    {
        if (const auto it = names.vendors.find(device.vendorId); device.vendor.empty() && it != names.vendors.end())
        {
            device.vendor = it->second;
        }
        if (const auto it = names.devices.find(HwIds::deviceKey(device.vendorId, device.productId));
            device.name.empty() && it != names.devices.end())
        {
            device.name = it->second;
        }
    }
    return names;
}

/// Whether a PCI device of @p classCode (0xCCSSPP) commonly runs without a driver: memory controllers,
/// bridges (host, ISA, PCI) and generic system peripherals (IOMMU, timers).
[[nodiscard]] constexpr bool driverlessIsNormal(std::uint32_t classCode) noexcept
{
    const std::uint32_t base = classCode >> 16U;
    return base == 0x05 || base == 0x06 || base == 0x08;
}

/// Every PCI device under @p root, by slot, named from pci.ids when it is installed.
[[nodiscard]] inline std::vector<Device> readPci(const std::filesystem::path& root, bool& ok)
{
    const std::filesystem::path dir = root / "sys/bus/pci/devices";
    std::vector<Device> devices;
    std::vector<std::uint32_t> classCodes;
    for (const std::string& entry : sortedEntries(dir, ok))
    {
        const std::filesystem::path path = dir / entry;
        const std::string uevent = LinuxOsInfo::readFile(path / "uevent");
        Device device;
        device.vendorId = static_cast<std::uint16_t>(hexValue(LinuxOsInfo::readLine(path / "vendor")));
        device.productId = static_cast<std::uint16_t>(hexValue(LinuxOsInfo::readLine(path / "device")));
        device.driver = LinuxGraphics::ueventValue(uevent, "DRIVER");
        device.location = LinuxGraphics::ueventValue(uevent, "PCI_SLOT_NAME");
        device.location = device.location.empty() ? entry : device.location;
        classCodes.push_back(hexValue(LinuxOsInfo::readLine(path / "class")));
        devices.push_back(std::move(device));
    }
    const HwIds::IdNames names = nameDevices(devices, readIdsFile(root, "pci.ids"));
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        const auto base = static_cast<std::uint8_t>(classCodes[i] >> 16U);
        const auto sub = static_cast<std::uint8_t>(classCodes[i] >> 8U);
        devices[i].className = names.className(base, sub);
        if (devices[i].className.empty())
        {
            devices[i].className = std::format("Class {:02X}{:02X}", base, sub);
        }
        if (devices[i].driver.empty() && !driverlessIsNormal(classCodes[i]))
        {
            devices[i].problem = "No driver";
        }
    }
    return devices;
}

/// Every USB device under @p root (interfaces and root hubs left out), parents first, named from its
/// own strings, else usb.ids.
[[nodiscard]] inline std::vector<Device> readUsb(const std::filesystem::path& root, bool& ok)
{
    const std::filesystem::path dir = root / "sys/bus/usb/devices";
    std::vector<Device> devices;
    for (const std::string& entry : sortedEntries(dir, ok)) // "1-1" sorts before "1-1.2"
    {
        if (entry.contains(':') || entry.starts_with("usb"))
        {
            continue;
        }
        const std::filesystem::path path = dir / entry;
        Device device;
        device.location = entry;
        device.vendorId = static_cast<std::uint16_t>(hexValue(LinuxOsInfo::readLine(path / "idVendor")));
        device.productId = static_cast<std::uint16_t>(hexValue(LinuxOsInfo::readLine(path / "idProduct")));
        device.vendor = LinuxStorage::trimmed(LinuxOsInfo::readLine(path / "manufacturer"));
        device.name = LinuxStorage::trimmed(LinuxOsInfo::readLine(path / "product"));
        device.serial = LinuxStorage::trimmed(LinuxOsInfo::readLine(path / "serial"));
        device.speedMbps = usbSpeedMbps(LinuxOsInfo::readLine(path / "speed"));
        const std::string_view ports = std::string_view(entry).substr(entry.find('-') + 1);
        device.depth = static_cast<std::uint32_t>(std::ranges::count(ports, '.')) + 1;
        if (const std::size_t dot = entry.rfind('.'); dot != std::string::npos)
        {
            const std::string_view hub = std::string_view(entry).substr(0, dot);
            const auto it = std::ranges::find(devices, hub, &Device::location);
            device.parent = it != devices.end() ? std::optional(static_cast<std::size_t>(it - devices.begin())) : std::nullopt;
        }
        devices.push_back(std::move(device));
    }
    static_cast<void>(nameDevices(devices, readIdsFile(root, "usb.ids")));
    return devices;
}

/// /proc/asound/cards' card names by index: " 0 [PCH ]: HDA-Intel - HDA Intel PCH" gives 0, "HDA Intel PCH".
[[nodiscard]] inline std::map<int, std::string> parseAsoundCards(std::string_view text)
{
    std::map<int, std::string> cards;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        end = end == std::string_view::npos ? text.size() : end;
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
        int index = -1;
        const auto [digitsEnd, ec] = std::from_chars(line.data(), line.data() + line.size(), index);
        const std::size_t colon = line.find("]: ");
        if (ec != std::errc{} || digitsEnd == line.data() || colon == std::string_view::npos || index < 0)
        {
            continue; // a card's second (description) line, or "--- no soundcards ---"
        }
        std::string_view name = line.substr(colon + 3);
        if (const std::size_t dash = name.find(" - "); dash != std::string_view::npos)
        {
            name = name.substr(dash + 3);
        }
        cards[index] = LinuxStorage::trimmed(name);
    }
    return cards;
}

/// /proc/asound/pcm's endpoints: "00-00: ALC892 Analog : ALC892 Analog : playback 1 : capture 1" is an
/// output and an input named "<card>: ALC892 Analog".
[[nodiscard]] inline std::vector<AudioEndpoint> parseAsoundPcm(std::string_view text, const std::map<int, std::string>& cards)
{
    std::vector<AudioEndpoint> endpoints;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        end = end == std::string_view::npos ? text.size() : end;
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        int card = -1;
        const auto [digitsEnd, ec] = std::from_chars(line.data(), line.data() + line.size(), card);
        const std::size_t colon = line.find(": ");
        if (ec != std::errc{} || colon == std::string_view::npos || card < 0)
        {
            continue;
        }
        const std::size_t nameEnd = line.find(" : ", colon);
        const std::string pcm =
            LinuxStorage::trimmed(line.substr(colon + 2, nameEnd == std::string_view::npos ? std::string_view::npos : nameEnd - colon - 2));
        const auto it = cards.find(card);
        const std::string name = std::format("{}: {}", it != cards.end() ? it->second : std::format("Card {}", card), pcm);
        const std::string_view flows = nameEnd == std::string_view::npos ? std::string_view{} : line.substr(nameEnd);
        if (flows.contains(": playback"))
        {
            endpoints.push_back({.name = name, .flow = AudioFlow::Output});
        }
        if (flows.contains(": capture"))
        {
            endpoints.push_back({.name = name, .flow = AudioFlow::Input});
        }
    }
    return endpoints;
}

/// Every Devices fact under @p root.
inline void readDeviceFacts(const std::filesystem::path& root, DevicesInfo& info)
{
    info.available = true;
    info.family = OsFamily::Linux;
    info.pci = readPci(root, info.pciRead);
    info.problemsRead = info.pciRead;
    for (const Device& device : info.pci)
    {
        if (!device.problem.empty())
        {
            info.problems.push_back(device);
        }
    }
    info.usb = readUsb(root, info.usbRead);

    const std::string cardsText = LinuxOsInfo::readFile(root / "proc/asound/cards");
    info.audioRead = !cardsText.empty();
    const std::map<int, std::string> cards = parseAsoundCards(cardsText);
    info.audio = parseAsoundPcm(LinuxOsInfo::readFile(root / "proc/asound/pcm"), cards);
    if (info.audio.empty())
    {
        for (const auto& [index, name] : cards) // no PCM list: the cards, direction unknown
        {
            info.audio.push_back({.name = name, .flow = AudioFlow::Unknown});
        }
    }
}

} // namespace Platform::LinuxDevices
