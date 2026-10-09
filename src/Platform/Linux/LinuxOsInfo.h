#pragma once

// The Linux Operating system section's file-based facts (#1512), read under an injected root ("/"
// in the app, a fixture tree in tests): /etc/os-release, /proc/1/comm, btime from /proc/stat, the
// /etc/localtime link, and container/VM hints (/.dockerenv, /run/.containerenv, the DMI vendor).
// Only standard-library file access, so the parsing and the fixture tests build and run everywhere.

#include "Platform/ISystemInfoProbe.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Platform::LinuxOsInfo
{

/// The os-release fields the section shows.
struct OsRelease
{
    std::string prettyName; ///< PRETTY_NAME, else NAME
    std::string versionId;  ///< VERSION_ID
};

/// An os-release value without its quotes and with its backslash escapes undone (os-release(5)).
[[nodiscard]] inline std::string unquoteOsReleaseValue(std::string_view value)
{
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
    {
        value = value.substr(1, value.size() - 2);
    }
    std::string out;
    out.reserve(value.size());
    // A backslash escapes the character after it; a trailing one is kept as written.
    std::size_t i = 0;
    while (i < value.size())
    {
        const bool escape = value[i] == '\\' && i + 1 < value.size();
        const std::size_t next = escape ? i + 1 : i;
        out.push_back(value[next]);
        i = next + 1;
    }
    return out;
}

/// PRETTY_NAME (else NAME) and VERSION_ID from os-release text; comments and blank lines skipped.
[[nodiscard]] inline OsRelease parseOsRelease(std::string_view text)
{
    OsRelease release;
    std::string name;
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        const std::size_t equals = line.find('=');
        if (line.starts_with('#') || equals == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = line.substr(0, equals);
        const std::string value = unquoteOsReleaseValue(line.substr(equals + 1));
        if (key == "PRETTY_NAME")
        {
            release.prettyName = value;
        }
        else if (key == "NAME")
        {
            name = value;
        }
        else if (key == "VERSION_ID")
        {
            release.versionId = value;
        }
    }
    if (release.prettyName.empty())
    {
        release.prettyName = std::move(name);
    }
    return release;
}

/// The zone name from where /etc/localtime points ("../usr/share/zoneinfo/Europe/Berlin" ->
/// "Europe/Berlin"); empty when the target isn't under a zoneinfo directory.
[[nodiscard]] inline std::string timeZoneFromLocaltimeTarget(std::string_view target)
{
    constexpr std::string_view ZONEINFO = "zoneinfo/";
    const std::size_t at = target.rfind(ZONEINFO);
    if (at == std::string_view::npos)
    {
        return {};
    }
    std::string_view zone = target.substr(at + ZONEINFO.size());
    // zoneinfo/posix/... and zoneinfo/right/... hold the same zones with other leap-second rules.
    for (const std::string_view prefix : {std::string_view{"posix/"}, std::string_view{"right/"}})
    {
        if (zone.starts_with(prefix))
        {
            zone.remove_prefix(prefix.size());
        }
    }
    return std::string(zone);
}

/// The session's locale as setlocale(LC_ALL, "") would pick it for messages: LC_ALL, else LANG,
/// else "C". Null or empty variables are skipped.
[[nodiscard]] inline std::string chooseLocale(const char* lcAll, const char* lang)
{
    for (const char* value : {lcAll, lang})
    {
        if (value != nullptr && *value != '\0')
        {
            return value;
        }
    }
    return "C";
}

/// btime (boot time, Unix seconds) from /proc/stat text; 0 when absent.
[[nodiscard]] inline std::uint64_t parseBootTime(std::string_view procStat) noexcept
{
    constexpr std::string_view KEY = "\nbtime ";
    const std::size_t at = procStat.find(KEY);
    if (at == std::string_view::npos)
    {
        return 0;
    }
    const char* first = procStat.data() + at + KEY.size();
    std::uint64_t value = 0;
    static_cast<void>(std::from_chars(first, procStat.data() + procStat.size(), value)); // 0 when unparsable
    return value;
}

/// The whole of a small file, or empty when it can't be read.
[[nodiscard]] inline std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::string{};
}

/// A one-line file without its trailing newline and spaces.
[[nodiscard]] inline std::string readLine(const std::filesystem::path& path)
{
    std::string text = readFile(path);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    {
        text.pop_back();
    }
    return text;
}

/// A container or VM hint, all unprivileged: Podman's /run/.containerenv, Docker's /.dockerenv, then
/// the DMI system vendor and product names known hypervisors set. "None detected" otherwise.
[[nodiscard]] inline std::string virtualizationHint(const std::filesystem::path& root)
{
    std::error_code ec;
    if (std::filesystem::exists(root / "run/.containerenv", ec))
    {
        return "Container (Podman)";
    }
    if (std::filesystem::exists(root / ".dockerenv", ec))
    {
        return "Container (Docker)";
    }
    const std::string dmi = readLine(root / "sys/class/dmi/id/sys_vendor") + " " + readLine(root / "sys/class/dmi/id/product_name");
    constexpr std::array<std::pair<std::string_view, std::string_view>, 8> VENDORS{{
        {"QEMU", "Virtual machine (QEMU)"},
        {"KVM", "Virtual machine (KVM)"},
        {"VMware", "Virtual machine (VMware)"},
        {"VirtualBox", "Virtual machine (VirtualBox)"},
        {"Virtual Machine", "Virtual machine (Hyper-V)"}, // Microsoft Corporation's product name
        {"Xen", "Virtual machine (Xen)"},
        {"Parallels", "Virtual machine (Parallels)"},
        {"Amazon EC2", "Virtual machine (Amazon EC2)"},
    }};
    for (const auto& [needle, label] : VENDORS)
    {
        if (dmi.contains(needle))
        {
            return std::string(label);
        }
    }
    return "None detected";
}

/// The file-based facts under @p root into @p info: distro, init, boot time, time zone, virtualization.
inline void readFileFacts(const std::filesystem::path& root, OsInfo& info)
{
    std::string osRelease = readFile(root / "etc/os-release");
    if (osRelease.empty())
    {
        osRelease = readFile(root / "usr/lib/os-release"); // os-release(5)'s fallback
    }
    const OsRelease release = parseOsRelease(osRelease);
    info.name = release.prettyName;
    info.version = release.versionId;
    info.initSystem = readLine(root / "proc/1/comm");
    info.bootUnixSeconds = parseBootTime(readFile(root / "proc/stat"));

    std::error_code ec;
    const std::filesystem::path target = std::filesystem::read_symlink(root / "etc/localtime", ec);
    info.timeZone = ec ? std::string{} : timeZoneFromLocaltimeTarget(target.generic_string());
    if (info.timeZone.empty())
    {
        info.timeZone = readLine(root / "etc/timezone"); // Debian's copy when localtime isn't a link
    }
    info.virtualization = virtualizationHint(root);
}

} // namespace Platform::LinuxOsInfo
