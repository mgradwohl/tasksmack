#pragma once

#include <filesystem>

namespace UI
{

/// Locate the runtime assets root directory by probing candidate paths in priority order.
///
/// Supports three deployment layouts:
///   1. Portable / build-dir:    \<exeDir\>/assets/
///      (post-build copy, used during development and for portable installs)
///   2. FHS, binary at prefix:   \<exeDir\>/share/tasksmack/assets/
///      (cmake --install PREFIX, binary lands at prefix root)
///   3. FHS, binary in bin/:     \<exeDir\>/../share/tasksmack/assets/
///      (standard Linux system install via DEB/RPM)
///
/// The probe checks for the existence of the "fonts" subdirectory under each candidate.
/// Returns the first match, or falls back to candidate 1 if none are found.
[[nodiscard]] std::filesystem::path findAssetsDir();

} // namespace UI
