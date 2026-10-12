#pragma once

// An icon held as an image file (#1800): a Linux desktop entry's PNG, or a packaged Windows app's logo.
// Decoded with the shared stb_image (Platform/StbImage.cpp) into IProcessIconSource's pixels.

#include "Platform/IProcessIconSource.h"

#include <optional>
#include <string>

namespace Platform
{

/// The largest image decoded, per side; a larger one is skipped rather than held in memory.
inline constexpr int MAX_ICON_IMAGE_SIDE_PX = 1024;

/// The pixels of the image (PNG, or another format stb_image reads) at @p utf8Path as straight RGBA;
/// nullopt when it can't be read or decoded, or is larger than MAX_ICON_IMAGE_SIDE_PX on a side.
/// The path is UTF-8 on every platform (opened with _wfopen on Windows).
[[nodiscard]] std::optional<IconPixels> decodeIconImageFile(const std::string& utf8Path);

} // namespace Platform
