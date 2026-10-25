#pragma once

// The hwdata pci.ids / usb.ids format (#1520), the names the Devices section shows for Linux's PCI and USB
// ids. A vendor line is four hex digits, two spaces and the name; its devices follow on lines indented by
// one tab (subsystems, two tabs, aren't read). pci.ids' "C xx  Name" class lines and their one-tab
// subclasses are read too; usb.ids' own top-level sections ("C", "AT", "HID", ...) are skipped. Only the
// vendors and devices asked for are kept, so a 1.4 MB file costs one pass and a few strings. Pure and
// standard library only: tests/fuzz/fuzz_hwids.cpp fuzzes it.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::HwIds
{

/// The key a device is asked for and found by.
[[nodiscard]] constexpr std::uint32_t deviceKey(std::uint16_t vendorId, std::uint16_t deviceId) noexcept
{
    return (static_cast<std::uint32_t>(vendorId) << 16U) | deviceId;
}

/// The names found.
struct IdNames
{
    std::map<std::uint16_t, std::string> vendors;
    std::map<std::uint32_t, std::string> devices;    ///< By deviceKey()
    std::map<std::uint8_t, std::string> classes;     ///< pci.ids' base classes
    std::map<std::uint16_t, std::string> subclasses; ///< pci.ids', by (class << 8) | subclass

    /// The subclass's name, else the base class's; empty when neither is known.
    [[nodiscard]] std::string className(std::uint8_t baseClass, std::uint8_t subclass) const
    {
        if (const auto it = subclasses.find(static_cast<std::uint16_t>((baseClass << 8U) | subclass)); it != subclasses.end())
        {
            return it->second;
        }
        const auto it = classes.find(baseClass);
        return it != classes.end() ? it->second : std::string{};
    }
};

/// The value of @p digits hex digits at the start of @p line followed by two spaces, and the name after
/// them; false when the line isn't one.
template<typename T> [[nodiscard]] bool parseIdLine(std::string_view line, std::size_t digits, T& id, std::string_view& name)
{
    if (line.size() < digits + 3 || line[digits] != ' ' || line[digits + 1] != ' ')
    {
        return false;
    }
    const std::string_view hex = line.substr(0, digits);
    const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
    if (ec != std::errc{} || end != hex.data() + hex.size())
    {
        return false;
    }
    name = line.substr(digits + 2);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
    {
        name.remove_suffix(1);
    }
    return !name.empty();
}

/// Parses pci.ids or usb.ids @p text, keeping the vendors in @p vendors, the devices in @p devices (by
/// deviceKey()) and every PCI class. Lines it doesn't recognise are skipped.
[[nodiscard]] inline IdNames parseIds(std::string_view text, const std::set<std::uint16_t>& vendors, const std::set<std::uint32_t>& devices)
{
    enum class Block : std::uint8_t
    {
        None,
        Vendor,
        Class,
    };
    IdNames names;
    Block block = Block::None;
    std::uint16_t vendor = 0;
    std::uint8_t baseClass = 0;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        end = end == std::string_view::npos ? text.size() : end;
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (line.ends_with('\r'))
        {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        std::string_view name;
        if (line.front() != '\t')
        {
            block = Block::None;
            if (parseIdLine(line, 4, vendor, name))
            {
                block = Block::Vendor;
                if (vendors.contains(vendor))
                {
                    names.vendors.emplace(vendor, name);
                }
            }
            else if (line.starts_with("C ") && parseIdLine(line.substr(2), 2, baseClass, name))
            {
                block = Block::Class;
                names.classes.emplace(baseClass, name);
            }
            continue;
        }
        if (line.size() < 2 || line[1] == '\t')
        {
            continue; // a subsystem or programming interface
        }
        line.remove_prefix(1);
        if (std::uint16_t device = 0; block == Block::Vendor && parseIdLine(line, 4, device, name))
        {
            if (devices.contains(deviceKey(vendor, device)))
            {
                names.devices.emplace(deviceKey(vendor, device), name);
            }
        }
        else if (std::uint8_t subclass = 0; block == Block::Class && parseIdLine(line, 2, subclass, name))
        {
            names.subclasses.emplace(static_cast<std::uint16_t>((baseClass << 8U) | subclass), name);
        }
    }
    return names;
}

} // namespace Platform::HwIds
