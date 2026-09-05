#pragma once

// Pure(ish) logic extracted from PathService.cpp's toAbsolute(), so it can be unit-tested
// directly instead of only indirectly through PathService's constructor. See CONTRIBUTING.md's
// "extract the pure decision logic into a small header" pattern (also used by Core/FramePacing.h,
// Core/ResizePerfTrace.h). The fallback branches below require std::filesystem::absolute() and/or
// current_path() to fail, which essentially never happens against a real OS - taking those two
// primitives as injectable function parameters lets tests fabricate that failure directly instead
// of needing a genuinely broken filesystem.

#include <spdlog/spdlog.h>

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace Core
{

using AbsolutePathFn = std::function<std::filesystem::path(const std::filesystem::path&, std::error_code&)>;
using CurrentPathFn = std::function<std::filesystem::path(std::error_code&)>;

/// Normalize a raw path from a platform provider to an absolute, cleaned path, using the
/// supplied @p absoluteFn / @p currentPathFn instead of calling std::filesystem::absolute() /
/// current_path() directly.
///
/// Fallback order when @p absoluteFn fails:
///   1. If raw is relative, try (currentPathFn() / raw).lexically_normal().
///   2. Return raw.lexically_normal() as a last resort.
///
/// NOTE: the final fallback can return a relative path only when BOTH @p absoluteFn AND
/// @p currentPathFn fail simultaneously. Against the real filesystem this can only occur in a
/// severely broken environment (e.g., an unmounted filesystem or a chroot where the kernel
/// refuses all CWD queries); in that state the application cannot locate any of its assets or
/// config, so the relaxed invariant is acceptable. A warning is emitted so the condition is at
/// least observable at runtime.
///
/// Templated on the callable types (rather than taking AbsolutePathFn/CurrentPathFn directly) so
/// PathService.cpp's real lambdas bind here without first converting to std::function - avoiding
/// that conversion's allocation (and non-noexcept construction) on PathService's construction
/// hot path. The two aliases remain available for callers (tests included) that want a concrete,
/// storable type for the callables.
template<typename AbsoluteFn, typename CurrentPathFn>
[[nodiscard]] inline std::filesystem::path
resolveAbsolutePath(const std::filesystem::path& raw, AbsoluteFn&& absoluteFn, CurrentPathFn&& currentPathFn)
{
    std::error_code ec;
    const std::filesystem::path abs = absoluteFn(raw, ec);
    if (!ec)
    {
        return abs.lexically_normal();
    }

    // absoluteFn failed — try composing with currentPathFn for relative inputs.
    if (raw.is_relative())
    {
        std::error_code cwdEc;
        const std::filesystem::path cwd = currentPathFn(cwdEc);
        if (!cwdEc)
        {
            return (cwd / raw).lexically_normal();
        }
    }

    // Both absoluteFn and currentPathFn failed. The returned path may not be absolute,
    // violating the normal contract of PathService. Log a warning so this degenerate
    // condition is visible; the caller should not rely on the result being absolute.
    //
    // All logging below is best-effort: raw.string() can throw when the path contains
    // characters that cannot be represented in the current locale encoding, and spdlog::warn
    // itself can throw. Each warn call is wrapped in its own try/catch so that no exception can
    // escape from this fallback path.
    try
    {
        const std::string rawStr = raw.empty() ? "<empty>" : raw.string();
        try
        {
            spdlog::warn("PathService: could not resolve absolute path for '{}'; "
                         "both absolute() and current_path() failed. "
                         "Returning lexically-normalized raw path.",
                         rawStr);
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {}
    }
    catch (...)
    {
        try
        {
            spdlog::warn("PathService: could not resolve absolute path (path contains "
                         "non-representable characters); both absolute() and current_path() failed. "
                         "Returning lexically-normalized raw path.");
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {}
    }
    return raw.lexically_normal();
}

} // namespace Core
