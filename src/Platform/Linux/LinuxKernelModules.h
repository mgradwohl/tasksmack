#pragma once

// The Linux Drivers facts (#1521): the loaded kernel modules, read under an injected root ("/" in the app,
// a fixture tree in tests), unprivileged and enumeration only:
// - /proc/modules, one line per module: "name size refcount deps state address [(taints)]", where deps is
//   "-" or a comma-terminated list of the modules using this one, with "[permanent]," for a module that
//   can't be unloaded; a kernel without module unloading prints "- -" for refcount and deps.
// - /sys/module/<name>/version where the module declares one (MODULE_VERSION).
// Modules built into the kernel aren't in /proc/modules, so they aren't listed.
// Standard library only, so the fixture tests and the fuzzer (fuzz_proc_modules) run on every platform.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxOsInfo.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::LinuxKernelModules
{

/// The marker /proc/modules puts in the deps field of a module that can't be unloaded.
inline constexpr std::string_view PERMANENT = "[permanent]";

/// One /proc/modules line as a module; nullopt for a line without a name, size, refcount, deps and state.
[[nodiscard]] inline std::optional<KernelDriver> parseModuleLine(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::string_view rest = line;
    while (!rest.empty())
    {
        const std::size_t start = rest.find_first_not_of(" \t\r");
        if (start == std::string_view::npos)
        {
            break;
        }
        rest.remove_prefix(start);
        const std::size_t length = std::min(rest.find_first_of(" \t\r"), rest.size());
        fields.push_back(rest.substr(0, length));
        rest.remove_prefix(length);
    }
    constexpr std::size_t REQUIRED = 5; // name size refcount deps state
    if (fields.size() < REQUIRED)
    {
        return std::nullopt;
    }
    KernelDriver module;
    module.name = std::string(fields[0]);
    const std::string_view size = fields[1];
    if (const auto [end, ec] = std::from_chars(size.data(), size.data() + size.size(), module.sizeBytes);
        ec != std::errc{} || end != size.data() + size.size())
    {
        return std::nullopt;
    }
    const std::string_view count = fields[2];
    if (std::uint32_t refs = 0; std::from_chars(count.data(), count.data() + count.size(), refs).ec == std::errc{})
    {
        module.useCount = refs;
    }
    std::string_view deps = fields[3];
    while (!deps.empty() && deps != "-")
    {
        const std::size_t comma = std::min(deps.find(','), deps.size());
        const std::string_view dep = deps.substr(0, comma);
        if (dep == PERMANENT)
        {
            module.permanent = true;
        }
        else if (!dep.empty())
        {
            module.usedBy.emplace_back(dep);
        }
        deps.remove_prefix(std::min(comma + 1, deps.size()));
    }
    module.moduleState = std::string(fields[4]);
    // fields[5] is the load address (0x0 without CAP_SYSLOG); fields[6], when present, the taints: "(POE)",
    // with a trailing "+" or "-" while loading or unloading.
    constexpr std::size_t TAINTS = 6;
    if (fields.size() > TAINTS && fields[TAINTS].starts_with('('))
    {
        for (const char flag : fields[TAINTS])
        {
            if (flag != '(' && flag != ')' && flag != '+' && flag != '-')
            {
                module.taints += flag;
            }
        }
    }
    return module;
}

/// Every module in a /proc/modules text, in its order (the most recently loaded first).
[[nodiscard]] inline std::vector<KernelDriver> parseProcModules(std::string_view text)
{
    std::vector<KernelDriver> modules;
    while (!text.empty())
    {
        const std::size_t newline = std::min(text.find('\n'), text.size());
        if (std::optional<KernelDriver> module = parseModuleLine(text.substr(0, newline)); module.has_value())
        {
            modules.push_back(std::move(*module));
        }
        text.remove_prefix(std::min(newline + 1, text.size()));
    }
    return modules;
}

/// The loaded modules under @p root, with each one's /sys/module version where it has one.
inline void readKernelModules(const std::filesystem::path& root, DriversInfo& info)
{
    info.available = true;
    info.family = OsFamily::Linux;
    const std::filesystem::path procModules = root / "proc/modules";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(procModules, ec)) // a kernel built without module support
    {
        return;
    }
    info.listed = true;
    info.drivers = parseProcModules(LinuxOsInfo::readFile(procModules));
    for (KernelDriver& module : info.drivers)
    {
        if (!module.name.contains('/') && module.name != "." && module.name != "..")
        {
            module.version = LinuxOsInfo::readLine(root / "sys/module" / module.name / "version");
        }
    }
}

} // namespace Platform::LinuxKernelModules
