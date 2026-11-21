#pragma once

// Windows' own "same name" for file paths, process names and account names: ordinal (no culture),
// case-insensitive, over UTF-16 (#4309). The shared Platform functions that compare such text take a
// Platform::TextEquals and Windows callers pass the UTF-8 one; Windows code that already holds UTF-16 (a
// path, an account name) compares it with the UTF-16 one, so every such comparison is one call (#4368).

#include <string_view>

namespace Platform::Windows
{

/// Whether UTF-8 @p a and @p b are equal as Windows compares file names: CompareStringOrdinal on the
/// UTF-16 forms, ignoring case, so "É" equals "é". Text that isn't valid UTF-8, is too long for the API
/// or can't be converted (out of memory) is compared exactly instead.
[[nodiscard]] bool equalsOrdinalIgnoreCase(std::string_view a, std::string_view b) noexcept;

/// Whether UTF-16 @p a and @p b are equal as Windows compares file names: CompareStringOrdinal, ignoring
/// case, on the text as it is (an unpaired surrogate included). Each is passed with its own length, never
/// read as NUL-terminated; each must be at most INT_MAX units, the API's limit.
[[nodiscard]] bool equalsOrdinalIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept;

/// Whether UTF-16 paths @p a and @p b name the same path as text, the elevated code's one path comparison (#4699):
/// equalsOrdinalIgnoreCase() after any trailing separators ('\' or '/') are dropped, except a root's, which counts. So
/// "C:\a\" equals "C:\a" and "\\server\share\" equals "\\server\share", but "C:\" (the drive's root folder) isn't "C:"
/// (the drive itself, or its current folder): a drive root, "\\?\C:\" and "\\.\C:\" among them, and a path of
/// separators alone keep one separator, and which one doesn't matter. Nothing else is normalized: "." and "..", doubled
/// separators inside and the \\?\ prefix compare as written. Compared on views of the text, nothing copied.
[[nodiscard]] bool samePathIgnoringTrailingSeparators(std::wstring_view a, std::wstring_view b) noexcept;

} // namespace Platform::Windows
