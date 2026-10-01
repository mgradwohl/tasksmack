#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string_view>

namespace Platform::CgroupPath
{

/// True when `candidate` lies strictly inside `base`, compared component-by-component on the
/// lexically normalised forms. Per-component rather than per-character: a string-prefix test is
/// separator-sensitive and would also accept a sibling such as "<base>-evil/x", whose text begins
/// with the base's. Strictly inside: equal to `base` is not contained, as there is no file there.
[[nodiscard]] inline bool isContainedIn(const std::filesystem::path& base, const std::filesystem::path& candidate)
{
    const std::filesystem::path normalisedBase = base.lexically_normal();
    const std::filesystem::path normalisedCandidate = candidate.lexically_normal();
    const auto [baseEnd, candidateEnd] = std::ranges::mismatch(normalisedBase, normalisedCandidate);
    return baseEnd == normalisedBase.end() && candidateEnd != normalisedCandidate.end();
}

/// Builds `base / relative / leaf`, returning nullopt unless the result provably stays inside
/// `base`.
///
/// `relative` originates from /proc/<pid>/cgroup, i.e. it is read out of a file and is therefore
/// untrusted input to a file-access function (CodeQL cpp/path-injection, alert #2948). A ".."
/// component would escape the cgroup hierarchy, and since the caller only tests the first bytes of
/// the file for a "FROZEN"/"FREEZING" prefix, an escape turns that check into a content oracle for
/// arbitrary readable files.
///
/// Two mechanisms, with distinct jobs. Rejecting ".." components is the policy: a cgroup path from
/// the kernel never contains traversal, so anything that does is refused rather than normalised.
/// isContainedIn() is then the backstop asserting the policy actually held. No input the policy
/// admits can reach that rejection today -- it exists so relaxing the policy later cannot silently
/// permit an escape, and it is exposed and tested separately for exactly that reason, since a
/// backstop no test can reach is indistinguishable from dead code.
///
/// Deliberately free of platform APIs so it is unit-testable on any host, and deliberately lexical
/// rather than filesystem-resolving: this must not follow symlinks or touch the filesystem, and it
/// must behave identically whether or not the path exists.
[[nodiscard]] inline std::optional<std::filesystem::path>
buildContainedCgroupPath(const std::filesystem::path& base, std::string_view relative, std::string_view leaf)
{
    if (relative.empty() || leaf.empty())
    {
        return std::nullopt;
    }

    const std::filesystem::path relativePath(relative);

    // Must be genuinely relative. Re-rooting ("/etc") or a drive-relative form would otherwise
    // replace the base entirely when appended.
    if (relativePath.has_root_directory() || relativePath.has_root_name())
    {
        return std::nullopt;
    }

    for (const auto& component : relativePath)
    {
        if (component == "..")
        {
            return std::nullopt;
        }
    }

    std::filesystem::path candidate = (base / relativePath / leaf).lexically_normal();
    if (!isContainedIn(base, candidate))
    {
        return std::nullopt;
    }

    return candidate;
}

} // namespace Platform::CgroupPath
