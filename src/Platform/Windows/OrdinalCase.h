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

} // namespace Platform::Windows
