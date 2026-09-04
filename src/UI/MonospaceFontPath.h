#pragma once

// Extracted from UILayer.cpp's anonymous-namespace getMonospaceFontPath() so the
// probe-candidates-in-order logic is directly unit-testable with an injectable existence
// predicate, mirroring UI::selectAssetsDir()/UI::findAssetsDir() (AssetPath.h) - see #770.

#include <filesystem>
#include <functional>
#include <span>

namespace UI
{

/// Locate a monospace font by probing platform-specific candidate paths in priority order
/// (Windows: Consolas, then Cascadia Mono; Linux: DejaVu Sans Mono, then Liberation Mono).
/// Returns an empty path if none of the candidates exist - callers should treat that as
/// "no monospace font available" rather than an error.
[[nodiscard]] std::filesystem::path findMonospaceFontPath();

/// Testable inner implementation: returns the first of \p candidates for which \p probeExists
/// returns true, or an empty path if none match.
[[nodiscard]] std::filesystem::path selectMonospaceFontPath(std::span<const std::filesystem::path> candidates,
                                                            const std::function<bool(const std::filesystem::path&)>& probeExists);

} // namespace UI
