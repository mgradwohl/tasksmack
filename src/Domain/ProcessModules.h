#pragma once

// Display text for Process Details' Modules section (#802): Platform reads the modules raw
// (Platform/IProcessModules.h); these turn a row into its name, hex base address and version string,
// once per read, never per frame.

#include "Platform/IProcessModules.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace Domain::Modules
{

/// The file name in @p path, after its last '/' or '\' ("ntdll.dll", "libc.so.6"); all of it when it
/// has neither.
[[nodiscard]] constexpr std::string_view fileName(std::string_view path) noexcept
{
    if (const std::size_t slash = path.find_last_of("/\\"); slash != std::string_view::npos)
    {
        path.remove_prefix(slash + 1);
    }
    return path;
}

/// @p address as hex, at least 12 digits so a table's addresses line up ("0x7FF8A1B20000").
[[nodiscard]] inline std::string formatAddress(std::uint64_t address)
{
    return std::format("0x{:012X}", address);
}

/// "10.0.26100.4202", or empty when the module has no version.
[[nodiscard]] inline std::string formatVersion(const std::optional<Platform::ModuleVersion>& version)
{
    if (!version.has_value())
    {
        return {};
    }
    return std::format("{}.{}.{}.{}", version->major, version->minor, version->build, version->revision);
}

} // namespace Domain::Modules
