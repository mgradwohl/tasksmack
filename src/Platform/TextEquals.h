#pragma once

// How a pure Platform function decides two pieces of text are the same name (#4309). Windows compares
// file paths and account names ordinal-ignore-case over UTF-16, which is a Windows API call, so the shared
// code takes the comparison as a parameter: Windows callers pass Platform::Windows::equalsOrdinalIgnoreCase
// (Platform/Windows/OrdinalCase.h), other callers an ASCII-fold (Platform::equalsIgnoreCase,
// Platform/AsciiCase.h) or a byte-exact compare. There is no default, so every caller chooses.

#include <string_view>

namespace Platform
{

/// Whether the two texts name the same thing. Must not throw.
using TextEquals = bool (*)(std::string_view, std::string_view) noexcept;

} // namespace Platform
