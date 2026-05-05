#pragma once

#include <filesystem>
#include <functional>

namespace UI
{

/// Locate the runtime assets root directory by probing candidate paths in priority order.
///
/// Supports four deployment layouts:
///   1. Developer / debug helper: \<exeDir\>/bin/assets/
///      (when the executable directory is a build root and assets are copied under bin/)
///   2. Portable / build-dir:      \<exeDir\>/assets/
///      (post-build copy, used during development and for portable installs)
///   3. FHS, binary at prefix:     \<exeDir\>/share/\<project_name_lower\>/assets/
///      (cmake --install PREFIX, binary lands at prefix root)
///   4. FHS, binary in bin/:       \<exeDir\>/../share/\<project_name_lower\>/assets/
///      (standard Linux system install via DEB/RPM)
///
/// The probe checks for the existence of the "fonts" subdirectory under each candidate.
/// Returns the first match, or falls back to \<exeDir\>/assets if none are found.
[[nodiscard]] std::filesystem::path findAssetsDir();

/// Testable inner implementation: selects the assets directory given a custom executable
/// directory and an existence predicate. Probes candidate paths in priority order and
/// returns the first for which \p probeExists returns true, or falls back to \<exeDir\>/assets.
[[nodiscard]] std::filesystem::path selectAssetsDir(const std::filesystem::path& exeDir,
                                                    const std::function<bool(const std::filesystem::path&)>& probeExists);

} // namespace UI
