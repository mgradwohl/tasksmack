#pragma once

// Small Win32 value conversions the Windows probes share (#2563): a FILETIME as 100 ns ticks, those
// ticks as Unix seconds, and a GUID in its registry text form. No Windows header: the helpers take
// any struct with FILETIME's or GUID's fields, so the tests run on every platform (#1133).

#include <concepts>
#include <cstdint>
#include <format>
#include <string>

namespace Platform::Windows
{

/// A struct laid out like FILETIME: the low and high 32 bits of a 100 ns tick count since 1601.
template<typename T>
concept FileTimeFields = requires(const T& time) {
    { time.dwLowDateTime } -> std::convertible_to<std::uint32_t>;
    { time.dwHighDateTime } -> std::convertible_to<std::uint32_t>;
};

/// A struct laid out like GUID: Data1 (32 bits), Data2 and Data3 (16 bits) and Data4 (8 bytes).
template<typename T>
concept GuidFields = requires(const T& guid) {
    { guid.Data1 } -> std::convertible_to<std::uint32_t>;
    { guid.Data2 } -> std::convertible_to<std::uint16_t>;
    { guid.Data3 } -> std::convertible_to<std::uint16_t>;
    { guid.Data4[7] } -> std::convertible_to<std::uint8_t>;
};

/// 100 ns FILETIME ticks between 1601-01-01 and the Unix epoch.
inline constexpr std::uint64_t FILETIME_UNIX_EPOCH = 116444736000000000ULL;
inline constexpr std::uint64_t FILETIME_TICKS_PER_SECOND = 10000000ULL;

/// A FILETIME as one 64-bit count of 100 ns ticks.
template<FileTimeFields FileTime> [[nodiscard]] constexpr std::uint64_t fileTimeTicks(const FileTime& time) noexcept
{
    constexpr unsigned HIGH_SHIFT = 32;
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << HIGH_SHIFT) | static_cast<std::uint64_t>(time.dwLowDateTime);
}

/// A FILETIME (100 ns ticks since 1601) as Unix seconds; 0 for one at or before the Unix epoch.
[[nodiscard]] constexpr std::uint64_t fileTimeToUnixSeconds(std::uint64_t fileTime) noexcept
{
    return fileTime > FILETIME_UNIX_EPOCH ? (fileTime - FILETIME_UNIX_EPOCH) / FILETIME_TICKS_PER_SECOND : 0;
}

/// A GUID in its registry form, upper-case hex in braces: "{4D36E972-E325-11CE-BFC1-08002BE10318}".
template<GuidFields Guid> [[nodiscard]] std::string guidText(const Guid& guid)
{
    return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
                       static_cast<std::uint32_t>(guid.Data1),
                       static_cast<std::uint16_t>(guid.Data2),
                       static_cast<std::uint16_t>(guid.Data3),
                       static_cast<std::uint8_t>(guid.Data4[0]),
                       static_cast<std::uint8_t>(guid.Data4[1]),
                       static_cast<std::uint8_t>(guid.Data4[2]),
                       static_cast<std::uint8_t>(guid.Data4[3]),
                       static_cast<std::uint8_t>(guid.Data4[4]),
                       static_cast<std::uint8_t>(guid.Data4[5]),
                       static_cast<std::uint8_t>(guid.Data4[6]),
                       static_cast<std::uint8_t>(guid.Data4[7]));
}

} // namespace Platform::Windows
