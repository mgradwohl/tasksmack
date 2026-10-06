#pragma once

// Tabular (fixed-width) digits for live numbers (#1201). Inter's default digits are proportional --
// a "1" is narrower than a "0" -- so a value that changes every second moved the text around it.
// UILayer gives each body font a digits-only first source whose digits all advance by the widest
// digit's width (ImFontConfig::GlyphMinAdvanceX); every other glyph comes from the font merged after
// it. The measuring is here, apart from the font atlas, so it can be unit-tested.

#include <imgui.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace UI
{

/// Every codepoint except '0'-'9', as ImFontConfig::GlyphExcludeRanges pairs: a font source given
/// these supplies only the digits. Zero-terminated, as ImGui requires.
inline constexpr std::array<ImWchar, 5> NON_DIGIT_GLYPH_RANGES = {
    ImWchar{0x0001},
    ImWchar{'0' - 1},
    ImWchar{'9' + 1},
    ImWchar{IM_UNICODE_CODEPOINT_MAX},
    ImWchar{0},
};

/// The widest of a font's digits '0'-'9', as a fraction of the height ImGui sizes fonts by.
///
/// ImGui's FreeType loader sizes a font so its ascender-to-descender height is the requested pixel
/// size (FT_SIZE_REQUEST_TYPE_REAL_DIM), not its em, so the ratio is taken against that height:
/// multiplied by a font's SizePixels it gives the digit's advance in pixels at that size.
///
/// @param fontData  The font file's bytes (TrueType/OpenType).
/// @return nullopt if FreeType can't read the font or it has no digits.
[[nodiscard]] std::optional<float> widestDigitAdvanceRatio(std::span<const std::byte> fontData);

/// The GlyphMinAdvanceX that makes every digit as wide as the widest at `fontSizePx`: rounded up to
/// a whole pixel, since hinting can round a digit's own advance up. 0 (no minimum) for a ratio or
/// size that isn't positive and finite.
[[nodiscard]] float tabularDigitAdvancePx(float widestDigitRatio, float fontSizePx) noexcept;

} // namespace UI
