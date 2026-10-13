#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace Platform::CgroupContainer
{

/// Characters of a container id shown: the 12 `docker ps` and `podman ps` show.
inline constexpr std::size_t SHORT_ID_LENGTH = 12;

namespace Detail
{

[[nodiscard]] constexpr bool isHexId(std::string_view text) noexcept
{
    return text.size() >= SHORT_ID_LENGTH &&
           std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/// "<runtime>:<first 12 of id>", or empty when `id` is not a hex container id.
[[nodiscard]] inline std::string hexLabel(std::string_view runtime, std::string_view id)
{
    if (!isHexId(id))
    {
        return {};
    }
    std::string label(runtime);
    label += ':';
    label += id.substr(0, SHORT_ID_LENGTH);
    return label;
}

/// "<runtime>:<name>", or empty when `name` is.
[[nodiscard]] inline std::string nameLabel(std::string_view runtime, std::string_view name)
{
    if (name.empty())
    {
        return {};
    }
    std::string label(runtime);
    label += ':';
    label += name;
    return label;
}

/// `text` between `prefix` and `suffix`, or empty unless it has both.
[[nodiscard]] constexpr std::string_view between(std::string_view text, std::string_view prefix, std::string_view suffix) noexcept
{
    if (text.size() < prefix.size() + suffix.size() || !text.starts_with(prefix) || !text.ends_with(suffix))
    {
        return {};
    }
    text.remove_prefix(prefix.size()); // remove_*, not substr(): those don't throw
    text.remove_suffix(suffix.size());
    return text;
}

/// The container one cgroup path component names, given the component before it (empty for the
/// first): systemd-driver scopes ("docker-<id>.scope", "libpod-<id>.scope",
/// "cri-containerd-<id>.scope", "crio-<id>.scope"), cgroupfs-driver directories ("docker/<id>",
/// "libpod-<id>", "kubepods/.../<id>", "lxc/<name>"), LXC 4+'s "lxc.payload.<name>", and
/// systemd-machined's guests ("machine-<name>.scope", "systemd-nspawn@<name>.service"). Empty when
/// it names none. The runtime monitors ("libpod-conmon-<id>.scope", "crio-conmon-<id>.scope") run
/// outside the container and are not one.
[[nodiscard]] inline std::string componentLabel(std::string_view parent, std::string_view component)
{
    if (component.starts_with("libpod-conmon-") || component.starts_with("crio-conmon-"))
    {
        return {};
    }
    struct HexScope
    {
        std::string_view prefix;
        std::string_view runtime;
    };
    // "cri-containerd-" before any shorter prefix it could be mistaken for.
    static constexpr std::array HEX_SCOPES{
        HexScope{.prefix = "cri-containerd-", .runtime = "containerd"},
        HexScope{.prefix = "docker-", .runtime = "docker"},
        HexScope{.prefix = "libpod-", .runtime = "podman"},
        HexScope{.prefix = "crio-", .runtime = "cri-o"},
    };
    for (const HexScope& scope : HEX_SCOPES)
    {
        if (std::string label = hexLabel(scope.runtime, between(component, scope.prefix, ".scope")); !label.empty())
        {
            return label;
        }
    }
    // Podman's cgroupfs driver: "libpod_parent/libpod-<id>", no ".scope".
    if (std::string label = hexLabel("podman", between(component, "libpod-", "")); !label.empty())
    {
        return label;
    }
    if (parent == "docker")
    {
        return hexLabel("docker", component);
    }
    if (parent == "lxc")
    {
        return nameLabel("lxc", component);
    }
    if (component.starts_with("lxc.payload."))
    {
        return nameLabel("lxc", component.substr(std::string_view("lxc.payload.").size()));
    }
    if (std::string label = nameLabel("nspawn", between(component, "systemd-nspawn@", ".service")); !label.empty())
    {
        return label;
    }
    if (parent == "machine.slice")
    {
        return nameLabel("machine", between(component, "machine-", ".scope"));
    }
    // Kubernetes' cgroupfs driver: "kubepods/<qos>/pod<uid>/<id>", the runtime unnamed.
    if (parent.starts_with("pod"))
    {
        return hexLabel("k8s", component);
    }
    return {};
}

/// The container a cgroup path is in: the innermost component naming one, so a container nested in
/// another is named for itself. Empty when none does.
[[nodiscard]] inline std::string pathLabel(std::string_view path)
{
    std::string innermost;
    std::string_view parent;
    std::size_t start = 0;
    while (start <= path.size())
    {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos)
        {
            end = path.size();
        }
        const std::string_view component = path.substr(start, end - start);
        if (!component.empty())
        {
            if (std::string label = componentLabel(parent, component); !label.empty())
            {
                innermost = std::move(label);
            }
            parent = component;
        }
        start = end + 1;
    }
    return innermost;
}

} // namespace Detail

/// The container a process is in, from its /proc/[pid]/cgroup contents: "<runtime>:<id>" with the
/// id's first 12 hex characters ("docker:1a2b3c4d5e6f", "podman:…", "containerd:…", "cri-o:…",
/// "k8s:…") or "<runtime>:<name>" ("lxc:web", "nspawn:debian", "machine:fedora"). The cgroup v2
/// ("0::") path is read first, then the first v1 line naming a container. Empty for a
/// process in no container -- including one inside a container whose cgroup namespace hides the
/// path above it, where every process reads "0::/".
[[nodiscard]] inline std::string containerFromCgroups(std::string_view cgroups)
{
    std::string fromV1;
    std::size_t lineStart = 0;
    while (lineStart < cgroups.size())
    {
        std::size_t lineEnd = cgroups.find('\n', lineStart);
        if (lineEnd == std::string_view::npos)
        {
            lineEnd = cgroups.size();
        }
        const std::string_view line = cgroups.substr(lineStart, lineEnd - lineStart);
        if (line.starts_with("0::"))
        {
            // A hybrid hierarchy's v2 path can be systemd's alone, the container's in the v1 lines.
            if (std::string label = Detail::pathLabel(line.substr(3)); !label.empty())
            {
                return label;
            }
        }
        // v1: "hierarchy:controllers:path"; the path is after the second ':'.
        else if (const std::size_t first = line.find(':'); fromV1.empty() && first != std::string_view::npos)
        {
            if (const std::size_t second = line.find(':', first + 1); second != std::string_view::npos)
            {
                fromV1 = Detail::pathLabel(line.substr(second + 1));
            }
        }
        lineStart = lineEnd + 1;
    }
    return fromV1;
}

} // namespace Platform::CgroupContainer
