#pragma once

// Every colour field of a UI::ColorScheme, for test code that must not miss one: the Theme stub's
// visible scheme (ThemeStub.cpp) and the test that holds it to that (test_ThemeHeader.cpp, #1472).
//
// ColorScheme is `name` (a std::string) followed by nothing but ImVec4s -- the accents array, then
// one ImVec4 per colour field. A field of any other type would break this walk; the static_asserts
// below catch the cheap cases and test_ThemeHeader.cpp checks the count against a known field.

#include "UI/Theme.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <vector>

namespace TestColorScheme
{

static_assert(std::is_same_v<decltype(UI::ColorScheme::accents), std::array<ImVec4, 8>>);
static_assert(std::is_trivially_copyable_v<ImVec4> && alignof(ImVec4) == alignof(float));

/// The bytes from the first colour (accents[0]) to the end of the scheme.
[[nodiscard]] inline std::size_t colorBytes(const UI::ColorScheme& scheme) noexcept
{
    const auto* const first =
        reinterpret_cast<const std::byte*>(scheme.accents.data());          // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* const object = reinterpret_cast<const std::byte*>(&scheme); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return sizeof(UI::ColorScheme) - static_cast<std::size_t>(first - object);
}

/// How many ImVec4 colours the scheme holds (accents included). Trailing padding, if any, is
/// smaller than one ImVec4 and is not counted.
[[nodiscard]] inline std::size_t colorCount(const UI::ColorScheme& scheme) noexcept
{
    return colorBytes(scheme) / sizeof(ImVec4);
}

/// Calls @p visit(ImVec4&) for every colour of @p scheme, in declaration order. The colours are
/// copied out whole, visited, and copied back, with no pointer arithmetic over the scheme's bytes.
template<typename Visit> void forEachColor(UI::ColorScheme& scheme, const Visit& visit)
{
    // Addressed from the scheme itself, not accents.data(), so the copies stay inside one object a
    // compiler can see is large enough (accents alone is only 8 colours).
    auto* const object = reinterpret_cast<std::byte*>(&scheme); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::ptrdiff_t offset =
        reinterpret_cast<std::byte*>(scheme.accents.data()) - object; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    std::byte* const colorsBegin = std::next(object, offset);
    std::vector<ImVec4> colors(colorCount(scheme));
    const std::size_t bytes = colors.size() * sizeof(ImVec4);
    std::memcpy(colors.data(), colorsBegin, bytes);
    for (ImVec4& color : colors)
    {
        visit(color);
    }
    std::memcpy(colorsBegin, colors.data(), bytes);
}

} // namespace TestColorScheme
