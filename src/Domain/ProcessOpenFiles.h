#pragma once

// Display text for Process Details' Open files section (#183): Platform reads the files raw
// (Platform/IProcessOpenFiles.h); these turn a row into its descriptor, type and mode text, once per
// read, never per frame.

#include "Platform/IProcessOpenFiles.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace Domain::OpenFiles
{

/// Linux's open-flag bits the mode shows (the kernel's values, the same on every architecture).
inline constexpr std::uint32_t ACCESS_MODE_MASK = 03;
inline constexpr std::uint32_t WRITE_ONLY = 01;
inline constexpr std::uint32_t READ_WRITE = 02;
inline constexpr std::uint32_t APPEND = 02000;

/// "File", "Socket", ...
[[nodiscard]] constexpr std::string_view kindLabel(Platform::OpenFileKind kind) noexcept
{
    switch (kind)
    {
    case Platform::OpenFileKind::File:
        return "File";
    case Platform::OpenFileKind::Directory:
        return "Directory";
    case Platform::OpenFileKind::Device:
        return "Device";
    case Platform::OpenFileKind::Socket:
        return "Socket";
    case Platform::OpenFileKind::Pipe:
        return "Pipe";
    case Platform::OpenFileKind::AnonInode:
        return "Anon inode";
    case Platform::OpenFileKind::Memfd:
        return "memfd";
    case Platform::OpenFileKind::Other:
        break;
    }
    return "Other";
}

/// A descriptor as its number ("3"), or a Windows handle value in hex ("0x1A4").
[[nodiscard]] inline std::string formatDescriptor(std::uint64_t descriptor, bool hex)
{
    return hex ? std::format("0x{:X}", descriptor) : std::to_string(descriptor);
}

/// The access mode in Linux open @p flags: "r", "w" or "rw", with " append" for O_APPEND; empty when
/// the flags were not read.
[[nodiscard]] inline std::string formatMode(std::optional<std::uint32_t> flags)
{
    if (!flags.has_value())
    {
        return {};
    }
    const std::uint32_t access = *flags & ACCESS_MODE_MASK;
    std::string mode = "r";
    if (access == WRITE_ONLY)
    {
        mode = "w";
    }
    else if (access == READ_WRITE)
    {
        mode = "rw";
    }
    if ((*flags & APPEND) != 0)
    {
        mode += " append";
    }
    return mode;
}

} // namespace Domain::OpenFiles
