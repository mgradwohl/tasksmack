#pragma once

// Windows' own "same name" for file paths, process names and account names: ordinal (no culture),
// case-insensitive, over UTF-16 (#4309). The shared Platform functions that compare such text take a
// Platform::TextEquals and Windows callers pass this one.

#include <string_view>

namespace Platform::Windows
{

/// Whether UTF-8 @p a and @p b are equal as Windows compares file names: CompareStringOrdinal on the
/// UTF-16 forms, ignoring case, so "É" equals "é". Text that isn't valid UTF-8, is too long for the API
/// or can't be converted (out of memory) is compared exactly instead.
[[nodiscard]] bool equalsOrdinalIgnoreCase(std::string_view a, std::string_view b) noexcept;

} // namespace Platform::Windows
