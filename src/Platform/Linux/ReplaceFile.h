#pragma once

/// @file ReplaceFile.h
/// @brief A file replaced whole, safely (#3771): written to a staging file beside it and renamed over it, so a reader
/// never sees half a file and a failure leaves the old one.

#include <filesystem>
#include <string>
#include <string_view>

namespace Platform::ReplaceFile
{

/// Replaces @p path's contents with @p text, or makes the file. The staging file beside it has an unpredictable name
/// and is created with O_EXCL and O_NOFOLLOW, so nothing planted there (a link to another file, a file of someone
/// else's) is followed or written; it is flushed to disk (fsync) before the rename. A replaced file keeps its
/// permissions; a new one gets the umask's, as any file the user makes. A link at @p path itself is replaced by the
/// file, as a rename does. On failure the staging file is removed and @p path is left as it was.
/// @return Empty on success, else what failed ("couldn't write <staging>: No space left on device").
[[nodiscard]] std::string replace(const std::filesystem::path& path, std::string_view text);

} // namespace Platform::ReplaceFile
